# SteamEM

**Local multiplayer for compatible Steam games over LAN or a private VPN.**

SteamEM implements selected Steamworks services locally so players can discover one another, host a lobby, and join supported games without reaching Steam's online services. Each player runs their own copy of the game. The LAN or VPN provides the route between them.

[Quick start](#quick-start) · [How it works](#how-it-works) · [Build from source](#build-from-source) · [Troubleshooting](#troubleshooting) · [Persian guide](README.fa.md)

## What works today

| Component | Scope |
| --- | --- |
| Steam compatibility | Local peer discovery, lobbies, invites, and messaging for games that use supported Steamworks interfaces |
| Windows overlay | `Shift+Tab`, separate IPv4 and port fields, optional LAN discovery, and a right-to-left Persian interface |
| Windows launcher | Game selection, executable architecture detection, saved peer addresses, and process logs |
| Epic LAN | Launches game clients and dedicated servers that already support LAN or direct IP |

Compatibility depends on the game, its version, and the interfaces it uses. Games that require a publisher backend for sign-in, session creation, or matchmaking need title-specific support. EOS emulation and Xbox multiplayer are outside the current build.

## Quick start

1. **Prepare the game.** Use the same version of the game on both PCs and choose the SteamEM build for the game's architecture. Use `experimental/x64/steam_api64.dll` or `experimental/x86/steam_api.dll` for the Windows overlay.
2. **Set up Steamworks interfaces.** Back up the original game DLL. Run the matching [interface generator](post_build/README.generate_interfaces.md) against that original DLL, then save `steam_interfaces.txt` under `steam_settings/` next to the DLL. Put the matching SteamEM DLL in the original DLL's location. Use the game's App ID in `steam_settings/steam_appid.txt` if the game needs it. [Example configuration files](post_build/steam_settings.EXAMPLE/)
3. **Enable the overlay.** Create or update `steam_settings/configs.overlay.ini`.

   ```ini
   [overlay::general]
   enable_experimental_overlay=1
   ```

   To use Persian, set `language=persian` under `[user::general]` in `steam_settings/configs.user.ini`. Give each player a distinct local player ID.

4. **Find a peer.** Connect both PCs to the same LAN or private VPN, start the game, and press `Shift+Tab`. Open **Network**, enter the other player's IPv4 address and the **UDP** port shown in their overlay, then select **Find friend**. Leave the port blank for `47584`. You can add peers while the game is running.
5. **Start the session.** Create or join a lobby through the game's multiplayer menu. To manage addresses before launch, open **Peers** in `SteamEM_x64.exe` or `SteamEM_x86.exe` under `tools/steamem_launcher/`.

![SteamEM's Windows overlay showing Friends, Network, Achievements, History, and Settings](docs/overlay-preview.png)

## How it works

| Stage | What SteamEM does |
| --- | --- |
| API calls | The game loads SteamEM's Steam API library. The library dispatches Steamworks calls to compatible local interfaces using the game's settings and App ID. |
| Peer discovery | SteamEM sends UDP announcements directly to saved IPv4 endpoints. Peers reply with their player and game identifiers and the TCP port they advertise. |
| LAN discovery | The optional toggle adds broadcasts and legacy configured destinations. It starts off by default in a game session and can be changed while playing. Established connections stay up. |
| Multiplayer | SteamEM handles supported lobby and peer traffic over UDP or TCP as appropriate. Messages use Protocol Buffers. The game still controls its own session flow. |
| Overlay | InGameOverlay attaches to the rendering path. Dear ImGui draws the panel using cached display data when Steam callbacks are busy. |

Direct discovery uses the existing route to the peer's IP, so it can work on a routed VPN that does not forward LAN broadcasts. On Windows, the launcher and overlay share `%APPDATA%\SteamEM\custom_broadcasts.txt`. Edits are picked up in a running game. The Experimental Windows build also applies LAN destination rules to selected Winsock calls.

[Network implementation](dll/network.cpp) · [Message schema](dll/net.proto) · [Lobby implementation](dll/steam_matchmaking.cpp) · [Overlay source](overlay_experimental/steam_overlay.cpp) · [Launcher source](tools/steamem_launcher/win/)

### Epic LAN

Open `epic/EpicLAN_x64.exe` or `epic/EpicLAN_x86.exe` and select the original game client or server in **Library**. For a game with a LAN browser, join through the game itself. For a game that accepts a direct-connect launch argument, save and select the server under **Servers**, then enter `{endpoint}` where the game expects the address in **Launch options**. The launcher substitutes `IPv4:port` using the **game server's** port and starts the original executable. [Official direct-connect example](https://dev.epicgames.com/documentation/en-us/unreal-engine/setting-up-dedicated-servers-in-unreal-engine#connectclientstodedicatedserver)

## Build from source

### Dependencies

GBE provides the build helpers and dependency archives in its `third-party/...` branches. If your checkout has no populated `third-party/` directory, run the following from the SteamEM root in PowerShell. [GBE upstream](https://github.com/Detanup01/gbe_fork) · [Submodule definitions](.gitmodules)

```powershell
git clone --recurse-submodules https://github.com/Detanup01/gbe_fork.git ../gbe-upstream
if ($LASTEXITCODE -ne 0) { throw 'Could not clone upstream dependencies.' }

robocopy '..\gbe-upstream\third-party' '.\third-party' /E /XD .git /XF .git
if ($LASTEXITCODE -ge 8) { throw 'Could not copy upstream dependencies.' }
```

On Linux, copy the same directory with `rsync -a --exclude=.git ../gbe-upstream/third-party/ ./third-party/`. The inherited GitHub Actions workflows check out dependency branches from the hosting repository. A source-only mirror needs those branches or dependency checkout steps pointing to GBE upstream. [Windows workflow](.github/workflows/emu-build-all-win.yml)

### Windows

Install Visual Studio 2026 or compatible Build Tools with Desktop development with C++ and the Windows SDK. From Command Prompt, build the dependencies and then the project.

```bat
build_win_premake.bat --deps
build_win_premake.bat
```

The script builds Debug and Release configurations for x64 and Win32. To include ARM64, install its toolchain and pass `--arm64` to both commands. Release output is under `build/win/vs2026/release/`.

| Output directory | Contents |
| --- | --- |
| `regular/<arch>/` | Steam API library |
| `experimental/<arch>/` | Steam API library with the Windows overlay |
| `steamclient_experimental/` | Alternate Steam Client loader and library |
| `tools/steamem_launcher/` | SteamEM desktop launcher |
| `tools/generate_interfaces/` | Interface generator |
| `tools/lobby_connect/` | Lobby discovery and join utility |
| `epic/` | Epic LAN launcher |

### Linux

The upstream Premake and GNU Make build is available through `bash build_linux_premake.sh --deps`. A full build requires toolchains and development dependencies for x64, x86, and ARM64. The SteamEM launcher and overlay changes documented here were validated on Windows. [Linux build script](build_linux_premake.sh) · [Development notes](dev.notes/)

## Troubleshooting

| Symptom | Check |
| --- | --- |
| `Shift+Tab` does not open the overlay | Confirm the game's DLL architecture, the Experimental build, and `enable_experimental_overlay=1`. The game's save folder contains `overlay-startup.log` with renderer and first-frame status. |
| A peer stays on **Searching** | Check the peer's VPN IPv4 and displayed UDP port, the route between systems, and the firewall rules for that private network. |
| A peer is visible but cannot join | Compare game versions, App IDs, unique player IDs, and the game's selected multiplayer mode. |
| Epic LAN launches the game but it cannot connect | Check that the title supports native LAN or direct IP, the host is running, and the launch argument matches the game's documented syntax. |

Local Windows validation covered overlay rendering on x64 and x86. Two x64 clients discovered one another through a manually added address, joined the same lobby, and delivered a chat message. Automatic discovery and its overlay toggle were checked separately. Actual compatibility needs to be verified for each title.

## Upstream and license

SteamEM is based on [GBE Fork](https://github.com/Detanup01/gbe_fork), which builds on the original [Goldberg Emulator](https://gitlab.com/Mr_Goldberg/goldberg_emulator). The overlay uses [InGameOverlay](https://github.com/Nemirtingas/ingame_overlay) and [Dear ImGui](https://github.com/ocornut/imgui). Windows hooks use [Microsoft Detours](https://github.com/microsoft/Detours), and network messages use [Protocol Buffers](https://protobuf.dev/overview/).

[EmulatorLauncherRepository](https://gitlab.com/Nemirtingas/emulatorlauncherrepository/-/raw/main/README.md) and [EOS Reimagined](https://github.com/anishgoyal1108/EOS-Reimagined/blob/main/README.md#how-it-works) were reviewed as references for launcher design and local EOS networking. Epic LAN is built from this repository's Win32 launcher code.

[LGPL-3.0-or-later license](LICENSE) · [Third-party notices](CREDITS.md) · [Upstream changelog](CHANGELOG.md)
