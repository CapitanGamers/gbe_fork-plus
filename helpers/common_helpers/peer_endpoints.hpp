#pragma once

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace steamem_peers {
inline constexpr uint16_t default_port = 47584;

struct Endpoint {
    uint32_t address{};
    uint16_t port{};

    bool operator==(const Endpoint &other) const
    {
        return address == other.address && port == other.port;
    }
};

inline std::string_view trim(std::string_view value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

inline bool decimal(std::string_view value, unsigned limit, unsigned &number)
{
    if (value.empty()) return false;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), number);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size() && number <= limit;
}

inline bool parse(std::string_view input, Endpoint &result, std::string &error,
                  uint16_t fallback_port = default_port)
{
    input = trim(input);
    auto address = input.substr(0, input.find(':'));
    uint32_t ip = 0;
    for (int i = 0; i < 4; ++i) {
        const auto dot = address.find('.');
        unsigned octet = 0;
        if ((dot == std::string_view::npos) != (i == 3) ||
            !decimal(address.substr(0, dot), 255, octet)) {
            error = "Enter an IPv4 address, optionally followed by :port.";
            return false;
        }
        ip = (ip << 8) | octet;
        if (i < 3) address.remove_prefix(dot + 1);
    }

    if (ip >> 24 == 0 || ip >> 24 >= 224) {
        error = "Enter the unicast address of the other player.";
        return false;
    }

    unsigned port = fallback_port;
    const auto colon = input.find(':');
    if ((colon != std::string_view::npos && !decimal(input.substr(colon + 1), 65535, port)) || port == 0) {
        error = "Enter a port between 1 and 65535.";
        return false;
    }

    result = {ip, static_cast<uint16_t>(port)};
    error.clear();
    return true;
}

inline std::string format(const Endpoint &endpoint)
{
    const auto ip = endpoint.address;
    return std::to_string(ip >> 24) + "." + std::to_string((ip >> 16) & 255) + "." +
           std::to_string((ip >> 8) & 255) + "." + std::to_string(ip & 255) + ":" +
           std::to_string(endpoint.port);
}

namespace detail {

inline constexpr std::size_t max_file_size = 64 * 1024;

#ifdef _WIN32
inline std::filesystem::path environment_path(const wchar_t *name)
{
    const auto size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size == 0) return {};
    std::wstring value(size, L'\0');
    const auto length = GetEnvironmentVariableW(name, value.data(), size);
    if (length == 0 || length >= size) return {};
    value.resize(length);
    return value;
}
#endif

class FileLock {
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
#else
    int handle = -1;
#endif
    bool acquired = false;

public:
    explicit FileLock(const std::filesystem::path &file)
    {
        auto path = file;
        path += ".lock";
#ifdef _WIN32
        handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        OVERLAPPED range{};
        if (handle != INVALID_HANDLE_VALUE) {
            acquired = LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                                  0, 1, 0, &range) != FALSE;
        }
#else
        handle = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if (handle >= 0) acquired = flock(handle, LOCK_EX | LOCK_NB) == 0;
#endif
    }

    FileLock(const FileLock &) = delete;
    FileLock &operator=(const FileLock &) = delete;

    ~FileLock()
    {
#ifdef _WIN32
        if (handle != INVALID_HANDLE_VALUE) {
            OVERLAPPED range{};
            if (acquired) UnlockFileEx(handle, 0, 1, 0, &range);
            CloseHandle(handle);
        }
#else
        if (handle >= 0) close(handle);
#endif
    }

    explicit operator bool() const { return acquired; }
};

inline bool file_exists(const std::filesystem::path &file, bool &exists, std::string &error)
{
    if (file.empty()) {
        error = "I couldn't locate the SteamEM peer settings directory.";
        return false;
    }
    std::error_code filesystem_error;
    exists = std::filesystem::exists(file, filesystem_error);
    if (filesystem_error) {
        error = "I couldn't check the SteamEM peer settings file.";
        return false;
    }
    return true;
}

inline bool load_document(const std::filesystem::path &file, std::string &contents, std::string &error)
{
    bool exists = false;
    if (!file_exists(file, exists, error)) return false;
    if (!exists) return true;

    std::ifstream input(file, std::ios::binary);
    if (!input) {
        error = "I couldn't open the SteamEM peer settings file.";
        return false;
    }
    contents.resize(max_file_size + 1);
    input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    const auto size = static_cast<std::size_t>(input.gcount());
    if (input.bad() || size > max_file_size) {
        error = "The SteamEM peer settings file could not be read or is too large.";
        return false;
    }
    contents.resize(size);
    return true;
}

template <typename ReceiveLine>
inline void for_each_line(std::string_view contents, ReceiveLine receive_line)
{
    while (!contents.empty()) {
        const auto end = contents.find('\n');
        const auto length = end == std::string_view::npos ? contents.size() : end + 1;
        receive_line(contents.substr(0, length));
        contents.remove_prefix(length);
    }
}

inline bool parse_line(std::string_view line, Endpoint &endpoint)
{
    if (line.substr(0, 3) == "\xef\xbb\xbf") line.remove_prefix(3);
    std::string ignored;
    return parse(line.substr(0, line.find_first_of("#;")), endpoint, ignored);
}

inline bool write_document(const std::filesystem::path &file, const std::string &contents, std::string &error)
{
    if (contents.size() > max_file_size) {
        error = "The SteamEM peer settings file is too large.";
        return false;
    }

    auto temporary_file = file;
    temporary_file += ".steamem.tmp";
    std::ofstream output(temporary_file, std::ios::binary | std::ios::trunc);
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    output.close();
    std::error_code filesystem_error;
    if (output) {
#ifdef _WIN32
        if (MoveFileExW(temporary_file.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return true;
#else
        std::filesystem::rename(temporary_file, file, filesystem_error);
        if (!filesystem_error) return true;
#endif
    }
    std::filesystem::remove(temporary_file, filesystem_error);
    error = "I couldn't save the SteamEM peer settings file.";
    return false;
}

inline bool update(const std::filesystem::path &file, const Endpoint &endpoint, bool remove, std::string &error)
{
    error.clear();
    if (file.empty()) {
        error = "I couldn't locate the SteamEM peer settings directory.";
        return false;
    }

    std::error_code filesystem_error;
    if (!file.parent_path().empty()) std::filesystem::create_directories(file.parent_path(), filesystem_error);
    if (filesystem_error) {
        error = "I couldn't create the SteamEM peer settings directory.";
        return false;
    }

    FileLock lock(file);
    if (!lock) {
        error = "The SteamEM peer settings file is busy or inaccessible. Retry in a moment.";
        return false;
    }

    std::string contents;
    if (!load_document(file, contents, error)) return false;

    bool found = false;
    std::string updated;
    for_each_line(contents, [&](std::string_view line) {
        Endpoint old;
        const bool matched = parse_line(line, old) && old == endpoint;
        found = found || matched;
        if (!remove || !matched) {
            updated.append(line);
        } else if (line.substr(0, 3) == "\xef\xbb\xbf") {
            updated.append(line.substr(0, 3));
        }
    });
    if (remove) {
        return !found || write_document(file, updated, error);
    }
    if (found) return true;

    if (!contents.empty() && contents.back() != '\n') contents.push_back('\n');
    contents += format(endpoint) + "\n";
    return write_document(file, contents, error);
}

} // namespace detail

inline std::filesystem::path default_file()
{
#ifdef _WIN32
    auto override = detail::environment_path(L"STEAMEM_PEERS_FILE");
    if (!override.empty()) return override;
    const auto directory = detail::environment_path(L"APPDATA");
#else
    const char *override = std::getenv("STEAMEM_PEERS_FILE");
    if (override && *override) return override;
    const char *config_home = std::getenv("XDG_CONFIG_HOME");
    const char *home = std::getenv("HOME");
    const std::filesystem::path directory = config_home && *config_home ? std::filesystem::path(config_home)
        : (home && *home ? std::filesystem::path(home) / ".config" : std::filesystem::path{});
#endif
    if (directory.empty()) return {};
    return directory / "SteamEM" / "custom_broadcasts.txt";
}

inline bool read(const std::filesystem::path &file, std::vector<Endpoint> &out, std::string &error)
{
    error.clear();
    bool exists = false;
    if (!detail::file_exists(file, exists, error)) return false;
    if (!exists) {
        out.clear();
        return true;
    }

    detail::FileLock lock(file);
    if (!lock) {
        error = "The SteamEM peer settings file is busy or inaccessible.";
        return false;
    }
    std::string contents;
    if (!detail::load_document(file, contents, error)) return false;
    out.clear();
    detail::for_each_line(contents, [&](std::string_view line) {
        Endpoint endpoint;
        if (detail::parse_line(line, endpoint) && std::find(out.begin(), out.end(), endpoint) == out.end()) {
            out.push_back(endpoint);
        }
    });
    return true;
}

inline bool add(const std::filesystem::path &file, const Endpoint &endpoint, std::string &error)
{
    return detail::update(file, endpoint, false, error);
}

inline bool remove(const std::filesystem::path &file, const Endpoint &endpoint, std::string &error)
{
    return detail::update(file, endpoint, true, error);
}

} // namespace steamem_peers
