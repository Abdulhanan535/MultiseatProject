// ================================================================
//  MultiseatProject -- service/main.c
//  Windows service entry point — wires up all components.
// ================================================================

#include <windows.h>
#include <stdio.h>
#include "session_manager.h"
#include "device_manager.h"
#include "display_manager.h"
#include "dll_injector.h"
#include "termsrv_patch.h"

#define SERVICE_NAME     L"MultiseatSvc"
#define CONFIG_PATH      L"C:\\ProgramData\\MultiseatProject\\config.json"
#define HOOK_DLL_NAME    L"multiseat_mutex_hook.dll"

static SERVICE_STATUS        g_Svc;
static SERVICE_STATUS_HANDLE g_SvcHandle;
static HANDLE                g_StopEvent;

static VOID WINAPI ServiceMain(DWORD argc, LPWSTR* argv);
static VOID WINAPI CtrlHandler(DWORD ctrl);
static VOID        RunLogic(void);
static VOID        SetSvcStatus(DWORD state);
static BOOL        Install(void);
static BOOL        Uninstall(void);
static WCHAR*      GetHookDllPath(void);

int wmain(int argc, wchar_t* argv[])
{
    if (argc > 1) {
        if (!wcscmp(argv[1], L"--install"))   return Install()   ? 0 : 1;
        if (!wcscmp(argv[1], L"--uninstall")) return Uninstall() ? 0 : 1;
        if (!wcscmp(argv[1], L"--console")) {
            printf("MultiseatSvc [console mode]\n");
            g_StopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
            RunLogic();
            return 0;
        }
    }
    SERVICE_TABLE_ENTRYW tbl[] = {
        { SERVICE_NAME, ServiceMain }, { NULL, NULL }
    };
    StartServiceCtrlDispatcherW(tbl);
    return 0;
}

static VOID WINAPI ServiceMain(DWORD argc, LPWSTR* argv)
{
    UNREFERENCED_PARAMETER(argc); UNREFERENCED_PARAMETER(argv);
    g_StopEvent  = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_SvcHandle  = RegisterServiceCtrlHandlerW(SERVICE_NAME, CtrlHandler);
    SetSvcStatus(SERVICE_START_PENDING);
    RunLogic();
    SetSvcStatus(SERVICE_STOPPED);
}

static VOID WINAPI CtrlHandler(DWORD ctrl)
{
    if (ctrl == SERVICE_CONTROL_STOP || ctrl == SERVICE_CONTROL_SHUTDOWN) {
        SetSvcStatus(SERVICE_STOP_PENDING);
        SetEvent(g_StopEvent);
    }
}

static VOID RunLogic(void)
{
    // Create config directory
    CreateDirectoryW(L"C:\\ProgramData\\MultiseatProject", NULL);

    // ── 1. Initialize all sub-systems ────────────────────────
    SessionManager_Init();      // also patches termsrv.dll
    DisplayManager_Enumerate();
    DeviceManager_Enumerate();

    // ── 2. Locate the hook DLL (same dir as service exe) ─────
    DllInjector_Init(GetHookDllPath());

    // ── 3. Load saved device/monitor assignments ─────────────
    DeviceManager_LoadConfig(CONFIG_PATH);

    SetSvcStatus(SERVICE_RUNNING);
    printf("[Main] Service ready.\n");

    WaitForSingleObject(g_StopEvent, INFINITE);

    SessionManager_Shutdown();
    printf("[Main] Service stopped.\n");
}

static WCHAR g_DllPathBuf[MAX_PATH];
static WCHAR* GetHookDllPath(void)
{
    GetModuleFileNameW(NULL, g_DllPathBuf, MAX_PATH);
    // Replace filename with hook DLL name
    WCHAR* last = wcsrchr(g_DllPathBuf, L'\\');
    if (last) wcscpy_s(last + 1, MAX_PATH - (last - g_DllPathBuf + 1), HOOK_DLL_NAME);
    return g_DllPathBuf;
}

static VOID SetSvcStatus(DWORD state)
{
    if (!g_SvcHandle) return;
    g_Svc.dwServiceType      = SERVICE_WIN32_OWN_PROCESS;
    g_Svc.dwCurrentState     = state;
    g_Svc.dwControlsAccepted = (state == SERVICE_RUNNING) ? SERVICE_ACCEPT_STOP : 0;
    g_Svc.dwWin32ExitCode    = NO_ERROR;
    g_Svc.dwWaitHint         = 3000;
    SetServiceStatus(g_SvcHandle, &g_Svc);
}

static BOOL Install(void)
{
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!scm) return FALSE;
    SC_HANDLE svc = CreateServiceW(scm, SERVICE_NAME,
        L"Multiseat Session Manager",
        SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
        path, NULL, NULL, NULL, NULL, NULL);
    BOOL ok = (svc != NULL) || (GetLastError() == ERROR_SERVICE_EXISTS);
    if (svc) CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    if (ok) printf("Service installed. Run: sc start %ws\n", SERVICE_NAME);
    return ok;
}

static BOOL Uninstall(void)
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scm) return FALSE;
    SC_HANDLE svc = OpenServiceW(scm, SERVICE_NAME, DELETE | SERVICE_STOP);
    if (!svc) { CloseServiceHandle(scm); return FALSE; }
    SERVICE_STATUS ss;
    ControlService(svc, SERVICE_CONTROL_STOP, &ss);
    Sleep(1000);
    BOOL ok = DeleteService(svc);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok;
}
