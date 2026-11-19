#include "game_launch.h"
#include "common_helpers/peer_endpoints.hpp"

#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <dwmapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <future>
#include <optional>
#include <thread>

namespace {

namespace fs = std::filesystem;
namespace launcher = steamem_launcher;
namespace peers = steamem_peers;

constexpr COLORREF background = RGB(16, 23, 34);
constexpr COLORREF header = RGB(20, 29, 42);
constexpr COLORREF panel = RGB(25, 36, 51);
constexpr COLORREF input_bg = RGB(18, 27, 39);
constexpr COLORREF edge = RGB(51, 70, 91);
constexpr COLORREF foreground = RGB(238, 244, 251);
constexpr COLORREF muted = RGB(167, 185, 206);
constexpr COLORREF accent = RGB(104, 214, 206);
constexpr COLORREF error_color = RGB(254, 158, 152);

#ifdef STEAMEM_EPIC_LAN
constexpr bool epic_lan = true;
constexpr auto app_title = L"Epic LAN";
#else
constexpr bool epic_lan = false;
constexpr auto app_title = L"SteamEM";
#endif

enum class Page { Library, Peers, Activity };
enum Control {
    LibraryTab = 100, PeersTab, ActivityTab,
    Title, Subtitle, StatusLabel,
    OpenFolder, PickExecutable, GameList, GameName, GamePath, GameInfo, ArgumentsLabel, ArgumentsEdit, Launch,
    PeerLabel, PeerEdit, PortLabel, PortEdit, AddPeer, PeerList, RemovePeer, PeerHelp,
    ActivityList, OpenLog, ControlEnd
};

constexpr auto nav_controls = std::array{LibraryTab, PeersTab, ActivityTab};
constexpr auto library_controls = std::array{OpenFolder, PickExecutable, GameList, GameName, GamePath, GameInfo,
                                             ArgumentsLabel, ArgumentsEdit, Launch};
constexpr auto peer_controls = std::array{PeerLabel, PeerEdit, PortLabel, PortEdit, AddPeer, PeerList, RemovePeer, PeerHelp};
constexpr auto activity_controls = std::array{ActivityList, OpenLog};

struct ReleaseCom {
    void operator()(IUnknown *object) const noexcept { object->Release(); }
};

template <class Work>
auto run_in_background(Work work)
{
    std::packaged_task task(std::move(work));
    auto future = task.get_future();
    std::thread(std::move(task)).detach();
    return future;
}

std::wstring wide_text(std::string_view text)
{
    if (text.empty()) return {};
    const auto length = static_cast<int>(text.size());
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, nullptr, 0);
    if (!count) return L"Details unavailable";
    std::wstring result(count, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, result.data(), count);
    return result;
}

std::wstring control_text(HWND control)
{
    const int length = GetWindowTextLengthW(control);
    std::wstring result(static_cast<size_t>(length) + 1, L'\0');
    result.resize(GetWindowTextW(control, result.data(), length + 1));
    return result;
}

fs::path activity_file()
{
    PWSTR path = nullptr;
    const auto result = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &path);
    fs::path file;
    if (SUCCEEDED(result)) file = fs::path(path) / L"SteamEM" / (epic_lan ? L"epic-lan.log" : L"launcher.log");
    CoTaskMemFree(path);
    return file;
}

fs::path endpoint_file()
{
    auto file = peers::default_file();
    if (epic_lan && !file.empty()) file.replace_filename(L"epic_lan_endpoints.txt");
    return file;
}

fs::path pick_path(HWND parent, bool folder)
{
    IFileOpenDialog *opened = nullptr;
    HRESULT result = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&opened));
    if (FAILED(result)) throw std::system_error(result, std::system_category(), "File picker");
    std::unique_ptr<IFileOpenDialog, ReleaseCom> dialog(opened);

    DWORD options = 0;
    dialog->GetOptions(&options);
    options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
    if (folder) {
        options |= FOS_PICKFOLDERS;
        dialog->SetTitle(L"Choose a game folder");
    } else {
        options |= FOS_FILEMUSTEXIST;
        const COMDLG_FILTERSPEC filter{L"Windows executable", L"*.exe"};
        dialog->SetFileTypes(1, &filter);
        dialog->SetTitle(L"Choose the game's executable");
    }
    dialog->SetOptions(options);
    result = dialog->Show(parent);
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return {};
    if (FAILED(result)) throw std::system_error(result, std::system_category(), "File picker");

    IShellItem *item = nullptr;
    result = dialog->GetResult(&item);
    if (FAILED(result)) throw std::system_error(result, std::system_category(), "Selected path");
    std::unique_ptr<IShellItem, ReleaseCom> selected(item);
    PWSTR name = nullptr;
    result = selected->GetDisplayName(SIGDN_FILESYSPATH, &name);
    if (FAILED(result)) throw std::system_error(result, std::system_category(), "Selected path");
    fs::path picked(name);
    CoTaskMemFree(name);
    return picked;
}

void paint_rect(HDC dc, RECT rect, COLORREF color)
{
    const auto brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

class App {
public:
    HWND window = nullptr;
    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam);

private:
    std::array<HWND, ControlEnd - LibraryTab> controls{};
    Page page = Page::Library;
    UINT dpi = 96;
    HFONT body_font = nullptr;
    HFONT title_font = nullptr;
    HFONT strong_font = nullptr;
    HBRUSH panel_brush = nullptr;
    HBRUSH input_brush = nullptr;
    HBRUSH background_brush = nullptr;

    fs::path game_folder;
    std::vector<fs::path> games;
    fs::path selected_game;
    fs::path scanned_folder;
    std::future<std::vector<fs::path>> scan;
    std::future<launcher::LaunchResult> launch;
    launcher::ProcessHandle process;
    DWORD process_id = 0;
    ULONGLONG started = 0;
    bool window_seen = false;
    bool window_timeout_reported = false;

    fs::path peer_file = endpoint_file();
    std::vector<peers::Endpoint> peer_list;
    std::optional<fs::file_time_type> peer_time;

    fs::path log_file = activity_file();
    bool status_error = false;
    int x(int pixels) const { return MulDiv(pixels, dpi, 96); }
    HWND control(Control id) const { return controls[id - LibraryTab]; }
    void create(Control id, const wchar_t *class_name, const wchar_t *text, DWORD style, DWORD extended = 0);
    void create_window();
    void change_page(Page next);
    void layout();
    void paint();
    void draw_button(const DRAWITEMSTRUCT &item) const;
    void refresh_fonts();
    void status(const std::wstring &message, bool error = false);
    void log(const std::wstring &message);
    void choose_game(bool folder);
    void show_games();
    void select_game();
    void start_game();
    void poll();
    bool refresh_peers(bool force = false);
    void edit_peers(bool remove);
};

void App::create(Control id, const wchar_t *class_name, const wchar_t *text, DWORD style, DWORD extended)
{
    controls[id - LibraryTab] = CreateWindowExW(extended, class_name, text, WS_CHILD | style,
        0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
}

void App::create_window()
{
    dpi = GetDpiForWindow(window);
    panel_brush = CreateSolidBrush(panel);
    input_brush = CreateSolidBrush(input_bg);
    background_brush = CreateSolidBrush(background);

    constexpr DWORD button = BS_OWNERDRAW | WS_TABSTOP;
    constexpr DWORD listbox = LBS_NOINTEGRALHEIGHT | LBS_NOTIFY | WS_TABSTOP | WS_VSCROLL;
    constexpr DWORD label = SS_NOPREFIX;
    create(LibraryTab, L"BUTTON", L"Library", button | WS_VISIBLE);
    create(PeersTab, L"BUTTON", epic_lan ? L"Servers" : L"Peers", button | WS_VISIBLE);
    create(ActivityTab, L"BUTTON", L"Activity", button | WS_VISIBLE);
    create(Title, L"STATIC", L"", label | WS_VISIBLE);
    create(Subtitle, L"STATIC", L"", label | WS_VISIBLE);
    create(StatusLabel, L"STATIC", L"Ready", label | SS_ENDELLIPSIS | WS_VISIBLE);

    create(OpenFolder, L"BUTTON", L"Open folder", button);
    create(PickExecutable, L"BUTTON", L"Pick executable", button);
    create(GameList, L"LISTBOX", L"", listbox, WS_EX_CLIENTEDGE);
    create(GameName, L"STATIC", L"No executable selected", label | SS_ENDELLIPSIS);
    create(GamePath, L"STATIC", L"", label | SS_PATHELLIPSIS);
    create(GameInfo, L"STATIC", L"", label);
    create(ArgumentsLabel, L"STATIC", epic_lan ? L"Launch options ({endpoint} uses the selected server)" : L"Launch options", label);
    create(ArgumentsEdit, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE);
    create(Launch, L"BUTTON", L"Launch game", button);
    EnableWindow(control(Launch), FALSE);

    create(PeerLabel, L"STATIC", epic_lan ? L"Server IPv4 address" : L"Friend's IPv4 address", label);
    create(PeerEdit, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE);
    SendMessageW(control(PeerEdit), EM_SETLIMITTEXT, 48, 0);
    create(PortLabel, L"STATIC", epic_lan ? L"Server port" : L"Port (47584)", label);
    create(PortEdit, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE);
    SendMessageW(control(PortEdit), EM_SETLIMITTEXT, 12, 0);
    create(AddPeer, L"BUTTON", epic_lan ? L"Add server" : L"Add peer", button);
    create(PeerList, L"LISTBOX", L"", listbox, WS_EX_CLIENTEDGE);
    create(RemovePeer, L"BUTTON", L"Remove selected", button);
    create(PeerHelp, L"STATIC", epic_lan ?
        L"Select a server here, then use {endpoint} in a launch argument supported by the game. For games with an in-game LAN browser, join there." :
        L"Saved addresses are shared with the in-game overlay. Open the overlay to check discovery and lobby status.", label);
    EnableWindow(control(RemovePeer), FALSE);

    create(ActivityList, L"LISTBOX", L"", listbox | WS_HSCROLL, WS_EX_CLIENTEDGE);
    SendMessageW(control(ActivityList), LB_SETHORIZONTALEXTENT, 1800, 0);
    create(OpenLog, L"BUTTON", L"Open log", button);
    std::error_code log_error;
    EnableWindow(control(OpenLog), !log_file.empty() && fs::is_regular_file(log_file, log_error));
    refresh_fonts();
    change_page(Page::Library);
    SetTimer(window, 1, 250, nullptr);
}

void App::refresh_fonts()
{
    for (const auto font : {body_font, title_font, strong_font}) if (font) DeleteObject(font);
    const auto font = [&](int height, int weight) {
        return CreateFontW(-x(height), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    };
    body_font = font(15, FW_NORMAL);
    title_font = font(34, FW_SEMIBOLD);
    strong_font = font(19, FW_SEMIBOLD);
    for (auto hwnd : controls) SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(body_font), FALSE);
    SendMessageW(control(Title), WM_SETFONT, reinterpret_cast<WPARAM>(title_font), FALSE);
    SendMessageW(control(GameName), WM_SETFONT, reinterpret_cast<WPARAM>(strong_font), FALSE);
}

void App::change_page(Page next)
{
    page = next;
    const auto show = [&](const auto &group, bool visible) {
        for (auto id : group) ShowWindow(control(id), visible ? SW_SHOWNA : SW_HIDE);
    };
    show(library_controls, page == Page::Library);
    show(peer_controls, page == Page::Peers);
    show(activity_controls, page == Page::Activity);

    switch (page) {
    case Page::Library:
        SetWindowTextW(control(Title), L"Game library");
        SetWindowTextW(control(Subtitle), epic_lan ?
            L"Open a game or dedicated server that supports local multiplayer." :
            L"Select a folder or executable to launch an installed game.");
        break;
    case Page::Peers:
        SetWindowTextW(control(Title), epic_lan ? L"LAN servers" : L"LAN peers");
        SetWindowTextW(control(Subtitle), epic_lan ? L"Save a game server's LAN or VPN endpoint." :
            L"Register the address your friend uses on the local network or VPN.");
        refresh_peers(true);
        break;
    case Page::Activity:
        SetWindowTextW(control(Title), L"Activity");
        SetWindowTextW(control(Subtitle), L"Game startup, window detection, and exit details.");
        break;
    }
    layout();
    InvalidateRect(window, nullptr, FALSE);
    for (auto id : nav_controls) InvalidateRect(control(id), nullptr, FALSE);
}

void App::layout()
{
    RECT rect{};
    GetClientRect(window, &rect);
    const int width = MulDiv(rect.right, 96, dpi);
    const int height = MulDiv(rect.bottom, 96, dpi);
    const int left = 214;
    const int right = width - 28;
    const int body_width = right - left;
    const int split = left + body_width * 53 / 100;
    const int game_details = split + 32;
    const int details_width = right - game_details - 16;
    const auto place = [&](Control id, int sx, int sy, int sw, int sh) {
        MoveWindow(control(id), x(sx), x(sy), x((std::max)(0, sw)), x((std::max)(0, sh)), TRUE);
    };
    place(LibraryTab, 18, 87, 160, 42);
    place(PeersTab, 18, 138, 160, 42);
    place(ActivityTab, 18, 189, 160, 42);
    place(Title, left, 83, body_width, 46);
    place(Subtitle, left, 134, body_width, 25);
    place(StatusLabel, left, height - 43, body_width, 23);

    place(OpenFolder, left + 16, 191, 113, 37);
    place(PickExecutable, left + 139, 191, 137, 37);
    place(GameList, left + 16, 242, split - left - 31, height - 319);
    place(GameName, game_details, 192, details_width, 29);
    place(GamePath, game_details, 232, details_width, 24);
    place(GameInfo, game_details, 274, details_width, 83);
    place(ArgumentsLabel, game_details, 363, details_width, 36);
    place(ArgumentsEdit, game_details, 401, details_width, 30);
    place(Launch, game_details, 454, details_width, 42);

    place(PeerLabel, left + 16, 191, body_width - 273, 22);
    place(PeerEdit, left + 16, 223, body_width - 273, 33);
    place(PortLabel, right - 246, 191, 104, 22);
    place(PortEdit, right - 246, 223, 104, 33);
    place(AddPeer, right - 130, 223, 114, 33);
    place(PeerList, left + 16, 272, body_width - 32, height - 450);
    place(RemovePeer, left + 16, height - 162, 154, 36);
    place(PeerHelp, left + 16, height - 110, body_width - 32, 41);

    place(ActivityList, left + 16, 192, body_width - 32, height - 333);
    place(OpenLog, left + 16, height - 125, 101, 37);
}

void App::paint()
{
    PAINTSTRUCT ps{};
    const auto dc = BeginPaint(window, &ps);
    RECT bounds{};
    GetClientRect(window, &bounds);
    FillRect(dc, &bounds, background_brush);
    paint_rect(dc, RECT{0, 0, bounds.right, x(62)}, header);
    paint_rect(dc, RECT{0, x(62), x(194), bounds.bottom}, header);
    paint_rect(dc, RECT{x(193), x(62), x(194), bounds.bottom}, edge);
    paint_rect(dc, RECT{x(22), x(24), x(33), x(36)}, accent);
    const auto original = SelectObject(dc, strong_font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, foreground);
    RECT brand{x(44), 0, x(182), x(62)};
    DrawTextW(dc, app_title, -1, &brand, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    SelectObject(dc, original);

    const LONG right = bounds.right - x(28);
    const LONG split = x(214) + (right - x(214)) * 53 / 100;
    const LONG bottom = bounds.bottom - x(60);
    paint_rect(dc, RECT{x(214), x(174), page == Page::Library ? split : right, bottom}, panel);
    if (page == Page::Library) paint_rect(dc, RECT{split + x(16), x(174), right, bottom}, panel);
    EndPaint(window, &ps);
}

void App::draw_button(const DRAWITEMSTRUCT &item) const
{
    const auto id = static_cast<Control>(item.CtlID);
    const bool nav = id >= LibraryTab && id <= ActivityTab;
    const bool selected = (id == LibraryTab && page == Page::Library) || (id == PeersTab && page == Page::Peers) ||
                          (id == ActivityTab && page == Page::Activity);
    const bool primary = id == Launch || id == AddPeer;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool down = (item.itemState & ODS_SELECTED) != 0;
    const auto color = disabled ? input_bg : primary ? (down ? RGB(84, 184, 180) : accent) :
                       nav ? (selected || down ? panel : header) : (down ? edge : RGB(41, 59, 78));
    paint_rect(item.hDC, item.rcItem, color);
    const auto previous = SelectObject(item.hDC, body_font);
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, disabled ? muted : primary ? input_bg : selected ? accent : foreground);
    RECT text = item.rcItem;
    if (nav) {
        text.left += x(17);
        if (selected) paint_rect(item.hDC, RECT{item.rcItem.left, item.rcItem.top + x(8), item.rcItem.left + x(3),
                                                item.rcItem.bottom - x(8)}, accent);
    }
    const std::wstring label = control_text(item.hwndItem);
    DrawTextW(item.hDC, label.c_str(), -1, &text, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | (nav ? DT_LEFT : DT_CENTER));
    if ((item.itemState & ODS_FOCUS) != 0 && (item.itemState & ODS_NOFOCUSRECT) == 0) {
        RECT inset = item.rcItem;
        InflateRect(&inset, -x(3), -x(3));
        DrawFocusRect(item.hDC, &inset);
    }
    SelectObject(item.hDC, previous);
}

void App::status(const std::wstring &message, bool error)
{
    status_error = error;
    SetWindowTextW(control(StatusLabel), message.c_str());
    InvalidateRect(control(StatusLabel), nullptr, TRUE);
}

void App::log(const std::wstring &message)
{
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t stamp[28]{};
    swprintf(stamp, std::size(stamp), L"%04u-%02u-%02u %02u:%02u:%02u", time.wYear, time.wMonth, time.wDay,
             time.wHour, time.wMinute, time.wSecond);
    const auto line = std::wstring(stamp) + L"  " + message;
    const HWND list = control(ActivityList);
    if (SendMessageW(list, LB_GETCOUNT, 0, 0) >= 300) SendMessageW(list, LB_DELETESTRING, 0, 0);
    const auto index = SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.c_str()));
    if (index >= 0) SendMessageW(list, LB_SETTOPINDEX, index, 0);

    if (log_file.empty()) return;
    std::error_code error;
    fs::create_directories(log_file.parent_path(), error);
    if (error) return;
    std::ofstream stream(log_file, std::ios::binary | std::ios::app);
    const int size = static_cast<int>(line.size());
    const int count = WideCharToMultiByte(CP_UTF8, 0, line.data(), size, nullptr, 0, nullptr, nullptr);
    if (!stream || !count) return;
    std::string utf8(count, '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.data(), size, utf8.data(), count, nullptr, nullptr);
    stream << utf8 << "\r\n";
    stream.close();
    EnableWindow(control(OpenLog), stream.good());
}

void App::choose_game(bool folder)
{
    const auto path = pick_path(window, folder);
    if (path.empty()) return;
    if (!folder) {
        game_folder = path.parent_path();
        games = {path};
        show_games();
        return;
    }

    scanned_folder = path;
    scan = run_in_background([path] { return launcher::find_executables(path); });
    EnableWindow(control(OpenFolder), FALSE);
    EnableWindow(control(PickExecutable), FALSE);
    status(L"Scanning the selected folder…");
}

void App::show_games()
{
    const HWND list = control(GameList);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    for (const auto &path : games) {
        const auto display = path.lexically_relative(game_folder).native();
        SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(display.c_str()));
    }
    SendMessageW(list, LB_SETCURSEL, games.size() == 1 ? 0 : -1, 0);
    select_game();
    if (games.empty()) status(L"No executables found. Use Pick executable to select a file directly.", true);
    else if (games.size() > 1) status(L"Select the game's executable from the list.");
}

void App::select_game()
{
    const auto index = SendMessageW(control(GameList), LB_GETCURSEL, 0, 0);
    selected_game = index >= 0 && static_cast<size_t>(index) < games.size() ? games[index] : fs::path{};
    EnableWindow(control(Launch), !selected_game.empty() && !launch.valid() && !process);
    if (selected_game.empty()) {
        SetWindowTextW(control(GameName), L"No executable selected");
        SetWindowTextW(control(GamePath), L"");
        SetWindowTextW(control(GameInfo), L"");
        return;
    }
    const auto info = launcher::inspect_game(selected_game);
    SetWindowTextW(control(GameName), selected_game.stem().c_str());
    SetWindowTextW(control(GamePath), selected_game.c_str());
    auto summary = info.architecture + L"\r\n" + info.detected_apis;
    if (!info.overlay_setting.empty()) summary += L"\r\n" + info.overlay_setting;
    SetWindowTextW(control(GameInfo), summary.c_str());
    if (!process && !launch.valid()) status(L"Ready to launch");
}

void App::start_game()
{
    if (selected_game.empty() || launch.valid() || process) return;
    const auto path = selected_game;
    auto options = control_text(control(ArgumentsEdit));
    if (epic_lan && options.find(launcher::endpoint_argument) != std::wstring::npos) {
        if (!refresh_peers(true)) return;
        const auto index = SendMessageW(control(PeerList), LB_GETCURSEL, 0, 0);
        if (index < 0 || static_cast<size_t>(index) >= peer_list.size()) {
            change_page(Page::Peers);
            status(L"Select a server to use {endpoint} in the launch options.", true);
            return;
        }
        options = launcher::with_server_endpoint(std::move(options), wide_text(peers::format(peer_list[index])));
    }
    launch = run_in_background([path, options] { return launcher::launch_game(path, options); });
    EnableWindow(control(Launch), FALSE);
    SetWindowTextW(control(Launch), L"Starting…");
    status(L"Starting " + path.filename().native() + L"…");
    log(L"Launch requested: " + path.native());
}

void App::poll()
{
    using namespace std::chrono_literals;
    if (scan.valid() && scan.wait_for(0s) == std::future_status::ready) {
        EnableWindow(control(OpenFolder), TRUE);
        EnableWindow(control(PickExecutable), TRUE);
        try {
            games = scan.get();
            game_folder = std::move(scanned_folder);
            show_games();
        } catch (const std::system_error &error) {
            const auto message = L"Could not scan folder. " + launcher::describe_error(error.code().value());
            status(message, true);
            log(message);
        }
    }
    if (launch.valid() && launch.wait_for(0s) == std::future_status::ready) {
        auto result = launch.get();
        process = std::move(result.process);
        process_id = result.process_id;
        if (!process) {
            const auto message = L"Launch failed. " + launcher::describe_error(result.error);
            status(message, true);
            log(message);
            SetWindowTextW(control(Launch), L"Launch game");
            EnableWindow(control(Launch), !selected_game.empty());
        } else {
            started = GetTickCount64();
            window_seen = false;
            window_timeout_reported = false;
            SetWindowTextW(control(Launch), L"Process running");
            status(L"Process started. Waiting for a game window.");
            log(L"Windows created process " + std::to_wstring(process_id));
        }
    }

    if (process) {
        const auto result = WaitForSingleObject(process.get(), 0);
        if (result == WAIT_OBJECT_0 || result == WAIT_FAILED) {
            const DWORD wait_error = result == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
            DWORD code = 0;
            std::wstring message;
            bool failed = wait_error != ERROR_SUCCESS;
            if (failed || !GetExitCodeProcess(process.get(), &code)) {
                const DWORD error = failed ? wait_error : GetLastError();
                message = L"Could not read process status. " + launcher::describe_error(error);
                failed = true;
            } else {
                wchar_t hex[16]{};
                swprintf(hex, std::size(hex), L"0x%08lX", code);
                message = window_seen ? L"Game process exited. " : L"Process exited before a game window appeared. ";
                message += std::wstring(L"Exit code ") + hex + L".";
                failed = code != 0;
            }
            status(message, failed);
            log(message);
            process.reset();
            SetWindowTextW(control(Launch), L"Launch game");
            EnableWindow(control(Launch), !selected_game.empty());
        } else if (!window_seen && launcher::has_visible_window(process_id)) {
            window_seen = true;
            status(L"Game window detected.");
            log(L"Game window detected for process " + std::to_wstring(process_id));
        } else if (!window_seen && !window_timeout_reported && GetTickCount64() - started >= 12000) {
            window_timeout_reported = true;
            const std::wstring message = L"Process is running. It has not opened a visible window yet.";
            status(message);
            log(message);
        }
    }
    if (page == Page::Peers) refresh_peers();
}

bool App::refresh_peers(bool force)
{
    std::error_code error;
    const auto time = fs::last_write_time(peer_file, error);
    const std::optional<fs::file_time_type> current = error ? std::nullopt : std::optional{time};
    if (!force && current == peer_time) return true;

    std::vector<peers::Endpoint> updated;
    std::string reason;
    if (!peers::read(peer_file, updated, reason)) {
        status(L"Could not read peer addresses. " + wide_text(reason), true);
        return false;
    }
    peer_time = current;
    const HWND list = control(PeerList);
    const auto selection = SendMessageW(list, LB_GETCURSEL, 0, 0);
    const auto selected = selection >= 0 && static_cast<size_t>(selection) < peer_list.size() ?
        peers::format(peer_list[selection]) : std::string{};
    peer_list = std::move(updated);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    bool selected_again = false;
    for (size_t index = 0; index < peer_list.size(); ++index) {
        const auto entry = peers::format(peer_list[index]);
        const auto display = wide_text(entry);
        SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(display.c_str()));
        if (entry == selected) {
            SendMessageW(list, LB_SETCURSEL, index, 0);
            selected_again = true;
        }
    }
    EnableWindow(control(RemovePeer), selected_again);
    return true;
}

void App::edit_peers(bool remove)
{
    std::string reason;
    bool saved = false;
    peers::Endpoint endpoint{};
    if (remove) {
        const auto selection = SendMessageW(control(PeerList), LB_GETCURSEL, 0, 0);
        if (selection < 0 || static_cast<size_t>(selection) >= peer_list.size()) return;
        endpoint = peer_list[selection];
        saved = peers::remove(peer_file, endpoint, reason);
    } else {
        const auto ip = control_text(control(PeerEdit));
        const auto port = control_text(control(PortEdit));
        if (ip.find(L':') != std::wstring::npos || port.find(L':') != std::wstring::npos) {
            status(L"Enter the IPv4 address and port in their separate fields.", true);
            return;
        }
        if (epic_lan && port.find_first_not_of(L" \t") == std::wstring::npos) {
            status(L"Enter the port used by the game server.", true);
            return;
        }
        const auto input = port.find_first_not_of(L" \t") == std::wstring::npos ? ip : ip + L":" + port;
        std::string endpoint_text;
        endpoint_text.reserve(input.size());
        for (auto ch : input) {
            if (ch > 0x7f) {
                status(L"Enter the address and port using digits 0–9.", true);
                return;
            }
            endpoint_text.push_back(static_cast<char>(ch));
        }
        if (!peers::parse(endpoint_text, endpoint, reason)) {
            status(wide_text(reason), true);
            return;
        }
        saved = peers::add(peer_file, endpoint, reason);
    }
    if (!saved) {
        status(L"Could not update peer addresses. " + wide_text(reason), true);
        return;
    }
    if (!remove) {
        SetWindowTextW(control(PeerEdit), L"");
        SetWindowTextW(control(PortEdit), L"");
    }
    refresh_peers(true);
    if (epic_lan) status(remove ? L"Server removed." : L"Server saved.");
    else status(remove ? L"Peer removed." : L"Peer saved. See the in-game overlay for discovery status.");
}

LRESULT App::handle(UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_CREATE: create_window(); return 0;
    case WM_SIZE: layout(); InvalidateRect(window, nullptr, FALSE); return 0;
    case WM_DPICHANGED: {
        dpi = LOWORD(wparam);
        const auto &suggested = *reinterpret_cast<const RECT *>(lparam);
        SetWindowPos(window, nullptr, suggested.left, suggested.top, suggested.right - suggested.left,
                     suggested.bottom - suggested.top, SWP_NOZORDER | SWP_NOACTIVATE);
        refresh_fonts();
        layout();
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto &tracking = *reinterpret_cast<MINMAXINFO *>(lparam);
        tracking.ptMinTrackSize = POINT{x(970), x(674)};
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: paint(); return 0;
    case WM_DRAWITEM: draw_button(*reinterpret_cast<DRAWITEMSTRUCT *>(lparam)); return TRUE;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        SetTextColor(reinterpret_cast<HDC>(wparam), foreground);
        SetBkColor(reinterpret_cast<HDC>(wparam), input_bg);
        return reinterpret_cast<LRESULT>(input_brush);
    case WM_CTLCOLORSTATIC: {
        const auto id = GetDlgCtrlID(reinterpret_cast<HWND>(lparam));
        const auto dc = reinterpret_cast<HDC>(wparam);
        const bool on_panel = id == GameName || id == GamePath || id == GameInfo || id == ArgumentsLabel ||
                              id == PeerLabel || id == PortLabel || id == PeerHelp;
        SetTextColor(dc, id == StatusLabel && status_error ? error_color :
                         id == Title || id == GameName || id == PeerLabel ? foreground : muted);
        SetBkColor(dc, on_panel ? panel : background);
        return reinterpret_cast<LRESULT>(on_panel ? panel_brush : background_brush);
    }
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case LibraryTab: change_page(Page::Library); break;
        case PeersTab: change_page(Page::Peers); break;
        case ActivityTab: change_page(Page::Activity); break;
        case OpenFolder: choose_game(true); break;
        case PickExecutable: choose_game(false); break;
        case GameList:
            if (HIWORD(wparam) == LBN_SELCHANGE) select_game();
            if (HIWORD(wparam) == LBN_DBLCLK) start_game();
            break;
        case Launch: start_game(); break;
        case AddPeer: edit_peers(false); break;
        case RemovePeer: edit_peers(true); break;
        case PeerList:
            EnableWindow(control(RemovePeer), SendMessageW(control(PeerList), LB_GETCURSEL, 0, 0) != LB_ERR);
            break;
        case OpenLog:
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(window, L"open", log_file.c_str(), nullptr, nullptr, SW_SHOW)) <= 32) {
                status(L"Windows could not open the activity log.", true);
            }
            break;
        }
        return 0;
    case WM_TIMER: poll(); return 0;
    case WM_DESTROY:
        KillTimer(window, 1);
        for (auto font : {body_font, title_font, strong_font}) DeleteObject(font);
        for (auto brush : {panel_brush, input_brush, background_brush}) DeleteObject(brush);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    auto app = reinterpret_cast<App *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        app = static_cast<App *>(reinterpret_cast<const CREATESTRUCTW *>(lparam)->lpCreateParams);
        app->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app) return DefWindowProcW(window, message, wparam, lparam);
    try {
        return app->handle(message, wparam, lparam);
    } catch (const std::exception &error) {
        const auto message_text = wide_text(error.what());
        MessageBoxW(window, message_text.c_str(), app_title, MB_OK | MB_ICONERROR);
        return message == WM_CREATE ? -1 : 0;
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    window_class.lpszClassName = L"SteamEMMainWindow";
    RegisterClassW(&window_class);

    App app;
    const UINT dpi = GetDpiForSystem();
    const auto window = CreateWindowExW(0, window_class.lpszClassName, app_title, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(1120, dpi, 96), MulDiv(740, dpi, 96), nullptr, nullptr, instance, &app);
    if (!window) {
        if (SUCCEEDED(com)) CoUninitialize();
        return 1;
    }
    const BOOL dark_caption = TRUE;
    DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark_caption, sizeof(dark_caption));
    ShowWindow(window, show);
    UpdateWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == WM_KEYDOWN && message.wParam == VK_RETURN &&
            (message.hwnd == GetDlgItem(window, PeerEdit) || message.hwnd == GetDlgItem(window, PortEdit))) {
            SendMessageW(window, WM_COMMAND, AddPeer, 0);
            continue;
        }
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (SUCCEEDED(com)) CoUninitialize();
    return static_cast<int>(message.wParam);
}
