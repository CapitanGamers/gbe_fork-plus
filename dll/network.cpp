/* Copyright (C) 2019 Mr Goldberg
   This file is part of the Goldberg Emulator

   The Goldberg Emulator is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 3 of the License, or (at your option) any later version.

   The Goldberg Emulator is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with the Goldberg Emulator; if not, see
   <http://www.gnu.org/licenses/>.  */

#include "dll/network.h"
#include "dll/dll.h"
#include "common_helpers/peer_endpoints.hpp"

#include <cerrno>

#define MAX_BROADCASTS 16
static int number_broadcasts = -1;
static IP_PORT broadcasts[MAX_BROADCASTS];
static uint32_t lower_range_ips[MAX_BROADCASTS];
static uint32_t upper_range_ips[MAX_BROADCASTS];

#define BROADCAST_INTERVAL 5.0
#define HEARTBEAT_TIMEOUT 20.0
#define USER_TIMEOUT 20.0

#define MAX_UDP_SIZE 16384

#if defined(STEAM_WIN32)

static void get_broadcast_info(uint16 port)
{
    number_broadcasts = 0;

    ULONG ulOutBufLen = 16384;
    IP_ADAPTER_ADDRESSES *pAdapterInfo = (IP_ADAPTER_ADDRESSES *)malloc(ulOutBufLen);
    if (pAdapterInfo == nullptr) {
        return;
    }

    ULONG ret = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, nullptr, pAdapterInfo, &ulOutBufLen);

    if (ret == ERROR_BUFFER_OVERFLOW) {
        free(pAdapterInfo);
        pAdapterInfo = (IP_ADAPTER_ADDRESSES *)malloc(ulOutBufLen);
        if (pAdapterInfo == nullptr) {
            return;
        }

        ret = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, nullptr, pAdapterInfo, &ulOutBufLen);
    }

    if (ret == NO_ERROR) {
        for (IP_ADAPTER_ADDRESSES *pAdapter = pAdapterInfo; pAdapter; pAdapter = pAdapter->Next) {
            if (pAdapter->OperStatus == IfOperStatusUp &&
                pAdapter->IfType != IF_TYPE_SOFTWARE_LOOPBACK &&
                pAdapter->FirstUnicastAddress &&
                pAdapter->FirstUnicastAddress->OnLinkPrefixLength <= 32) {
                sockaddr_in *addr_ptr = (sockaddr_in *)(pAdapter->FirstUnicastAddress->Address.lpSockaddr);
                uint32 iface_ip = ntohl(addr_ptr->sin_addr.s_addr);

                ULONG prefix = pAdapter->FirstUnicastAddress->OnLinkPrefixLength;
                uint32 subnet_mask = (prefix == 0) ? 0 : (0xFFFFFFFFu << (32 - prefix));

                IP_PORT *ip_port = &broadcasts[number_broadcasts];
                uint32 broadcast_ip = htonl(iface_ip | ~subnet_mask);
                ip_port->ip = broadcast_ip;
                ip_port->port = port;
                lower_range_ips[number_broadcasts] = htonl(iface_ip & subnet_mask);
                upper_range_ips[number_broadcasts] = broadcast_ip;
                number_broadcasts++;

                if (number_broadcasts >= MAX_BROADCASTS) {
                    break;
                }
            }
        }
    }

    free(pAdapterInfo);
}

#elif defined(__linux__)

static void get_broadcast_info(uint16 port)
{
    /* Not sure how many platforms this will run on,
     * so it's wrapped in __linux for now.
     * Definitely won't work like this on Windows...
     */
    number_broadcasts = 0;
    sock_t sock = 0;

    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        return;

    /* Configure ifconf for the ioctl call. */
    struct ifreq i_faces[MAX_BROADCASTS];
    memset(i_faces, 0, sizeof(struct ifreq) * MAX_BROADCASTS);

    struct ifconf ifconf;
    ifconf.ifc_buf = (char *)i_faces;
    ifconf.ifc_len = sizeof(i_faces);

    if (ioctl(sock, SIOCGIFCONF, &ifconf) < 0) {
        close(sock);
        return;
    }

    /* ifconf.ifc_len is set by the ioctl() to the actual length used;
     * on usage of the complete array the call should be repeated with
     * a larger array, not done (640kB and 16 interfaces shall be
     * enough, for everybody!)
     */
    int i, count = ifconf.ifc_len / sizeof(struct ifreq);

    for (i = 0; i < count; i++) {
        /* there are interfaces with are incapable of broadcast */
        if (ioctl(sock, SIOCGIFBRDADDR, &i_faces[i]) < 0)
            continue;

        /* moot check: only AF_INET returned (backwards compat.) */
        if (i_faces[i].ifr_broadaddr.sa_family != AF_INET)
            continue;

        struct sockaddr_in *sock4 = (struct sockaddr_in *)&i_faces[i].ifr_broadaddr;

        if (number_broadcasts >= MAX_BROADCASTS) {
            close(sock);
            return;
        }

        IP_PORT *ip_port = &broadcasts[number_broadcasts];
        ip_port->ip = sock4->sin_addr.s_addr;

        if (ip_port->ip == 0) {
            continue;
        }

        ip_port->port = port;
        number_broadcasts++;
    }

    close(sock);
}
#endif

static bool is_socket_valid(sock_t sock)
{
#if defined(STEAM_WIN32)
    return sock != invalid_socket;
#else
    return sock >= 0;
#endif
}

static bool is_tcp_socket_valid(struct TCP_Socket &socket)
{
    return is_socket_valid(socket.sock);
}

static int set_socket_nonblocking(sock_t sock)
{
#if defined(STEAM_WIN32)
    u_long mode = 1;
    return (ioctlsocket(sock, FIONBIO, &mode) == 0);
#else
    return (fcntl(sock, F_SETFL, O_NONBLOCK, 1) == 0);
#endif
}

static bool disable_nagle(sock_t sock)
{
    int set = 1;
    return (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char *)&set, sizeof(set)) == 0);
}

static void kill_socket(sock_t sock)
{
#if defined(STEAM_WIN32)
    closesocket(sock);
#else
    close(sock);
#endif
}

static void kill_tcp_socket(struct TCP_Socket &socket)
{
    if (is_socket_valid(socket.sock)) {
        kill_socket(socket.sock);
    }

    socket = TCP_Socket();
}

static bool initialed;
static void run_at_startup()
{
    if (initialed) {
        return;
    }
#if defined(STEAM_WIN32)
    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != NO_ERROR) {
        PRINT_DEBUG("Networking WSAStartup error");
        return;
    }

    for (int i = 0; i < 10; ++i) {
        //hack: the game Full Mojo Rampage calls WSACleanup on startup so we call WSAStartup a few times so it doesn't get deallocated.
        WSADATA wsaData{};
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != NO_ERROR) {
            PRINT_DEBUG("Networking WSAStartup error");
            return;
        }
    }
    PRINT_DEBUG("Networking WSAStartup success!");
#else

#endif
    initialed = true;
}

static int get_last_error()
{
#if defined(STEAM_WIN32)
    return WSAGetLastError();
#else
    return errno;
#endif
}

//Reset the wsa error code so that games don't get confused.
static void reset_last_error()
{
#if defined(STEAM_WIN32)
    WSASetLastError(0);
#else
    return;
#endif
}

static int send_packet_to(sock_t sock, IP_PORT ip_port, char *data, unsigned long length)
{
    PRINT_DEBUG("send: %lu %hhu.%hhu.%hhu.%hhu:%hu", length, ((unsigned char *)&ip_port.ip)[0], ((unsigned char *)&ip_port.ip)[1], ((unsigned char *)&ip_port.ip)[2], ((unsigned char *)&ip_port.ip)[3], htons(ip_port.port));
    struct sockaddr_storage addr;
    struct sockaddr_in *addr4 = (struct sockaddr_in *)&addr;

#if defined(STEAM_WIN32) 
    int addrsize = (int)sizeof(struct sockaddr_in); 
 #else 
    socklen_t addrsize = (socklen_t)sizeof(struct sockaddr_in);
 #endif 
    addr4->sin_family = AF_INET;
    addr4->sin_addr.s_addr = ip_port.ip;
    addr4->sin_port = ip_port.port;

    return sendto(sock, data, length, 0, (struct sockaddr *)&addr, addrsize);
}

static int receive_packet(sock_t sock, IP_PORT *ip_port, char *data, unsigned long max_length)
{
    struct sockaddr_storage addr{};
#if defined(STEAM_WIN32)
    int addrlen = sizeof(addr);
#else
    socklen_t addrlen = sizeof(addr);
#endif

    int ret = recvfrom(sock, (char *) data, max_length, 0, (struct sockaddr *)&addr, &addrlen);
    if (ret >= 0) {
        struct sockaddr_in *addr_in = (struct sockaddr_in *)&addr;
        ip_port->ip = addr_in->sin_addr.s_addr;
        ip_port->port = addr_in->sin_port;
        return ret;
    }

    return -1;
}

int Networking::receive_discovery_packet(IP_PORT &ip_port, char *data, unsigned long capacity, bool &broadcast)
{
    broadcast = false;
#if defined(STEAM_WIN32)
    if (udp_receive_message) {
        sockaddr_in sender{};
        WSABUF buffer{capacity, data};
        WSAMSG message{};
        message.name = reinterpret_cast<sockaddr *>(&sender);
        message.namelen = sizeof(sender);
        message.lpBuffers = &buffer;
        message.dwBufferCount = 1;
        DWORD received = 0;

        if (udp_receive_message(udp_socket, &message, &received, nullptr, nullptr) == SOCKET_ERROR) return -1;
        broadcast = (message.dwFlags & (MSG_BCAST | MSG_MCAST)) != 0;
        ip_port = {sender.sin_addr.s_addr, sender.sin_port};
        return static_cast<int>(received);
    }
#endif
    return receive_packet(udp_socket, &ip_port, data, capacity);
}

static void send_broadcasts(sock_t sock, uint16 port, char *data, unsigned long length)
{
    static std::chrono::high_resolution_clock::time_point last_get_broadcast_info;
    if (number_broadcasts < 0 || check_timedout(last_get_broadcast_info, 60.0)) {
        PRINT_DEBUG("get_broadcast_info");
        get_broadcast_info(port);
        set_whitelist_ips(lower_range_ips, upper_range_ips, static_cast<unsigned int>(number_broadcasts));
        last_get_broadcast_info = std::chrono::high_resolution_clock::now();
    }

    IP_PORT main_broadcast;
    main_broadcast.ip = INADDR_BROADCAST;
    main_broadcast.port = port;
    send_packet_to(sock, main_broadcast, data, length);

    for (int i = 0; i < number_broadcasts; i++) {
        IP_PORT ip_port = broadcasts[i];
        ip_port.port = port;
        send_packet_to(sock, ip_port, data, length);
    }
}

static void buffers_set(sock_t sock)
{
    int n = 1024 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (char *)&n, sizeof(n));
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (char *)&n, sizeof(n));
}

static bool bind_socket(sock_t sock, uint16 port)
{
    struct sockaddr_storage addr = {};
    struct sockaddr_in *addr4 = (struct sockaddr_in *)&addr;
#if defined(STEAM_WIN32) 
    int addrsize = (int)sizeof(struct sockaddr_in);
 #else 
    socklen_t addrsize = (socklen_t)sizeof(struct sockaddr_in);
 #endif 
    addr4->sin_family = AF_INET;
    addr4->sin_port = htons(port);
    addr4->sin_addr.s_addr = 0;

    return !bind(sock, (struct sockaddr *)&addr, addrsize);
}

static bool socket_reuseaddr(sock_t sock)
{
    int set = 1;
    return (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *)&set, sizeof(set)) == 0);
}

static void connect_socket(sock_t sock, IP_PORT ip_port)
{
    struct sockaddr_storage addr;
    struct sockaddr_in *addr4 = (struct sockaddr_in *)&addr;
#if defined(STEAM_WIN32) 
    int addrsize = (int)sizeof(struct sockaddr_in); 
 #else 
    socklen_t addrsize = (socklen_t)sizeof(struct sockaddr_in);
 #endif 
    addr4->sin_family = AF_INET;
    addr4->sin_addr.s_addr = ip_port.ip;
    addr4->sin_port = ip_port.port;

    connect(sock, (struct sockaddr *)&addr, addrsize);
}

unsigned int receive_buffer_amount(sock_t sock)
{
#if defined(STEAM_WIN32)
    unsigned long count = 0;
    ioctlsocket(sock, FIONREAD, &count);
#else
    int count = 0;
    ioctl(sock, FIONREAD, &count);
#endif

    return count;
}


static void send_tcp_pending(struct TCP_Socket &socket)
{
    size_t buf_size = socket.send_buffer.size();
    if (buf_size == 0) return;

    int len = send(socket.sock, &(socket.send_buffer[0]), static_cast<int>(buf_size), MSG_NOSIGNAL);
    if (len <= 0) return;

    socket.send_buffer.erase(socket.send_buffer.begin(), socket.send_buffer.begin() + len);
}

static void send_buffer_tcp(struct TCP_Socket &socket, Common_Message *msg)
{
    uint32 size = static_cast<uint32>(msg->ByteSizeLong()), old_size = static_cast<uint32>(socket.send_buffer.size());
    socket.send_buffer.resize(old_size + sizeof(uint32) + size);
    memcpy(&(socket.send_buffer[old_size]), &size, sizeof(size));
    msg->SerializeToArray(&(socket.send_buffer[old_size + sizeof(uint32)]), size);

    send_tcp_pending(socket);
}

static unsigned long peek_buffer_tcp(struct TCP_Socket &socket)
{
    uint32 length;
    if (socket.recv_buffer.size() < sizeof(length)) return 0;

    memcpy(&length, &(socket.recv_buffer[0]), sizeof(length));
    if (length > socket.recv_buffer.size() - sizeof(length)) return 0;

    return length;
}

static bool unbuffer_tcp(struct TCP_Socket &socket, Common_Message *msg)
{
    uint32 l = peek_buffer_tcp(socket);
    if (!l) {
        return false;
    }

    if (msg->ParseFromArray(&(socket.recv_buffer[sizeof(uint32)]), l)) {
        socket.recv_buffer.erase(socket.recv_buffer.begin(), socket.recv_buffer.begin() + sizeof(l) + l);
        return true;
    } else {
        PRINT_DEBUG("BAD TCP DATA %u %zu %zu %hhu", l, socket.recv_buffer.size(), sizeof(uint32), *((char *)&(socket.recv_buffer[sizeof(uint32)])));
        kill_tcp_socket(socket);
    }

    return false;
}

static bool recv_tcp(struct TCP_Socket &socket)
{
    if (!is_tcp_socket_valid(socket)) return false;

    const auto pending = receive_buffer_amount(socket.sock);
    if (!pending) return false;

    const auto requested = (std::min)(pending, 65536u);
    const auto old_size = socket.recv_buffer.size();
    socket.recv_buffer.resize(old_size + requested);
    const auto length = recv(socket.sock, socket.recv_buffer.data() + old_size, requested, MSG_NOSIGNAL);
    socket.recv_buffer.resize(old_size + (length > 0 ? length : 0));
    if (length <= 0) return false;

    socket.received_data = true;
    return true;
}

static void socket_timeouts(struct TCP_Socket &socket, double extra_time)
{
    if (check_timedout(socket.last_heartbeat_sent, HEARTBEAT_TIMEOUT / 2.0)) {
        Common_Message msg;
        msg.set_allocated_low_level(new Low_Level());
        msg.mutable_low_level()->set_type(Low_Level::HEARTBEAT);
        send_buffer_tcp(socket, &msg);
        socket.last_heartbeat_sent = std::chrono::high_resolution_clock::now();
    }

    if (check_timedout(socket.last_heartbeat_received, HEARTBEAT_TIMEOUT + extra_time)) {
        kill_tcp_socket(socket);
        PRINT_DEBUG("TCP SOCKET HEARTBEAT TIMEOUT");
    }
}

std::set<IP_PORT> Networking::resolve_ip(std::string dns)
{
    run_at_startup();
    std::set<IP_PORT> ips;
    struct addrinfo* result = NULL;

    uint16 port = 0;

    auto port_sindex = dns.find(":", 0);
    if (port_sindex != std::string::npos) {
        port = (uint16)atoi(dns.substr(port_sindex + 1).c_str());
        dns = dns.substr(0, port_sindex);
    }

    if (getaddrinfo(dns.c_str(), NULL, NULL, &result) == 0) {
        for (struct addrinfo *res = result; res != NULL; res = res->ai_next) {
            // cast to size_t because on Linux 'ai_addrlen' is defined as unsigned int and clang complains
            PRINT_DEBUG("%zu %u", (size_t)res->ai_addrlen, res->ai_family);
            if (res->ai_family == AF_INET) {
                struct sockaddr_in *ipv4 = (struct sockaddr_in *)res->ai_addr;
                uint32 ip;
                memcpy(&ip, &ipv4->sin_addr, sizeof(ip));
                IP_PORT addr;
                addr.ip = ntohl(ip);
                addr.port = port;
                ips.insert(addr);
            }
        }
    }

    if (result)
        freeaddrinfo(result);
    return ips;
}

void Networking::do_callbacks_message(Common_Message *msg)
{
    if (msg->has_network() || msg->has_network_old()) {
        PRINT_DEBUG("has_network");
        run_callbacks(CALLBACK_ID_NETWORKING, msg);
    }

    if (msg->has_lobby()) {
        PRINT_DEBUG("has_lobby");
        run_callbacks(CALLBACK_ID_LOBBY, msg);
    }

    if (msg->has_lobby_messages()) {
        PRINT_DEBUG("has_lobby_messages");
        run_callbacks(CALLBACK_ID_LOBBY, msg);
    }

    if (msg->has_gameserver()) {
        PRINT_DEBUG("has_gameserver");
        run_callbacks(CALLBACK_ID_GAMESERVER, msg);
    }

    if (msg->has_friend_()) {
        PRINT_DEBUG("has_friend_");
        run_callbacks(CALLBACK_ID_FRIEND, msg);
    }

    if (msg->has_auth_ticket()) {
        PRINT_DEBUG("has_auth_ticket");
        run_callbacks(CALLBACK_ID_AUTH_TICKET, msg);
    }

    if (msg->has_friend_messages()) {
        PRINT_DEBUG("has_friend_messages");
        run_callbacks(CALLBACK_ID_FRIEND_MESSAGES, msg);
    }

    if (msg->has_networking_sockets()) {
        PRINT_DEBUG("has_networking_sockets");
        run_callbacks(CALLBACK_ID_NETWORKING_SOCKETS, msg);
    }

    if (msg->has_steam_messages()) {
        PRINT_DEBUG("has_steam_messages");
        run_callbacks(CALLBACK_ID_STEAM_MESSAGES, msg);
    }

    if (msg->has_networking_messages()) {
        PRINT_DEBUG("has_networking_messages");
        run_callbacks(CALLBACK_ID_NETWORKING_MESSAGES, msg);
    }

    if (msg->has_gameserver_stats_messages()) {
        PRINT_DEBUG("has_gameserver_stats");
        run_callbacks(CALLBACK_ID_GAMESERVER_STATS, msg);
    }
    
    if (msg->has_leaderboards_messages()) {
        PRINT_DEBUG("has_leaderboards_messages");
        run_callbacks(CALLBACK_ID_LEADERBOARDS_STATS, msg);
    }

    if (msg->has_steam_user_stats_messages()) {
        PRINT_DEBUG("has_steam_user_stats_messages");
        run_callbacks(CALLBACK_ID_USER_STATS, msg);
    }

    if (msg->has_gameserver_items_messages()) {
        PRINT_DEBUG("has_gameserver_items_messages");
        run_callbacks(CALLBACK_ID_GAMESERVER_ITEMS, msg);
    }
}

bool Networking::handle_tcp(Common_Message *msg, struct TCP_Socket &socket)
{
    socket.last_heartbeat_received = std::chrono::high_resolution_clock::now();
    if (msg->has_low_level()) {
        switch (msg->low_level().type()) {
            case Low_Level::DISCONNECT:
                
                break;
            case Low_Level::HEARTBEAT:
                //socket.last_heartbeat_received = std::chrono::high_resolution_clock::now();
                break;
        }
    }

    do_callbacks_message(msg);
    return true;
}

struct Connection *Networking::find_connection(CSteamID search_id, uint32 appid)
{
    auto conn = std::find_if(connections.begin(), connections.end(), [&search_id, appid](struct Connection const& conn) { 
        if (appid && (conn.appid != appid)) return false;

        for (const auto &id: conn.ids) {
            if (search_id == id) return true;
        }

        return false;
    });

    if (connections.end() != conn)
        return &(*conn);

    return nullptr;
}

bool Networking::add_id_connection(struct Connection *connection, CSteamID steam_id)
{
    if (!connection) return false;

    auto id = std::find(connection->ids.begin(), connection->ids.end(), steam_id);
    if (id != connection->ids.end())
        return false;

    PRINT_DEBUG("ADDED NEW USER ID %llu", (uint64)steam_id.ConvertToUint64());
    connection->ids.push_back(steam_id);
    if (connection->connected) {
        run_callback_user(steam_id, true, connection->appid);
    }

    return true;
}

struct Connection *Networking::new_connection(CSteamID search_id, uint32 appid)
{
    Connection *conn = find_connection(search_id, appid);
    if (conn && conn->appid == appid) return NULL;

    struct Connection connection;
    connection.ids.push_back(search_id);
    connection.appid = appid;
    connection.last_received = std::chrono::high_resolution_clock::now();

    PRINT_DEBUG("ADDED ID %llu", (uint64)search_id.ConvertToUint64());
    connections.push_back(connection);
    return &(connections[connections.size() - 1]);
}

bool Networking::handle_announce(Common_Message *msg, IP_PORT ip_port)
{
    const CSteamID source_id(msg->source_id());
    if (std::find(ids.begin(), ids.end(), source_id) != ids.end()) return false;

    Connection *conn = find_connection((uint64)msg->source_id(), msg->announce().appid());
    if (!conn || conn->appid != msg->announce().appid()) {
        conn = new_connection((uint64)msg->source_id(), msg->announce().appid());
        if (!conn) return false;
        PRINT_DEBUG("new connection created: user %llu, appid %u", (uint64)msg->source_id(), msg->announce().appid());
    }

    PRINT_DEBUG("Handle Announce: %u, " "%" PRIu64 ", %u, %u", conn->appid, msg->source_id(), msg->announce().appid(), msg->announce().type());
    conn->announce_endpoint = ip_port;
    conn->tcp_ip_port = ip_port;
    conn->tcp_ip_port.port = htons(msg->announce().tcp_port());
    conn->appid = msg->announce().appid();

    for (int i = 0; i < msg->announce().ids_size(); ++i) {
        add_id_connection(conn, (uint64) msg->announce().ids(i));
    }

    for (int i = 0; i < msg->announce().peers_size(); ++i) {
        CSteamID search_id((uint64)msg->announce().peers(i).id());
        auto id_temp = std::find(ids.begin(), ids.end(), search_id);
        if (id_temp != ids.end()) {
            own_ip = ntohl(msg->announce().peers(i).ip());
            continue;
        }
        if (!automatic_discovery.load()) continue;

        Connection *conn = find_connection((uint64)msg->announce().peers(i).id(), msg->announce().peers(i).appid());
        PRINT_DEBUG("%p %u %u " "%" PRIu64 "", conn, conn ? conn->appid : (uint32)0, msg->announce().peers(i).appid(), msg->announce().peers(i).id());
        if (!conn || conn->appid != msg->announce().peers(i).appid()) {
            Common_Message msg_ = create_announce(true);

            size_t size = msg_.ByteSizeLong();
            char *buffer = new char[size];
            msg_.SerializeToArray(buffer, static_cast<int>(size));
            IP_PORT ipp;
            ipp.ip = msg->announce().peers(i).ip();
            ipp.port = htons(msg->announce().peers(i).udp_port());
            send_packet_to(udp_socket, ipp, buffer, static_cast<unsigned long>(size));
            delete[] buffer;
        }
    }

    conn->last_received = std::chrono::high_resolution_clock::now();

    if (msg->announce().type() == Announce::PING) {
        Common_Message msg = create_announce(false, conn);
        size_t size = msg.ByteSizeLong(); 
        char *buffer = new char[size];
        msg.SerializeToArray(buffer, static_cast<int>(size));
        send_packet_to(udp_socket, ip_port, buffer, static_cast<unsigned long>(size));
        delete[] buffer;

        //send ping packet if not pinged
        if (!conn->udp_pinged) {
            Common_Message msg = create_announce(true);
            size_t size = msg.ByteSizeLong(); 
            char *buffer = new char[size];
            msg.SerializeToArray(buffer, static_cast<int>(size));
            send_packet_to(udp_socket, ip_port, buffer, static_cast<unsigned long>(size));
            delete[] buffer;
        }
    } else if (msg->announce().type() == Announce::PONG) {
        conn->udp_ip_port = ip_port;
        conn->udp_pinged = true;
    }

    return true;
}

bool Networking::handle_low_level_udp(Common_Message *msg, IP_PORT ip_port)
{
    //TODO: connection appid
    struct Connection *connection = find_connection((uint64)msg->source_id());
    if (!connection)
        return false;

    switch (msg->low_level().type()) {
        case Low_Level::DISCONNECT:
            
            break;
        case Low_Level::HEARTBEAT:
            
            break;
    }

    return false;
}

#define NUM_TCP_WAITING 128

Networking::Networking(CSteamID id, uint32 appid, uint16 port, std::set<IP_PORT> *custom_broadcasts, bool disable_sockets)
    : peer_file(steamem_peers::default_file()),
      automatic_discovery(get_env_variable("STEAMEM_AUTOMATIC_DISCOVERY") == "1")
{
    tcp_port = udp_port = port;
    own_ip = 0x7F000001;
    last_run = std::chrono::high_resolution_clock::now();
    this->appid = appid;

    if (custom_broadcasts) {
        for (const auto &addr : *custom_broadcasts) {
            const IP_PORT target{htonl(addr.ip), htons(addr.port ? addr.port : port)};
            peer_targets[target].legacy = true;
        }
    }

    refresh_peer_file();
    if (disable_sockets) return;

    run_at_startup();
    sock_t sock = static_cast<sock_t>(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    PRINT_DEBUG("UDP socket: %llu", static_cast<unsigned long long>(sock));
    if (is_socket_valid(sock) && set_socket_nonblocking(sock)) {
        int broadcast = 1;
        setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (char *)&broadcast, sizeof(broadcast));
        //socket_reuseaddr(sock);

        buffers_set(sock);
        for (unsigned i = 0; i < 1000 && port + i <= UINT16_MAX; ++i) {
            udp_port = port + i;
            if (bind_socket(sock, udp_port)) {
                PRINT_DEBUG("UDP successful");
                udp_socket = sock;
#if defined(STEAM_WIN32)
                GUID receive_id = WSAID_WSARECVMSG;
                DWORD bytes = 0;
                if (WSAIoctl(udp_socket, SIO_GET_EXTENSION_FUNCTION_POINTER, &receive_id, sizeof(receive_id),
                             &udp_receive_message, sizeof(udp_receive_message), &bytes, nullptr, nullptr) == SOCKET_ERROR) {
                    udp_receive_message = nullptr;
                }
#endif
                break;
            } else {
                //clear the error
                int error = 0;
                socklen_t len = sizeof(error);
                getsockopt(sock, SOL_SOCKET, SO_ERROR, (char *)&error, &len);
            }
        }

        if (!is_socket_valid(udp_socket)) {
            PRINT_DEBUG("UDP: could not bind socket");
        }
    } else {
        PRINT_DEBUG("UDP: could not initialize %i", get_last_error());
    }
    if (is_socket_valid(sock) && !is_socket_valid(udp_socket)) kill_socket(sock);

    sock = static_cast<sock_t>(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    PRINT_DEBUG("TCP socket: %llu", static_cast<unsigned long long>(sock));
    if (is_socket_valid(sock) && set_socket_nonblocking(sock)) {
        buffers_set(sock);
        //socket_reuseaddr(sock);

        for (unsigned i = 0; i < 1000 && port + i <= UINT16_MAX; ++i) {
            tcp_port = port + i;
            if (bind_socket(sock, tcp_port)) {
                if ((listen(sock, NUM_TCP_WAITING) == 0)) {
                    PRINT_DEBUG("TCP successful");
                    tcp_socket = sock;
                    break;
                } else {
                    int error = 0;
                    socklen_t len = sizeof(error);
                    getsockopt(sock, SOL_SOCKET, SO_ERROR, (char *)&error, &len);
                    PRINT_DEBUG("TCP listen error %i", error);
                }
            } else {
                int error = 0;
                socklen_t len = sizeof(error);
                getsockopt(sock, SOL_SOCKET, SO_ERROR, (char *)&error, &len);
            }
        }

        if (!is_socket_valid(tcp_socket)) {
            PRINT_DEBUG("TCP: could not bind or listen");
        }
    } else {
        PRINT_DEBUG("TCP: could not initialize %i", get_last_error());
    }
    if (is_socket_valid(sock) && !is_socket_valid(tcp_socket)) kill_socket(sock);

    if (curl_global_init(CURL_GLOBAL_ALL) == 0) {
        PRINT_DEBUG("CURL successful");
    } else {
        PRINT_DEBUG("CURL: could not initialize");
    }
 
    if (is_socket_valid(udp_socket) && is_socket_valid(tcp_socket)) {
        PRINT_DEBUG("Networking initialized successfully on udp: %u tcp: %u", udp_port, tcp_port);
        enabled = true;
    }

    PRINT_DEBUG("ADDED ID %llu", (uint64)id.ConvertToUint64());
    ids.push_back(id);

    reset_last_error();
}

Networking::~Networking()
{
    for (auto &c : connections) {
        kill_tcp_socket(c.tcp_socket_incoming);
        kill_tcp_socket(c.tcp_socket_outgoing);
    }

    for (auto &c : accepted) {
        kill_tcp_socket(c);
    }

    if (is_socket_valid(udp_socket)) kill_socket(udp_socket);
    if (is_socket_valid(tcp_socket)) kill_socket(tcp_socket);
    if (is_socket_valid(query_socket)) kill_socket(query_socket);

    curl_global_cleanup();
}

Common_Message Networking::create_announce(bool request, const Connection *recipient)
{
    Announce *announce = new Announce();
    PRINT_DEBUG("ids length %zu", ids.size());
    if (request) {
        announce->set_type(Announce::PING);
    } else {
        announce->set_type(Announce::PONG);

        const auto add_endpoint = [&](const Connection &conn, const IP_PORT &endpoint) {
            if (conn.ids.empty()) return;
            Announce_Other_Peers *peer = announce->add_peers();
            peer->set_id(conn.ids.front().ConvertToUint64());
            peer->set_ip(endpoint.ip);
            peer->set_udp_port(ntohs(endpoint.port));
            peer->set_appid(conn.appid);
        };
        if (automatic_discovery.load()) {
            for (const auto &conn : connections) {
                if (conn.udp_pinged) add_endpoint(conn, conn.udp_ip_port);
            }
        } else if (recipient) {
            // Give a directly connected client its observed address without sharing a peer list.
            add_endpoint(*recipient, recipient->announce_endpoint);
        }
    }

    announce->set_tcp_port(tcp_port);
    announce->set_appid(this->appid);
    for (auto &id : ids) announce->add_ids(id.ConvertToUint64());
    Common_Message msg;
    msg.set_allocated_announce(announce);
    msg.set_source_id(ids[0].ConvertToUint64());
    return msg;
}

void Networking::send_announce_broadcasts()
{
    last_broadcast = std::chrono::high_resolution_clock::now();
    Common_Message msg = create_announce(true);

    const auto size = msg.ByteSizeLong();
    std::vector<char> buffer(size);
    if (!msg.SerializeToArray(buffer.data(), static_cast<int>(size))) return;

    const bool include_automatic = automatic_discovery.load();
    if (include_automatic) {
        for (uint16 i = DEFAULT_PORT; i < DEFAULT_PORT + NUM_QUERY_PORTS; i++) {
            send_broadcasts(udp_socket, htons(i), buffer.data(), static_cast<unsigned long>(size));
        }
        if (udp_port < DEFAULT_PORT || udp_port >= DEFAULT_PORT + NUM_QUERY_PORTS) {
            send_broadcasts(udp_socket, htons(udp_port), buffer.data(), static_cast<unsigned long>(size));
        }
    }

    // Send each configured destination once per announcement, independently of LAN broadcast support.
    const std::lock_guard lock(peer_mutex);
    std::set<uint32_t> peer_ips;
    for (const auto &entry : peer_targets) peer_ips.insert(ntohl(entry.first.ip));
    set_discovery_peer_ips(std::move(peer_ips));
    for (auto &[endpoint, target] : peer_targets) {
        if (!include_automatic && !target.editable) continue;
        const auto sent = send_packet_to(udp_socket, endpoint, buffer.data(), static_cast<unsigned long>(size));
        target.attempted = true;
        target.socket_error = sent < 0 ? get_last_error() : 0;
    }
    PRINT_DEBUG("sent discovery messages, automatic discovery: %i", include_automatic);
}

void Networking::Run()
{
    std::chrono::high_resolution_clock::time_point now = std::chrono::high_resolution_clock::now();
    double time_extra = std::chrono::duration_cast<std::chrono::duration<double>>(now - last_run).count();
    last_run = now;

    const bool peers_changed = refresh_peer_file();
    if (!enabled || ids.empty()) {
        return;
    }

    //PRINT_DEBUG("%lf", time_extra);
    // PRINT_DEBUG_ENTRY();
    const bool mode_changed = discovery_mode_changed.exchange(false);
    if (mode_changed || peers_changed || check_timedout(last_broadcast, BROADCAST_INTERVAL)) {
        send_announce_broadcasts();
    }

    IP_PORT ip_port;
    char data[MAX_UDP_SIZE];
    int len;

    if (query_alive && is_socket_valid(query_socket)) {
        PRINT_DEBUG("RECV Source Query");
        Steam_Client* client = get_steam_client();
        sockaddr_in addr;
        addr.sin_family = AF_INET;

        while ((len = receive_packet(query_socket, &ip_port, data, sizeof(data))) >= 0) {
            PRINT_DEBUG("requesting Source Query server info from Steam_GameServer");
            client->steam_gameserver->HandleIncomingPacket(data, len, htonl(ip_port.ip), htons(ip_port.port));
            len = client->steam_gameserver->GetNextOutgoingPacket(data, sizeof(data), &ip_port.ip, &ip_port.port);

            PRINT_DEBUG("sending Source Query server info");
            addr.sin_addr.s_addr = htonl(ip_port.ip);
            addr.sin_port        = htons(ip_port.port);
            sendto(query_socket, data, len, 0, (sockaddr*)&addr, sizeof(addr));
        }
    }

    bool broadcast = false;
    while ((len = receive_discovery_packet(ip_port, data, sizeof(data), broadcast)) >= 0) {
        PRINT_DEBUG("recv UDP %i %hhu.%hhu.%hhu.%hhu:%hu", len,
            ((unsigned char *)&ip_port.ip)[0], ((unsigned char *)&ip_port.ip)[1], ((unsigned char *)&ip_port.ip)[2], ((unsigned char *)&ip_port.ip)[3], htons(ip_port.port));
        Common_Message msg;
        if (msg.ParseFromArray(data, len)) {
            if (msg.source_id()) {
                if (msg.has_announce()) {
                    if (!broadcast || automatic_discovery.load()) handle_announce(&msg, ip_port);
                } else if (msg.has_low_level()) {
                    handle_low_level_udp(&msg, ip_port);
                } else {
                    msg.set_source_ip(ntohl(ip_port.ip));
                    msg.set_source_port(ntohs(ip_port.port));
                    do_callbacks_message(&msg);
                }
            }
        }
    }

    // PRINT_DEBUG("RECV LOCAL %zu", local_send.size());
    std::vector<Common_Message> local_send_copy = local_send;
    local_send.clear();

    for (auto & m: local_send_copy) {
        m.set_source_ip(own_ip);
        m.set_source_port(udp_port);
        do_callbacks_message(&m);
    }

    sock_t sock;
    while (is_socket_valid(sock = accept(tcp_socket, nullptr, nullptr))) {
        PRINT_DEBUG("ACCEPT SOCKET %llu", static_cast<unsigned long long>(sock));
        struct TCP_Socket socket;
        if (set_socket_nonblocking(sock)) {
            PRINT_DEBUG("SET NONBLOCK");
            disable_nagle(sock);
            socket.sock = sock;
            socket.received_data = true;
            socket.last_heartbeat_received = std::chrono::high_resolution_clock::now();
            accepted.push_back(socket);
            PRINT_DEBUG("TCP ACCEPTED %llu", static_cast<unsigned long long>(sock));
        } else {
            kill_socket(sock);
        }
    }

    // PRINT_DEBUG("ACCEPTED %zu", accepted.size());
    auto conn = std::begin(accepted);
    while (conn != std::end(accepted)) {
        bool deleted = false;
        recv_tcp(*conn);
        Common_Message msg;
        if (unbuffer_tcp(*conn, &msg)) {
            if (msg.source_id()) {
                Connection *connection = find_connection((uint64)msg.source_id());
                if (connection) {
                    kill_tcp_socket(connection->tcp_socket_incoming);
                    connection->tcp_socket_incoming = *conn;
                    conn = accepted.erase(conn);
                    deleted = true;
                    PRINT_DEBUG("TCP REPLACED");
                    //TODO: add other ids?
                } else {
                    //Don't allow connection from unknown
                    //Connection *conn = Networking::new_connection(msg.source_id());
                    kill_tcp_socket(*conn);
                    conn = accepted.erase(conn);
                    deleted = true;
                    PRINT_DEBUG("TCP UNKNOWN");
                }
            }
        }

        if (!deleted && check_timedout(conn->last_heartbeat_received, HEARTBEAT_TIMEOUT + time_extra)) {
            kill_tcp_socket(*conn);
            conn = accepted.erase(conn);
            deleted = true;
            PRINT_DEBUG("TCP TIMEOUT");
        }
        
        if (!deleted){
            ++conn;
        }
    }

    // PRINT_DEBUG("CONNECTIONS %zu", connections.size());
    for (auto &conn: connections) {
        if (!is_tcp_socket_valid(conn.tcp_socket_outgoing)) {
            sock = static_cast<sock_t>(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
            if (is_socket_valid(sock) && set_socket_nonblocking(sock)) {
                PRINT_DEBUG("NEW SOCKET %llu %llu", static_cast<unsigned long long>(sock),
                            static_cast<unsigned long long>(conn.tcp_socket_outgoing.sock));
                disable_nagle(sock);
                connect_socket(sock, conn.tcp_ip_port);
                conn.tcp_socket_outgoing.sock = sock;
                conn.tcp_socket_outgoing.last_heartbeat_received = std::chrono::high_resolution_clock::now();
                Common_Message msg;
                msg.set_source_id(ids[0].ConvertToUint64());
                send_buffer_tcp(conn.tcp_socket_outgoing, &msg);
            } else if (is_socket_valid(sock)) {
                kill_socket(sock);
            }
        }

        // PRINT_DEBUG("RUN SOCKET1 %u %u", conn.tcp_socket_outgoing.sock, conn.tcp_socket_incoming.sock);
        recv_tcp(conn.tcp_socket_outgoing);
        recv_tcp(conn.tcp_socket_incoming);

        if (conn.tcp_socket_incoming.received_data || conn.tcp_socket_outgoing.received_data) {
            if (!conn.connected) {
                //reconnect the connection if it has the right appid
                if (conn.appid == this->appid || conn.appid == LOBBY_CONNECT_APPID) {
                    for (auto &c: connections) {
                        if (&c == &conn) continue;
                        if (c.appid != this->appid) continue;
                        for (auto &steam_id : conn.ids) {
                            auto i = std::find(c.ids.begin(), c.ids.end(), steam_id);
                            if (i != c.ids.end()) {
                                c.ids.erase(i);
                                run_callback_user(steam_id, false, c.appid);
                                PRINT_DEBUG("REMOVE OLD CONNECTION ID");
                            }
                        }
                    }

                    for (auto &steam_id : conn.ids) run_callback_user(steam_id, true, conn.appid);
                }

                conn.connected = true;
            }
        }

        // PRINT_DEBUG("RUN SOCKET2 %u %u", conn.tcp_socket_outgoing.sock, conn.tcp_socket_incoming.sock);
        send_tcp_pending(conn.tcp_socket_outgoing);
        send_tcp_pending(conn.tcp_socket_incoming);

        // PRINT_DEBUG("RUN SOCKET3 %u %u", conn.tcp_socket_outgoing.sock, conn.tcp_socket_incoming.sock);
        Common_Message msg;
        while (unbuffer_tcp(conn.tcp_socket_outgoing, &msg)) {
            PRINT_DEBUG("UNBUFFER SOCKET");
            msg.set_source_ip(ntohl(conn.tcp_ip_port.ip)); //TODO: get from tcp socket
            handle_tcp(&msg, conn.tcp_socket_outgoing);
            conn.last_received = std::chrono::high_resolution_clock::now();
        }

        while (unbuffer_tcp(conn.tcp_socket_incoming, &msg)) {
            PRINT_DEBUG("UNBUFFER SOCKET");
            msg.set_source_ip(ntohl(conn.tcp_ip_port.ip)); //TODO: get from tcp socket
            handle_tcp(&msg, conn.tcp_socket_incoming);
            conn.last_received = std::chrono::high_resolution_clock::now();
        }

        // PRINT_DEBUG("RUN SOCKET4 %u %u", conn.tcp_socket_outgoing.sock, conn.tcp_socket_incoming.sock);
        socket_timeouts(conn.tcp_socket_outgoing, time_extra);
        socket_timeouts(conn.tcp_socket_incoming, time_extra);

    }

    {
        auto conn = std::begin(connections);
        while (conn != std::end(connections)) {
            if (check_timedout(conn->last_received, USER_TIMEOUT + time_extra)) {
                if (conn->connected) for (auto &steam_id : conn->ids) run_callback_user(steam_id, false, conn->appid);
                kill_tcp_socket(conn->tcp_socket_outgoing);
                kill_tcp_socket(conn->tcp_socket_incoming);
                conn = connections.erase(conn);
                PRINT_DEBUG("USER TIMEOUT");
            } else {
                ++conn;
            }
        }
    }

    for (auto &conn: connections) {
        if (!(conn.tcp_socket_incoming.received_data || conn.tcp_socket_outgoing.received_data)) {
            if (conn.connected) for (auto &steam_id : conn.ids) run_callback_user(steam_id, false, conn.appid);
            conn.connected = false;
        }
    }

    update_peer_discovery();
    reset_last_error();
}

void Networking::addListenId(CSteamID id)
{
    if (!enabled) return;
    auto i = std::find(ids.begin(), ids.end(), id);
    if (i != ids.end()) {
        return;
    }

    PRINT_DEBUG("ADDED ID %llu", (uint64)id.ConvertToUint64());
    ids.push_back(id);
    send_announce_broadcasts();
    return;
}

void Networking::setAppID(uint32 appid)
{
    this->appid = appid;
}

bool Networking::sendToIPPort(Common_Message *msg, uint32 ip, uint16 port, bool reliable)
{
    if (!enabled || ids.empty()) return false;

    bool is_local_ip = ((ip >> 24) == 0x7F);
    uint32_t local_ip = getIP(ids.front());
    PRINT_DEBUG("%X %u %X", ip, is_local_ip, local_ip);
    // The game port is part of the payload. It need not match the emulator's transport ports.
    (void)port;
    bool sent = false;
    for (auto &conn: connections) {
        if (ntohl(conn.tcp_ip_port.ip) == ip || (is_local_ip && ntohl(conn.tcp_ip_port.ip) == local_ip)) {
            for (auto &steam_id : conn.ids) {
                msg->set_dest_id(steam_id.ConvertToUint64());
                sent = sendTo(msg, reliable, &conn) || sent;
            }
        }
    }

    return sent;
}

uint32 Networking::getIP(CSteamID id)
{
    Connection *conn = find_connection(id, this->appid);
    if (conn) {
        return ntohl(conn->tcp_ip_port.ip);
    }

    return 0;
}

uint16 Networking::getPort(CSteamID id)
{
    Connection *conn = find_connection(id, this->appid);
    if (conn) {
        return ntohs(conn->tcp_ip_port.port);
    }

    return 0;
}

bool Networking::sendTo(Common_Message *msg, bool reliable, Connection *conn)
{
    if (!enabled) return false;

    size_t size = msg->ByteSizeLong();
    if (size >= MAX_UDP_SIZE) reliable = true; //too big for UDP

    bool ret = false;
    CSteamID dest_id((uint64)msg->dest_id());
    if (std::find(ids.begin(), ids.end(), dest_id) != ids.end()) {
        PRINT_DEBUG("sending to self");
        if (!conn) {
            PRINT_DEBUG("local send");
            local_send.push_back(*msg);
            ret = true;
        }
    }

    if (!conn) {
        conn = find_connection(dest_id, this->appid);
    }

    if (!ret && conn) {
        if (reliable || !conn->udp_pinged) {
            if (conn->tcp_socket_incoming.received_data) {
                send_buffer_tcp(conn->tcp_socket_incoming, msg);
                ret = true;
            } else if (conn->tcp_socket_outgoing.received_data) {
                send_buffer_tcp(conn->tcp_socket_outgoing, msg);
                ret = true;
            }
        } else {
            std::vector<char> buffer(size, 0);
            msg->SerializeToArray(&buffer[0], static_cast<int>(size));
            send_packet_to(udp_socket, conn->udp_ip_port, &buffer[0], static_cast<unsigned long>(size));
            ret = true;
        }
    }

    reset_last_error();
    return ret;
}

bool Networking::sendToAllIndividuals(Common_Message *msg, bool reliable)
{
    for (auto &conn: connections) {
        for (auto &steam_id : conn.ids) {
            if (steam_id.BIndividualAccount()) {
                msg->set_dest_id(steam_id.ConvertToUint64());
                sendTo(msg, reliable, &conn);
            }
        }
    }

    return true;
}

bool Networking::sendToAllGameservers(Common_Message *msg, bool reliable)
{
    for (auto &conn: connections) {
        for (auto &steam_id : conn.ids) {
            if (steam_id.BGameServerAccount()) {
                msg->set_dest_id(steam_id.ConvertToUint64());
                sendTo(msg, reliable, &conn);
            }
        }
    }

    return true;
}

bool Networking::sendToAll(Common_Message *msg, bool reliable)
{
    for (auto &conn: connections) {
        for (auto &steam_id : conn.ids) {
            msg->set_dest_id(steam_id.ConvertToUint64());
            sendTo(msg, reliable, &conn);
        }
    }

    return true;
}

void Networking::run_callbacks(Callback_Ids id, Common_Message *msg)
{
    for (auto &cb : callbacks[id].callbacks) {
        uint64 callback_allowed_steamid = cb.steam_id.ConvertToUint64();
        uint64 message_destination_steamid = msg->dest_id();
        if (callback_allowed_steamid == 0 || // callback wants to receive all messages (callback for broadcast)
            message_destination_steamid == 0 || // message was broadcasted to all (broadcast message)
            callback_allowed_steamid == message_destination_steamid) { // callback destination is the same as the message destination
            cb.message_callback(cb.object, msg);
        }
    }
}

void Networking::run_callback_user(CSteamID steam_id, bool online, uint32 appid)
{
    //only give callbacks for right game accounts
    if (steam_id.BIndividualAccount() && appid != this->appid && appid != LOBBY_CONNECT_APPID) return;

    Common_Message msg{};
    msg.set_source_id(steam_id.ConvertToUint64());
    msg.set_allocated_low_level(new Low_Level());
    if (online) {
        msg.mutable_low_level()->set_type(Low_Level::CONNECT);
    } else {
        msg.mutable_low_level()->set_type(Low_Level::DISCONNECT);
    }

    run_callbacks(CALLBACK_ID_USER_STATUS, &msg);
}

bool Networking::setCallback(Callback_Ids id, CSteamID steam_id, void (*message_callback)(void *object, Common_Message *msg), void *object)
{
    if (id >= CALLBACK_IDS_MAX) return false;

    struct Network_Callback nc{};
    nc.message_callback = message_callback;
    nc.object = object;
    nc.steam_id = steam_id;

    callbacks[id].callbacks.push_back(nc);
    return true;
}

void Networking::rmCallback(Callback_Ids id, CSteamID steam_id, void (*message_callback)(void *object, Common_Message *msg), void *object)
{
    if (id >= CALLBACK_IDS_MAX) return;

    auto &target_cb = callbacks[id].callbacks;
    auto itrm = std::remove_if(
        target_cb.begin(),
        target_cb.end(),
        [=, &steam_id](const struct Network_Callback &item) {
            return item.message_callback == message_callback &&
                   item.object == object &&
                   item.steam_id == steam_id;
        }
    );

    target_cb.erase(itrm, target_cb.end());
}

uint32 Networking::getOwnIP()
{
    return own_ip;
}

void Networking::startQuery(IP_PORT ip_port)
{
    if (ip_port.port <= 1024)
        return;

    if (!query_alive)
    {
        if (ip_port.port == MASTERSERVERUPDATERPORT_USEGAMESOCKETSHARE)
        {
            PRINT_DEBUG("Source Query in Shared Mode");
            return;
        }

        int retry = 0;
        constexpr auto max_retry = 10;

        while (retry++ < max_retry)
        {
            query_socket = static_cast<sock_t>(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
            if (is_socket_valid(query_socket))
                break;
            if (retry > max_retry)
            {
                reset_last_error();
                return;
            }
        }
        retry = 0;

        sockaddr_in addr{};
        addr.sin_addr.s_addr = htonl(ip_port.ip);
        addr.sin_port        = htons(ip_port.port);
        addr.sin_family      = AF_INET;

        while (retry++ < max_retry)
        {
            int res = bind(query_socket, (sockaddr*)&addr, sizeof(sockaddr_in));
            if (res == 0)
            {
                set_socket_nonblocking(query_socket);
                break;
            }

            if (retry >= max_retry)
            {
                kill_socket(query_socket);
                query_socket = -1;
                reset_last_error();
                return;
            }
        }

        char str_ip[16]{};
        inet_ntop(AF_INET, &(addr.sin_addr), str_ip, 16);

        PRINT_DEBUG("Started query server on %s:%d", str_ip, htons(addr.sin_port));
    }
    query_alive = true;
}

void Networking::shutDownQuery()
{
    query_alive = false;
    if (is_socket_valid(query_socket)) kill_socket(query_socket);
    query_socket = invalid_socket;
}

bool Networking::isQueryAlive()
{
    return query_alive;
}

bool Networking::refresh_peer_file()
{
    if (peer_file.empty()) return false;
    {
        const std::lock_guard lock(peer_mutex);
        const auto now = std::chrono::steady_clock::now();
        if (now < next_peer_refresh) return false;
        next_peer_refresh = now + std::chrono::seconds(1);
    }

    std::vector<steamem_peers::Endpoint> configured;
    std::string error;
    if (!steamem_peers::read(peer_file, configured, error)) {
        PRINT_DEBUG("Peer settings refresh failed: %s", error.c_str());
        return false;
    }

    std::set<IP_PORT> shared_targets;
    for (const auto &entry : configured) {
        shared_targets.insert({htonl(entry.address), htons(entry.port)});
    }

    bool changed = false;
    const std::lock_guard lock(peer_mutex);
    for (const auto &address : shared_targets) {
        auto [peer, added] = peer_targets.try_emplace(address);
        peer->second.editable = true;
        changed = changed || added;
    }
    for (auto peer = peer_targets.begin(); peer != peer_targets.end();) {
        if (peer->second.editable && !shared_targets.count(peer->first)) {
            peer->second.editable = false;
            if (!peer->second.legacy) {
                peer = peer_targets.erase(peer);
                changed = true;
                continue;
            }
        }
        ++peer;
    }
    return changed;
}

void Networking::update_peer_discovery()
{
    const std::lock_guard lock(peer_mutex);
    manual_users.clear();
    for (auto &[address, peer] : peer_targets) {
        const auto found = std::find_if(connections.cbegin(), connections.cend(), [&](const Connection &conn) {
            return conn.appid == appid && conn.announce_endpoint.ip == address.ip &&
                   conn.announce_endpoint.port == address.port;
        });
        if (found != connections.cend()) {
            peer.state = found->connected ? PeerDiscoveryState::Connected : PeerDiscoveryState::Discovered;
            if (peer.editable && found->connected) {
                for (auto id : found->ids) {
                    if (id.BIndividualAccount()) manual_users.push_back(id);
                }
            }
        } else if (peer.socket_error) {
            peer.state = PeerDiscoveryState::SendFailed;
        } else {
            peer.state = peer.attempted ? PeerDiscoveryState::Searching : PeerDiscoveryState::Waiting;
        }
    }
    std::sort(manual_users.begin(), manual_users.end());
    manual_users.erase(std::unique(manual_users.begin(), manual_users.end()), manual_users.end());
}

bool Networking::add_custom_endpoint(std::string_view endpoint, std::string &error)
{
    steamem_peers::Endpoint address;
    if (!steamem_peers::parse(endpoint, address, error) || !steamem_peers::add(peer_file, address, error)) {
        return false;
    }

    const std::lock_guard lock(peer_mutex);
    next_peer_refresh = {};
    return true;
}

bool Networking::remove_custom_endpoint(std::string_view endpoint, std::string &error)
{
    steamem_peers::Endpoint address;
    if (!steamem_peers::parse(endpoint, address, error) || !steamem_peers::remove(peer_file, address, error)) {
        return false;
    }

    const std::lock_guard lock(peer_mutex);
    next_peer_refresh = {};
    return true;
}

void Networking::set_automatic_discovery(bool enabled)
{
    if (automatic_discovery.exchange(enabled) != enabled) discovery_mode_changed.store(true);
}

PeerDiscoverySnapshot Networking::peer_discovery_snapshot() const
{
    PeerDiscoverySnapshot snapshot{};
    snapshot.enabled = enabled;
    snapshot.automatic_discovery = automatic_discovery.load();
    snapshot.udp_port = is_socket_valid(udp_socket) ? udp_port : 0;
    snapshot.tcp_port = is_socket_valid(tcp_socket) ? tcp_port : 0;

    const std::lock_guard lock(peer_mutex);
    snapshot.manual_users = manual_users;
    snapshot.peers.reserve(peer_targets.size());
    for (const auto &[address, peer] : peer_targets) {
        if (!snapshot.automatic_discovery && !peer.editable) continue;
        snapshot.peers.push_back({steamem_peers::format({ntohl(address.ip), ntohs(address.port)}),
                                  peer.state, peer.socket_error, peer.editable});
    }
    return snapshot;
}

std::vector<std::string> Networking::get_local_vpn_ips()
{
    std::vector<std::string> result;
#ifdef __WINDOWS__
    ULONG bufLen = 32768;
    std::vector<uint8_t> buffer(bufLen);
    PIP_ADAPTER_ADDRESSES pAddresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
    ULONG result_code = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, nullptr, pAddresses, &bufLen);
    if (result_code == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(bufLen);
        pAddresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
        result_code = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, nullptr, pAddresses, &bufLen);
    }
    if (result_code == NO_ERROR) {
        for (PIP_ADAPTER_ADDRESSES pCurr = pAddresses; pCurr; pCurr = pCurr->Next) {
            if (pCurr->OperStatus != IfOperStatusUp || pCurr->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            for (PIP_ADAPTER_UNICAST_ADDRESS pUni = pCurr->FirstUnicastAddress; pUni; pUni = pUni->Next) {
                if (!pUni->Address.lpSockaddr || pUni->Address.lpSockaddr->sa_family != AF_INET) continue;
                sockaddr_in* sa = reinterpret_cast<sockaddr_in*>(pUni->Address.lpSockaddr);
                char ip_str[INET_ADDRSTRLEN]{};
                if (inet_ntop(AF_INET, &(sa->sin_addr), ip_str, sizeof(ip_str))) {
                    result.emplace_back(ip_str);
                }
            }
        }
    }
#endif
    return result;
}

// Room key cipher
static std::string& get_network_room_key() {
    static std::string key;
    static bool initialized = false;
    if (!initialized) {
        const char* env = getenv("GSE_ROOM_KEY");
        if (env) key = env;
        initialized = true;
    }
    return key;
}

static void transform_packet_cipher(char *data, unsigned long length) {
    const std::string& key = get_network_room_key();
    if (key.empty() || !data || length == 0) return;
    uint32_t state = 0x811C9DC5u;
    for (char c : key) state = (state ^ static_cast<uint8_t>(c)) * 0x01000193u;
    for (unsigned long i = 0; i < length; ++i) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        data[i] ^= static_cast<char>((state ^ key[i % key.size()]) & 0xFF);
    }
}
// ===================== END NETWORK FUNCTIONS =====================
