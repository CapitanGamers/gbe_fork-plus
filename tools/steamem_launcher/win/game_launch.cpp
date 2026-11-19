#include "game_launch.h"

#include <algorithm>
#include <fstream>
#include <system_error>

namespace steamem_launcher {

namespace {

WORD executable_machine(const std::filesystem::path &exe)
{
    std::ifstream input(exe, std::ios::binary);
    IMAGE_DOS_HEADER dos{};
    if (!input.read(reinterpret_cast<char *>(&dos), sizeof(dos)) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < static_cast<LONG>(sizeof(dos))) {
        return IMAGE_FILE_MACHINE_UNKNOWN;
    }

    input.seekg(dos.e_lfanew);
    DWORD signature = 0;
    IMAGE_FILE_HEADER header{};
    if (!input.read(reinterpret_cast<char *>(&signature), sizeof(signature)) || signature != IMAGE_NT_SIGNATURE ||
        !input.read(reinterpret_cast<char *>(&header), sizeof(header)) ||
        (header.Characteristics & IMAGE_FILE_DLL) != 0 ||
        (header.Characteristics & IMAGE_FILE_EXECUTABLE_IMAGE) == 0) {
        return IMAGE_FILE_MACHINE_UNKNOWN;
    }

    return header.Machine;
}

bool is_executable_name(const std::filesystem::path &path)
{
    return CompareStringOrdinal(path.extension().c_str(), -1, L".exe", -1, TRUE) == CSTR_EQUAL;
}

} // namespace

void HandleDeleter::operator()(HANDLE handle) const noexcept
{
    CloseHandle(handle);
}

std::vector<std::filesystem::path> find_executables(const std::filesystem::path &folder)
{
    std::vector<std::filesystem::path> executables;
    // Skip reparse points to avoid leaving the selected folder or walking a loop.
    std::error_code error;
    std::filesystem::recursive_directory_iterator end;
    for (std::filesystem::recursive_directory_iterator it(folder, std::filesystem::directory_options::skip_permission_denied, error);
         !error && it != end; it.increment(error)) {
        auto attributes = GetFileAttributesW(it->path().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) continue;
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 || it.depth() >= 5) {
                it.disable_recursion_pending();
            }
        } else if (is_executable_name(it->path())) {
            executables.push_back(it->path());
        }
    }
    if (error) throw std::filesystem::filesystem_error("Cannot scan the selected folder", folder, error);

    std::sort(executables.begin(), executables.end(), [&folder](const auto &a, const auto &b) {
        const auto left = a.lexically_relative(folder).native();
        const auto right = b.lexically_relative(folder).native();
        auto left_depth = std::count(left.begin(), left.end(), L'\\');
        auto right_depth = std::count(right.begin(), right.end(), L'\\');
        if (left_depth != right_depth) return left_depth < right_depth;
        return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    return executables;
}

GameInfo inspect_game(const std::filesystem::path &exe)
{
    GameInfo info{L"Unknown architecture", L"", L""};
    switch (executable_machine(exe)) {
    case IMAGE_FILE_MACHINE_I386: info.architecture = L"Windows x86"; break;
    case IMAGE_FILE_MACHINE_AMD64: info.architecture = L"Windows x64"; break;
    case IMAGE_FILE_MACHINE_ARM64: info.architecture = L"Windows ARM64"; break;
    }

    const auto directory = exe.parent_path();
    auto exists = [&directory](const wchar_t *name) {
        std::error_code error;
        return std::filesystem::is_regular_file(directory / name, error);
    };
    auto add_description = [&info](const wchar_t *name) {
        if (!info.detected_apis.empty()) info.detected_apis += L"  ·  ";
        info.detected_apis += name;
    };
    if (exists(L"steam_api64.dll") || exists(L"steam_api.dll")) {
        add_description(L"Steamworks");
        const auto overlay_config = directory / L"steam_settings" / L"configs.overlay.ini";
        const UINT overlay = GetPrivateProfileIntW(L"overlay::general", L"enable_experimental_overlay", 2,
                                                    overlay_config.c_str());
        if (overlay == 1) info.overlay_setting = L"Local Overlay setting: enabled (Experimental required)";
        else if (overlay == 0) info.overlay_setting = L"Local Overlay setting: disabled";
        else info.overlay_setting = L"Local Overlay setting: use the global configuration";
    }
    if (exists(L"EOSSDK-Win64-Shipping.dll") || exists(L"EOSSDK-Win32-Shipping.dll")) add_description(L"Epic Online Services");
    if (exists(L"XGameRuntime.dll") || exists(L"MicrosoftGame.config")) add_description(L"Xbox GDK");
    if (info.detected_apis.empty()) info.detected_apis = L"No platform files detected beside the executable";
    return info;
}

std::wstring with_server_endpoint(std::wstring options, std::wstring_view endpoint)
{
    for (auto pos = options.find(endpoint_argument); pos != std::wstring::npos;
         pos = options.find(endpoint_argument, pos + endpoint.size())) {
        options.replace(pos, endpoint_argument.size(), endpoint);
    }
    return options;
}

LaunchResult launch_game(const std::filesystem::path &exe, const std::wstring &arguments)
{
    LaunchResult result;
    if (!exe.is_absolute() || !is_executable_name(exe)) {
        result.error = ERROR_INVALID_NAME;
        return result;
    }
    if (executable_machine(exe) == IMAGE_FILE_MACHINE_UNKNOWN) {
        const auto attributes = GetFileAttributesW(exe.c_str());
        result.error = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_BAD_EXE_FORMAT;
        return result;
    }

    std::wstring command_line = L"\"" + exe.native() + L"\"";
    if (!arguments.empty()) command_line += L" " + arguments;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const auto directory = exe.parent_path();
    if (!CreateProcessW(exe.c_str(), command_line.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        directory.c_str(), &startup, &process)) {
        result.error = GetLastError();
        return result;
    }
    CloseHandle(process.hThread);
    result.process.reset(process.hProcess);
    result.process_id = process.dwProcessId;
    return result;
}

std::wstring describe_error(DWORD error)
{
    wchar_t *message = nullptr;
    const auto count = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                      FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0,
                                      reinterpret_cast<wchar_t *>(&message), 0, nullptr);
    std::wstring description = count ? std::wstring(message, count) : L"Unknown Windows error";
    if (message) LocalFree(message);
    const auto last = description.find_last_not_of(L" \r\n\t");
    if (last != std::wstring::npos) description.resize(last + 1);
    return description + L" (" + std::to_wstring(error) + L")";
}

bool has_visible_window(DWORD process_id)
{
    struct Search {
        DWORD process_id;
        bool found;
    } search{process_id, false};

    EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
        auto &search = *reinterpret_cast<Search *>(parameter);
        DWORD owner = 0;
        GetWindowThreadProcessId(window, &owner);
        if (owner == search.process_id && IsWindowVisible(window) && GetWindow(window, GW_OWNER) == nullptr) {
            search.found = true;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.found;
}

} // namespace steamem_launcher
