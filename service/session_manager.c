// ================================================================
//  MultiseatProject -- service/session_manager.c
//
//  Creates REAL local Windows sessions using undocumented
//  WinStation APIs from winsta.dll — no RDP, no network.
//
//  How a true local session is created:
//
//    1. LogonUser()              → get a user token
//    2. WinStationCreateDynamic  → allocate a new WinStation
//    3. CreateProcessAsUser()    → launch userinit.exe in that
//                                  session (triggers full logon)
//    4. WinStationConnectW()     → attach the WinStation to a
//                                  physical display/seat
//
//  The session runs completely independently:
//  - Its own desktop, start menu, registry hive (HKCU)
//  - Its own named-object namespace (session-local BaseNamedObjects)
//    → two instances of ANY game can run, one per seat, because
//      single-instance mutexes are session-scoped by default.
//
// ================================================================

#include <windows.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <shlwapi.h>
#include <lm.h>
#include <stdio.h>
#include "session_manager.h"
#include "termsrv_patch.h"

// Inline from removed shared/protocol.h
#define MAX_SEATS  4
typedef struct _SEAT_INFO {
    ULONG  SeatIndex;
    ULONG  SessionId;
    WCHAR  MonitorDevice[64];
    BOOL   Active;
} SEAT_INFO;

#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "winsta.lib")
#pragma comment(lib, "netapi32.lib")

// ── Undocumented WinStation API types ───────────────────────────
// Loaded dynamically from winsta.dll

typedef BOOL (WINAPI* PFN_WinStationCreateDynamic)(
    HANDLE  hServer,
    DWORD*  pSessionId);

typedef BOOL (WINAPI* PFN_WinStationConnectW)(
    HANDLE  hServer,
    ULONG   LogonId,          // session to connect (source)
    ULONG   TargetLogonId,    // seat/winstation to connect to
    PWSTR   pPassword,
    BOOL    bWait);

typedef BOOL (WINAPI* PFN_WinStationDisconnect)(
    HANDLE  hServer,
    ULONG   LogonId,
    BOOL    bWait);

typedef BOOL (WINAPI* PFN_WinStationSetInformationW)(
    HANDLE  hServer,
    ULONG   SessionId,
    ULONG   WinStationInformationClass,
    PVOID   pWinStationInformation,
    ULONG   WinStationInformationLength);

// WinStationInformationClass value for the display config
#define WinStationVideoData  0x1C

// ── Globals ──────────────────────────────────────────────────────
static SEAT_INFO  g_Seats[MAX_SEATS];
static HMODULE    g_hWinSta = NULL;

static PFN_WinStationCreateDynamic    pfnCreateDynamic    = NULL;
static PFN_WinStationConnectW         pfnConnect          = NULL;
static PFN_WinStationDisconnect       pfnDisconnect       = NULL;
static PFN_WinStationSetInformationW  pfnSetInformation   = NULL;

// ── Forward declarations ─────────────────────────────────────────
static BOOL LoadWinStaAPIs(void);
static BOOL EnsureLocalUser(LPCWSTR username, LPCWSTR password);
static ULONG FindSessionByUser(LPCWSTR username);
static BOOL LaunchLogon(HANDLE hToken, DWORD sessionId);
static BOOL EnableDebugPrivilege(void);
static BOOL LaunchSunshineInSession(DWORD sessionId, LPCWSTR username, LPCWSTR password);

// ── AppInit_DLLs for global mutex hook ───────────────────────────
// Sets the hook DLL to load into every GUI process system-wide.
// The DLL itself checks session ID and only hooks in secondary sessions.
static void EnableGlobalMutexHook(void)
{
    WCHAR dllPath[MAX_PATH];
    GetModuleFileNameW(NULL, dllPath, MAX_PATH);
    WCHAR* lastSlash = wcsrchr(dllPath, L'\\');
    if (lastSlash) wcscpy_s(lastSlash + 1, MAX_PATH - (lastSlash - dllPath + 1),
                            L"multiseat_mutex_hook.dll");

    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Windows",
            0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, L"AppInit_DLLs", 0, REG_SZ,
            (BYTE*)dllPath, (DWORD)((wcslen(dllPath) + 1) * sizeof(WCHAR)));
        DWORD one = 1, zero = 0;
        RegSetValueExW(hKey, L"LoadAppInit_DLLs", 0, REG_DWORD, (BYTE*)&one, 4);
        RegSetValueExW(hKey, L"RequireSignedAppInit_DLLs", 0, REG_DWORD, (BYTE*)&zero, 4);
        RegCloseKey(hKey);
        printf("[SessionMgr] Global mutex hook enabled via AppInit_DLLs\n");
    }
}

static void DisableGlobalMutexHook(void)
{
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Windows",
            0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        WCHAR empty[] = L"";
        RegSetValueExW(hKey, L"AppInit_DLLs", 0, REG_SZ, (BYTE*)empty, sizeof(empty));
        DWORD zero = 0;
        RegSetValueExW(hKey, L"LoadAppInit_DLLs", 0, REG_DWORD, (BYTE*)&zero, 4);
        RegCloseKey(hKey);
        printf("[SessionMgr] Global mutex hook disabled\n");
    }
}

// ================================================================
//  SessionManager_Init
// ================================================================
BOOL SessionManager_Init(void)
{
    RtlZeroMemory(g_Seats, sizeof(g_Seats));

    if (!EnableDebugPrivilege()) {
        printf("[SessionMgr] WARNING: Could not acquire SeDebugPrivilege. "
               "Run as SYSTEM or elevated admin.\n");
    }

    if (!LoadWinStaAPIs()) {
        printf("[SessionMgr] ERROR: Could not load WinStation APIs\n");
        return FALSE;
    }

    // Seat 0 = current console session (whoever ran the installer)
    g_Seats[0].SeatIndex = 0;
    g_Seats[0].SessionId = WTSGetActiveConsoleSessionId();
    g_Seats[0].Active    = TRUE;

    printf("[SessionMgr] Seat 0 = Session %lu (console)\n",
           g_Seats[0].SessionId);

    // Apply termsrv.dll patches so Windows allows >1 concurrent local session
    if (!TermsrvPatch_Apply()) {
        printf("[SessionMgr] WARNING: termsrv patch incomplete. "
               "Second session may be refused.\n");
    }

    return TRUE;
}

// ================================================================
//  LaunchSunshineInSession
//  Uses schtasks /IT to run Sunshine interactively in Seat2's session.
//  This works without SYSTEM privileges (admin is enough).
// ================================================================
static BOOL LaunchSunshineInSession(DWORD sessionId, LPCWSTR username, LPCWSTR password)
{
    (void)sessionId; // session is implicit — /IT runs in user's interactive session

    // Find Sunshine install path
    WCHAR sunPath[MAX_PATH] = {0};
    WCHAR testPaths[][MAX_PATH] = {
        L"C:\\Program Files\\Sunshine\\sunshine.exe",
        L"C:\\Program Files (x86)\\Sunshine\\sunshine.exe",
    };
    for (int i = 0; i < 2; i++) {
        if (GetFileAttributesW(testPaths[i]) != INVALID_FILE_ATTRIBUTES) {
            wcscpy_s(sunPath, MAX_PATH, testPaths[i]);
            break;
        }
    }
    if (!sunPath[0]) {
        printf("[SessionMgr] Sunshine not found, skipping auto-launch\n");
        return FALSE;
    }

    // Write a fully isolated config for Seat2
    WCHAR configDir[MAX_PATH];
    swprintf_s(configDir, MAX_PATH, L"C:\\ProgramData\\MultiseatProject\\sunshine_seat2");
    CreateDirectoryW(L"C:\\ProgramData\\MultiseatProject", NULL);
    CreateDirectoryW(configDir, NULL);

    WCHAR configPath[MAX_PATH];
    swprintf_s(configPath, MAX_PATH, L"%ws\\sunshine.conf", configDir);

    HANDLE hFile = CreateFileW(configPath, GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        char configDirA[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, configDir, -1, configDirA, MAX_PATH, NULL, NULL);
        char config[2048];
        sprintf_s(config, sizeof(config),
            "port = 47990\n"
            "sunshine_name = Seat2\n"
            "min_log_level = info\n"
            "origin_web_ui_allowed = lan\n"
            "credentials_file = %s\\credentials.json\n"
            "file_state = %s\\state.json\n"
            "log_path = %s\\sunshine.log\n"
            "pkey = %s\\key.pem\n"
            "cert = %s\\cert.pem\n",
            configDirA, configDirA, configDirA, configDirA, configDirA);
        for (char* p = config; *p; p++) {
            if (*p == '\\') *p = '/';
        }
        DWORD bw;
        WriteFile(hFile, config, (DWORD)strlen(config), &bw, NULL);
        CloseHandle(hFile);
        printf("[SessionMgr] Sunshine config written to %ws\n", configPath);
    }

    // Use schtasks to run Sunshine in Seat2User's interactive session.
    // /IT = run only when user is logged on (interactive), so it runs
    // in their session, not ours.
    // Delete any old task first
    WCHAR delCmd[512];
    swprintf_s(delCmd, _countof(delCmd),
        L"cmd.exe /c schtasks /delete /tn MultiseatSunshine /f 2>nul");
    STARTUPINFOW si0 = { sizeof(si0) };
    si0.dwFlags = STARTF_USESHOWWINDOW;
    si0.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi0 = {0};
    if (CreateProcessW(NULL, delCmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si0, &pi0)) {
        WaitForSingleObject(pi0.hProcess, 3000);
        CloseHandle(pi0.hProcess); CloseHandle(pi0.hThread);
    }

    // Create the task
    WCHAR createCmd[1024];
    swprintf_s(createCmd, _countof(createCmd),
        L"cmd.exe /c schtasks /create /tn MultiseatSunshine "
        L"/tr \"\\\"%ws\\\" \\\"%ws\\\"\" "
        L"/sc ONCE /st 00:00 /ru %ws /rp %ws /IT /f",
        sunPath, configPath, username, password);
    STARTUPINFOW si1 = { sizeof(si1) };
    si1.dwFlags = STARTF_USESHOWWINDOW;
    si1.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi1 = {0};
    if (!CreateProcessW(NULL, createCmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si1, &pi1)) {
        printf("[SessionMgr] Failed to create Sunshine task: %lu\n", GetLastError());
        return FALSE;
    }
    WaitForSingleObject(pi1.hProcess, 5000);
    CloseHandle(pi1.hProcess); CloseHandle(pi1.hThread);

    // Run the task now
    WCHAR runCmd[256];
    swprintf_s(runCmd, _countof(runCmd),
        L"cmd.exe /c schtasks /run /tn MultiseatSunshine");
    STARTUPINFOW si2 = { sizeof(si2) };
    si2.dwFlags = STARTF_USESHOWWINDOW;
    si2.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi2 = {0};
    if (CreateProcessW(NULL, runCmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si2, &pi2)) {
        WaitForSingleObject(pi2.hProcess, 5000);
        CloseHandle(pi2.hProcess); CloseHandle(pi2.hThread);
        printf("[SessionMgr] Sunshine task started in Seat2User's session, port 47990\n");
    }

    return TRUE;
}

// ================================================================
//  SessionManager_CreateSeat
//  Creates a real, independent local Windows session.
// ================================================================
BOOL SessionManager_CreateSeat(
    ULONG   seatIndex,
    LPCWSTR username,
    LPCWSTR password,
    LPCWSTR monitorDeviceName)   // e.g. L"\\\\.\\DISPLAY2"
{
    if (seatIndex == 0 || seatIndex >= MAX_SEATS) return FALSE;
    if (g_Seats[seatIndex].Active) {
        printf("[SessionMgr] Seat %lu already active.\n", seatIndex);
        return TRUE;
    }

    printf("[SessionMgr] Creating seat %lu (user=%ws, monitor=%ws)\n",
           seatIndex, username, monitorDeviceName);

    // ── 1. Make sure the local user account exists ───────────────
    if (!EnsureLocalUser(username, password)) {
        printf("[SessionMgr] Failed to ensure user account [%ws]\n", username);
        return FALSE;
    }

    // ── 2. Check if the user already has an active session ───────
    ULONG existingSession = FindSessionByUser(username);
    if (existingSession != (ULONG)-1) {
        printf("[SessionMgr] Found existing session %lu for [%ws]\n",
               existingSession, username);
        g_Seats[seatIndex].SessionId = existingSession;
        g_Seats[seatIndex].Active    = TRUE;
        printf("[SessionMgr] Seat %lu ready -> session %lu\n", seatIndex, existingSession);
        return TRUE;
    }

    // ── 3. Log the user on to create a new session ───────────────
    HANDLE hToken = NULL;
    if (!LogonUserW(
            (LPWSTR)username,
            L".",              // local machine
            (LPWSTR)password,
            LOGON32_LOGON_INTERACTIVE,
            LOGON32_PROVIDER_DEFAULT,
            &hToken)) {
        printf("[SessionMgr] LogonUser failed: %lu\n", GetLastError());
        return FALSE;
    }

    // ── 4. Try WinStation API or RDP loopback ─────────────────────
    DWORD newSessionId = (DWORD)-1;

    if (pfnCreateDynamic) {
        if (pfnCreateDynamic(WTS_CURRENT_SERVER_HANDLE, &newSessionId)) {
            printf("[SessionMgr] Dynamic WinStation: session %lu\n", newSessionId);
            SetTokenInformation(hToken, TokenSessionId, &newSessionId, sizeof(DWORD));
            if (!LaunchLogon(hToken, newSessionId)) {
                printf("[SessionMgr] LaunchLogon failed: %lu\n", GetLastError());
                CloseHandle(hToken);
                return FALSE;
            }
        }
    }

    HANDLE hMstsc = NULL;

    if (newSessionId == (DWORD)-1) {
        // Consumer Windows: RDP loopback creates a real separate session.
        printf("[SessionMgr] Using RDP loopback for session creation\n");
        CloseHandle(hToken);  // done with this, RDP will handle logon

        // Enable RDP
        HKEY hKey;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                L"SYSTEM\\CurrentControlSet\\Control\\Terminal Server",
                0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
            DWORD val = 0;
            RegSetValueExW(hKey, L"fDenyTSConnections", 0, REG_DWORD, (BYTE*)&val, 4);
            val = 0;
            RegSetValueExW(hKey, L"fSingleSessionPerUser", 0, REG_DWORD, (BYTE*)&val, 4);
            RegCloseKey(hKey);
        }

        // Save credentials for silent login
        WCHAR cmd[512];
        swprintf_s(cmd, _countof(cmd),
            L"cmd.exe /c cmdkey /generic:TERMSRV/127.0.0.2 /user:%ws /pass:%ws",
            username, password);
        STARTUPINFOW si = { sizeof(si) };
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi = { 0 };
        if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE,
                CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 5000);
            CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        }

        // Launch mstsc minimized — we'll kill it once the session appears
        WCHAR mstsc[256];
        swprintf_s(mstsc, _countof(mstsc),
            L"mstsc /v:127.0.0.2 /w:800 /h:600");
        STARTUPINFOW si2 = { sizeof(si2) };
        si2.dwFlags = STARTF_USESHOWWINDOW;
        si2.wShowWindow = SW_MINIMIZE;
        PROCESS_INFORMATION pi2 = { 0 };
        if (!CreateProcessW(NULL, mstsc, NULL, NULL, FALSE,
                0, NULL, NULL, &si2, &pi2)) {
            printf("[SessionMgr] Failed to launch mstsc: %lu\n", GetLastError());
            return FALSE;
        }
        printf("[SessionMgr] mstsc launched, waiting for session...\n");
        hMstsc = pi2.hProcess;
        CloseHandle(pi2.hThread);
    } else {
        CloseHandle(hToken);
    }

    // ── 6. Wait for the session to appear in the WTS list ────────
    for (int i = 0; i < 120; i++) {
        Sleep(500);
        ULONG sid = FindSessionByUser(username);
        if (sid != (ULONG)-1) {
            newSessionId = sid;
            break;
        }
    }

    // Kill mstsc now that the session is established (session stays alive)
    if (hMstsc) {
        TerminateProcess(hMstsc, 0);
        CloseHandle(hMstsc);
        printf("[SessionMgr] mstsc terminated (session persists)\n");
    }

    if (newSessionId == (DWORD)-1) {
        printf("[SessionMgr] Timed out waiting for session\n");
        return FALSE;
    }

    printf("[SessionMgr] Session %lu created for [%ws]\n", newSessionId, username);
    g_Seats[seatIndex].SessionId = newSessionId;
    g_Seats[seatIndex].Active    = TRUE;

    // ── 7. Enable global mutex hook for all apps in Seat 2 ────────
    EnableGlobalMutexHook();

    // ── 8. Launch Sunshine in the new session on port 47990 ──────
    LaunchSunshineInSession(newSessionId, username, password);

    printf("[SessionMgr] Seat %lu ready -> session %lu\n",
           seatIndex, g_Seats[seatIndex].SessionId);
    return TRUE;
}



// ================================================================
//  SessionManager_GetSessionId
// ================================================================
ULONG SessionManager_GetSessionId(ULONG seatIndex)
{
    if (seatIndex >= MAX_SEATS) return (ULONG)-1;
    return g_Seats[seatIndex].Active ? g_Seats[seatIndex].SessionId : (ULONG)-1;
}

// ================================================================
//  SessionManager_TerminateSeat
// ================================================================
BOOL SessionManager_TerminateSeat(ULONG seatIndex)
{
    if (!g_Seats[seatIndex].Active) return TRUE;
    ULONG sid = g_Seats[seatIndex].SessionId;

    if (pfnDisconnect) pfnDisconnect(WTS_CURRENT_SERVER_HANDLE, sid, FALSE);
    WTSLogoffSession(WTS_CURRENT_SERVER_HANDLE, sid, FALSE);

    DisableGlobalMutexHook();

    g_Seats[seatIndex].Active    = FALSE;
    g_Seats[seatIndex].SessionId = (ULONG)-1;
    printf("[SessionMgr] Seat %lu terminated\n", seatIndex);
    return TRUE;
}

// ================================================================
//  SessionManager_Shutdown
// ================================================================
void SessionManager_Shutdown(void)
{
    for (ULONG i = 1; i < MAX_SEATS; i++) {
        if (g_Seats[i].Active) SessionManager_TerminateSeat(i);
    }
    if (g_hWinSta) { FreeLibrary(g_hWinSta); g_hWinSta = NULL; }
    TermsrvPatch_Revert();
}



// ================================================================
//  EnsureLocalUser
//  Creates a local account using NetUserAdd if it doesn't exist.
// ================================================================
static BOOL EnsureLocalUser(LPCWSTR username, LPCWSTR password)
{
    USER_INFO_1 ui = { 0 };
    ui.usri1_name     = (LPWSTR)username;
    ui.usri1_password = (LPWSTR)password;
    ui.usri1_priv     = USER_PRIV_USER;
    ui.usri1_flags    = UF_SCRIPT | UF_NORMAL_ACCOUNT | UF_DONT_EXPIRE_PASSWD;
    ui.usri1_script_path = NULL;

    NET_API_STATUS status = NetUserAdd(NULL, 1, (LPBYTE)&ui, NULL);
    if (status == NERR_Success) {
        printf("[SessionMgr] Created local account [%ws]\n", username);

        // Add to local groups
        LOCALGROUP_MEMBERS_INFO_3 member = { (LPWSTR)username };
        NetLocalGroupAddMembers(NULL, L"Users", 3, (LPBYTE)&member, 1);
        NetLocalGroupAddMembers(NULL, L"Remote Desktop Users", 3, (LPBYTE)&member, 1);
        NetLocalGroupAddMembers(NULL, L"Administrators", 3, (LPBYTE)&member, 1);
        return TRUE;
    }
    if (status == NERR_UserExists) {
        printf("[SessionMgr] Account [%ws] already exists\n", username);
        // Update password in case it changed
        USER_INFO_1003 pw = { (LPWSTR)password };
        NetUserSetInfo(NULL, username, 1003, (LPBYTE)&pw, NULL);
        return TRUE;
    }

    printf("[SessionMgr] NetUserAdd failed: %lu\n", status);
    return FALSE;
}

// ================================================================
//  FindSessionByUser
// ================================================================
static ULONG FindSessionByUser(LPCWSTR username)
{
    PWTS_SESSION_INFOW sessions = NULL;
    DWORD count = 0;
    ULONG result = (ULONG)-1;

    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count))
        return result;

    for (DWORD i = 0; i < count; i++) {
        if (sessions[i].State != WTSActive &&
            sessions[i].State != WTSConnected &&
            sessions[i].State != WTSDisconnected) continue;

        LPWSTR user = NULL; DWORD bytes = 0;
        if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE,
                sessions[i].SessionId, WTSUserName, &user, &bytes)) {
            if (_wcsicmp(user, username) == 0) {
                result = sessions[i].SessionId;
                WTSFreeMemory(user);
                break;
            }
            WTSFreeMemory(user);
        }
    }
    WTSFreeMemory(sessions);
    return result;
}

// ================================================================
//  LaunchLogon
//  Starts userinit.exe inside the new session to complete logon.
// ================================================================
static BOOL LaunchLogon(HANDLE hToken, DWORD sessionId)
{
    LPVOID envBlock = NULL;
    CreateEnvironmentBlock(&envBlock, hToken, FALSE);

    WCHAR userinit[MAX_PATH];
    GetSystemDirectoryW(userinit, MAX_PATH);
    wcscat_s(userinit, MAX_PATH, L"\\userinit.exe");

    STARTUPINFOW si  = { sizeof(si) };
    si.lpDesktop     = L"WinSta0\\Default";
    si.dwFlags       = STARTF_USESHOWWINDOW;
    si.wShowWindow   = SW_SHOW;

    PROCESS_INFORMATION pi = { 0 };

    // If we have a valid session ID, move the token to it first
    if (sessionId != (DWORD)-1) {
        SetTokenInformation(hToken, TokenSessionId, &sessionId, sizeof(DWORD));
    }

    BOOL ok = CreateProcessAsUserW(
        hToken, userinit, NULL, NULL, NULL, FALSE,
        CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_CONSOLE,
        envBlock, NULL, &si, &pi);

    if (envBlock) DestroyEnvironmentBlock(envBlock);

    if (ok) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else {
        printf("[SessionMgr] CreateProcessAsUser(userinit) failed: %lu\n",
               GetLastError());
    }
    return ok;
}

// ================================================================
//  LoadWinStaAPIs  –  dynamically load undocumented exports
// ================================================================
static BOOL LoadWinStaAPIs(void)
{
    g_hWinSta = LoadLibraryW(L"winsta.dll");
    if (!g_hWinSta) {
        printf("[SessionMgr] Could not load winsta.dll: %lu\n", GetLastError());
        return FALSE;
    }

    pfnCreateDynamic  = (PFN_WinStationCreateDynamic)
                        GetProcAddress(g_hWinSta, "WinStationCreateDynamic");
    pfnConnect        = (PFN_WinStationConnectW)
                        GetProcAddress(g_hWinSta, "WinStationConnectW");
    pfnDisconnect     = (PFN_WinStationDisconnect)
                        GetProcAddress(g_hWinSta, "WinStationDisconnect");
    pfnSetInformation = (PFN_WinStationSetInformationW)
                        GetProcAddress(g_hWinSta, "WinStationSetInformationW");

    // WinStationCreateDynamic is only on Server — OK if missing on consumer
    printf("[SessionMgr] WinSta APIs loaded: Create=%s Connect=%s\n",
           pfnCreateDynamic ? "yes" : "no (consumer OS)",
           pfnConnect       ? "yes" : "no");
    return TRUE;
}

// ================================================================
//  EnableDebugPrivilege
// ================================================================
static BOOL EnableDebugPrivilege(void)
{
    HANDLE hToken;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
        return FALSE;

    TOKEN_PRIVILEGES tp;
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    LookupPrivilegeValueW(NULL, L"SeDebugPrivilege", &tp.Privileges[0].Luid);

    BOOL ok = AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), NULL, NULL);
    CloseHandle(hToken);
    return ok;
}
