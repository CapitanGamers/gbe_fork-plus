#pragma once

#include <windows.h>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace steamem_launcher {

struct HandleDeleter {
    void operator()(HANDLE handle) const noexcept;
};

using ProcessHandle = std::unique_ptr<void, HandleDeleter>;

struct LaunchResult {
    ProcessHandle process;
    DWORD process_id = 0;
    DWORD error = ERROR_SUCCESS;
};

struct GameInfo {
    std::wstring architecture;
    std::wstring detected_apis;
    std::wstring overlay_setting;
};

std::vector<std::filesystem::path> find_executables(const std::filesystem::path &folder);
GameInfo inspect_game(const std::filesystem::path &exe);
inline constexpr std::wstring_view endpoint_argument = L"{endpoint}";
std::wstring with_server_endpoint(std::wstring options, std::wstring_view endpoint);
LaunchResult launch_game(const std::filesystem::path &exe, const std::wstring &arguments);
std::wstring describe_error(DWORD error);
bool has_visible_window(DWORD process_id);

} // namespace steamem_launcher
