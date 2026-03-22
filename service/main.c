// ================================================================
//  MultiseatProject -- service/main.c
//  Windows service entry point — wires up all components.
// ================================================================

#include <windows.h>
#include <stdio.h>
#include "session_manager.h"
#include "dll_injector.h"
#include "termsrv_patch.h"
#include "../shared/pipe_protocol.h"

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
static DWORD WINAPI PipeServerThread(LPVOID param);

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

    // ── 2. Locate the hook DLL (same dir as service exe) ─────
    DllInjector_Init(GetHookDllPath());

    SetSvcStatus(SERVICE_RUNNING);
    printf("[Main] Service ready.\n");

    // Launch pipe server to receive commands from the UI
    HANDLE hPipeThread = CreateThread(NULL, 0, PipeServerThread, NULL, 0, NULL);

    WaitForSingleObject(g_StopEvent, INFINITE);

    if (hPipeThread) {
        TerminateThread(hPipeThread, 0);
        CloseHandle(hPipeThread);
    }
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

// ================================================================
//  PipeServerThread
//  Listens for commands from the Control Panel UI over a named pipe.
// ================================================================
static DWORD WINAPI PipeServerThread(LPVOID param)
{
    UNREFERENCED_PARAMETER(param);
    printf("[Pipe] Server started on %ws\n", MULTISEAT_PIPE_NAME);

    while (TRUE) {
        HANDLE hPipe = CreateNamedPipeW(
            MULTISEAT_PIPE_NAME,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            sizeof(PIPE_RESPONSE), sizeof(PIPE_REQUEST),
            0, NULL);

        if (hPipe == INVALID_HANDLE_VALUE) {
            printf("[Pipe] CreateNamedPipe failed: %lu\n", GetLastError());
            Sleep(1000);
            continue;
        }

        // Wait for a client (the UI) to connect
        if (!ConnectNamedPipe(hPipe, NULL) && GetLastError() != ERROR_PIPE_CONNECTED) {
            CloseHandle(hPipe);
            continue;
        }

        printf("[Pipe] Client connected\n");

        PIPE_REQUEST req = { 0 };
        PIPE_RESPONSE resp = { 0 };
        DWORD bytesRead = 0;

        if (ReadFile(hPipe, &req, sizeof(req), &bytesRead, NULL) &&
            bytesRead == sizeof(req)) {

            switch (req.Command) {
            case PIPE_CMD_START_SEAT:
                printf("[Pipe] CMD_START_SEAT: seat=%lu user=%ws\n",
                       req.SeatIndex, req.Username);
                resp.Success = SessionManager_CreateSeat(
                    req.SeatIndex, req.Username, req.Password, req.MonitorDevice);
                if (resp.Success) {
                    resp.SessionId = SessionManager_GetSessionId(req.SeatIndex);
                    DllInjector_InjectSession(resp.SessionId);
                    DllInjector_WatchSession(resp.SessionId);
                    wcscpy_s(resp.ErrorMsg, 128, L"Session created successfully.");
                } else {
                    wcscpy_s(resp.ErrorMsg, 128, L"Failed to create session.");
                }
                break;

            case PIPE_CMD_STOP_SEAT:
                printf("[Pipe] CMD_STOP_SEAT: seat=%lu\n", req.SeatIndex);
                resp.Success = SessionManager_TerminateSeat(req.SeatIndex);
                wcscpy_s(resp.ErrorMsg, 128,
                    resp.Success ? L"Session stopped." : L"Failed to stop session.");
                break;

            case PIPE_CMD_PING:
                resp.Success = TRUE;
                wcscpy_s(resp.ErrorMsg, 128, L"MultiseatSvc is running.");
                break;

            default:
                resp.Success = FALSE;
                wcscpy_s(resp.ErrorMsg, 128, L"Unknown command.");
                break;
            }
        }

        DWORD bytesWritten = 0;
        WriteFile(hPipe, &resp, sizeof(resp), &bytesWritten, NULL);
        FlushFileBuffers(hPipe);
        DisconnectNamedPipe(hPipe);
        CloseHandle(hPipe);
    }
    return 0;
}
