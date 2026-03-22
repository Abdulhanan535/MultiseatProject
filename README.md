# MultiseatProject

**Run two players on one PC** — one plays locally, the other connects via [Moonlight](https://moonlight-stream.org/) through [Sunshine](https://github.com/LizardByte/Sunshine). Each player gets their own Windows user account with separate saves, settings, and game sessions.

## What It Does

1. **Creates a second Windows session** (Seat2User) automatically
2. **Patches `termsrv.dll`** in memory to allow concurrent sessions on consumer Windows
3. **Registers game mutex hooks** so both players can run the same game simultaneously
4. Your friend connects from their PC using **Moonlight** → gets a full Windows desktop

## Requirements

- Windows 11 Pro
- [Sunshine](https://github.com/LizardByte/Sunshine) installed on the host PC
- [Moonlight](https://moonlight-stream.org/) on the remote player's PC
- Run as **Administrator**

## Quick Start

1. Download `MultiseatCtrlPanel.exe` and `multiseat_mutex_hook.dll` from [Releases](../../releases)
2. Place both files in the same folder
3. Install [Sunshine](https://github.com/LizardByte/Sunshine) if not already installed
4. Right-click `MultiseatCtrlPanel.exe` → **Run as administrator**
5. Set a username and password for the second player
6. Click **Start Seat 2**
7. Your friend opens Moonlight, connects to your PC's IP → full desktop!

## Games Tab

Register game executables (e.g. `game.exe`) so both players can run their own instance. This hooks the game's mutex check via IFEO (Image File Execution Options), preventing the "game already running" error.

## How It Works

- **termsrv.dll patch**: Modifies Windows Terminal Services in memory to remove the 1-session limit
- **Silent RDP loopback**: Creates the second session via `mstsc /v:127.0.0.2` in the background
- **IFEO mutex hook**: Registers a DLL that intercepts `CreateMutexW` to allow duplicate game instances
- **No kernel driver needed**: Everything runs in user mode

## Building

The project builds automatically via GitHub Actions. To build locally:

```cmd
:: Build the hook DLL
cd service\mutex_hook
cl /LD /W4 /O2 /D UNICODE /D _UNICODE mutex_hook.c /link /DLL /OUT:multiseat_mutex_hook.dll kernel32.lib

:: Build the control panel (single exe)
cd ui
cl /W4 /O2 /D UNICODE /D _UNICODE main.c ^
  ..\service\session_manager.c ..\service\dll_injector.c ..\service\termsrv_patch.c ^
  /I ..\service ^
  /link user32.lib gdi32.lib comctl32.lib advapi32.lib wtsapi32.lib netapi32.lib userenv.lib ^
  /OUT:MultiseatCtrlPanel.exe /SUBSYSTEM:WINDOWS
```

## Project Structure

```
MultiseatProject/
├── ui/
│   └── main.c                    # Control panel (2 tabs: Account + Games)
├── service/
│   ├── session_manager.c/h       # Session creation, user accounts, RDP loopback
│   ├── dll_injector.c/h          # DLL injection + IFEO setup
│   ├── termsrv_patch.c/h         # In-memory termsrv.dll patching
│   └── mutex_hook/
│       └── mutex_hook.c          # Hook DLL (intercepts CreateMutexW)
└── .github/workflows/build.yml   # CI build
```

## License

Use at your own risk. Patching `termsrv.dll` may violate Microsoft's license agreement.
