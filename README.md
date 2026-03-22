# MultiseatProject

True local multiseat on Windows 11 — **no RDP**, real independent sessions,
multiple game instances. Built the same way Aster works internally.

---

## How It Works

```
┌──────────────────────────────────────────────────────────────┐
│                    Your RTX PC                               │
│                                                              │
│  ┌─────────────────────┐  ┌─────────────────────────────┐   │
│  │     SESSION 1       │  │        SESSION 2             │   │
│  │  (your account)     │  │  (brother's account)         │   │
│  │                     │  │                              │   │
│  │  Monitor 1          │  │  Monitor 2                   │   │
│  │  Keyboard 1         │  │  Keyboard 2                  │   │
│  │  Mouse 1            │  │  Mouse 2                     │   │
│  │                     │  │                              │   │
│  │  Game Instance A ◄──┼──┼──► Game Instance B           │   │
│  │  (your save data)   │  │   (his save data)            │   │
│  └─────────────────────┘  └─────────────────────────────┘   │
│                                                              │
│  multiseat_filter.sys  ←  HID filter driver routes input    │
│  termsrv_patch         ←  lifts the "1 session" limit       │
│  multiseat_mutex_hook  ←  allows 2x same game to run        │
└──────────────────────────────────────────────────────────────┘
```

---

## Why No RDP?

RDP adds latency, compression artifacts, and a network stack between the
game and the GPU. Instead we:

1. **Patch termsrv.dll in memory** — remove the consumer Windows limit of
   one concurrent interactive session. The patch is done at runtime with
   `WriteProcessMemory` so nothing on disk changes.
2. **Use undocumented WinStation APIs** (`winsta.dll`) to create a real
   local session that draws directly to the physical framebuffer.
3. The second session is **session-isolated** — it has its own DWM, its
   own desktop, and its own object namespace.

---

## How Multiple Game Instances Work

Windows names objects in two namespaces:

| Name style | Namespace | Isolated per session? |
|---|---|---|
| `"GameMutex"` | `\Sessions\N\BaseNamedObjects\` | ✅ Yes |
| `"Global\GameMutex"` | `\BaseNamedObjects\` | ❌ No — machine-wide |

Most games use `Global\` prefixed mutexes for single-instance checks.
We hook `CreateMutexW/A`, `OpenMutexW/A`, `CreateEventW`, and
`CreateSemaphoreW` in Seat 2's processes and strip the `Global\` prefix
so the mutex becomes session-local. Both seats then get their own copy.

Injection methods (pick one):
- **IFEO / AppVerifier shim** — register in registry, Windows auto-loads
  the hook DLL before the game starts (preferred, no timing issues)
- **CreateRemoteThread** — inject into already-running processes

---

## Prerequisites

| Tool | Where |
|---|---|
| Visual Studio 2022 | visualstudio.microsoft.com |
| Windows Driver Kit 11 | aka.ms/wdk |
| Admin / SYSTEM account | Required for driver + session creation |

---

## Build Instructions

### Step 1 — Enable Test Signing (dev only)

```cmd
bcdedit /set testsigning on
reboot
```

For release: get an EV code-signing cert and submit to Microsoft attestation.

---

### Step 2 — Build the Kernel Driver

1. Open Visual Studio → New Project → **Kernel Mode Driver (KMDF)**
2. Add `driver/multiseat_filter.c`
3. Add `shared/protocol.h` to include paths
4. Target: Windows 11 (10.0.22000), KMDF 1.33, x64
5. Build

```cmd
signtool sign /fd SHA256 /a multiseat_filter.sys
pnputil /add-driver multiseat_filter.inf /install
```

---

### Step 3 — Build the Mutex Hook DLL

Open **x64 Native Tools Command Prompt for VS 2022**:

```cmd
cd service\mutex_hook
cl /LD /W4 /O2 mutex_hook.c ^
   /link /DLL /OUT:multiseat_mutex_hook.dll kernel32.lib
copy multiseat_mutex_hook.dll ..\..\ui\
```

---

### Step 4 — Build the Service

```cmd
cd service
cl /W4 /O2 main.c session_manager.c device_manager.c ^
   display_manager.c dll_injector.c termsrv_patch.c ^
   /link advapi32.lib wtsapi32.lib winsta.lib netapi32.lib ^
   setupapi.lib hid.lib userenv.lib shlwapi.lib ole32.lib ^
   oleaut32.lib wbemuuid.lib ^
   /OUT:MultiseatSvc.exe /SUBSYSTEM:CONSOLE
```

Install (run as admin):
```cmd
MultiseatSvc.exe --install
sc start MultiseatSvc
```

---

### Step 5 — Build the Control Panel

```cmd
cd ui
cl /W4 /O2 main.c ^
   /link user32.lib gdi32.lib comctl32.lib wtsapi32.lib ^
   advapi32.lib setupapi.lib hid.lib shlwapi.lib ^
   /OUT:MultiseatCtrlPanel.exe /SUBSYSTEM:WINDOWS
```

---

## Using the Control Panel

Run **MultiseatCtrlPanel.exe as Administrator**.

### Tab 1 — Devices
- Lists all detected keyboards and mice
- Select your keyboard → "Seat 1" → Assign
- Select your brother's keyboard → "Seat 2" → Assign
- Same for mice

### Tab 2 — Monitors
- Select your monitor → "Seat 1" → Assign
- Select your brother's monitor → "Seat 2" → Assign

### Tab 3 — Account
- Enter a username (e.g. `Seat2User`) and password for the second account
- The account is created automatically if it doesn't exist
- Click **Start Seat 2**

What happens behind the scenes:
1. `termsrv.dll` is patched in memory to allow concurrent local sessions
2. A local Windows user account is created via `NetUserAdd`
3. `LogonUser` + `CreateProcessAsUser` launches `userinit.exe` in a new session
4. The session is attached to your second monitor
5. The mutex hook DLL is injected into all processes in that session

### Tab 4 — Games
- Type the game's `.exe` name (e.g. `RocketLeague.exe`)
- Click **Add**
- This registers it in IFEO so the mutex hook DLL loads automatically
- Both seats can now run their own instance

---

## File Structure

```
MultiseatProject/
├── shared/
│   └── protocol.h                 ← IOCTLs + shared structs
├── driver/
│   ├── multiseat_filter.c         ← KMDF HID filter driver
│   └── multiseat_filter.inf       ← Driver installer
├── service/
│   ├── main.c                     ← Windows service entry
│   ├── termsrv_patch.c/h          ← ★ Patches termsrv.dll in memory
│   ├── session_manager.c/h        ← ★ Creates real local sessions
│   ├── device_manager.c/h         ← HID device enumeration
│   ├── display_manager.c/h        ← Monitor assignment
│   ├── dll_injector.c/h           ← ★ Injects hook DLL into session
│   └── mutex_hook/
│       └── mutex_hook.c           ← ★ Hooks CreateMutex → allows 2x games
└── ui/
    └── main.c                     ← Win32 control panel (4 tabs)
```

---

## Known Limitations & Roadmap

- [ ] **Kernel-mode display binding** — full physical GPU output → session
      binding needs `dxgkrnl.sys` calls (display_driver component, next phase)
- [ ] **Named pipe input relay** — driver writes raw HID reports to a ring
      buffer; service reads and calls `SendInput` in the correct session
- [ ] **Per-seat audio** — needs a virtual audio driver or USB audio per seat
- [ ] **Anti-cheat games** (EAC, BattlEye, Vanguard, nProtect) — kernel
      anti-cheats scan for injected DLLs and session anomalies.
      The IFEO approach has a higher survival rate than `CreateRemoteThread`.
- [ ] **Pattern scanner** — `termsrv_patch.c` includes pattern-matching that
      adapts to Windows updates; run the scanner on a fresh install if
      patterns change
- [ ] **Installer** (WiX) for one-click setup
