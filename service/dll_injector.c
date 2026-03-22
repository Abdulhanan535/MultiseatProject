// ================================================================
//  MultiseatProject -- service/dll_injector.c
//
//  Injects multiseat_mutex_hook.dll into every process running
//  in a target session so that multiple game instances work.
//
//  Also handles new-process injection via WMI process creation
//  events so games launched AFTER setup also get the hook.
//
//  Two injection paths:
//
//  A) IFEO (Image File Execution Options) — AppVerifier shim
//     Adds a "VerifierDlls" value under IFEO for each game exe.
//     Windows automatically loads the DLL before the main image.
//     No remote-thread needed. Works with anti-cheat (sort of).
//
//  B) CreateRemoteThread → LoadLibraryW
//     For processes already running. Enumerates all processes in
//     the seat's session and injects into each one.
//
// ================================================================

#include <windows.h>
#include <tlhelp32.h>
#include <wbemidl.h>
#include <objbase.h>
#include <stdio.h>
#include "dll_injector.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "wbemuuid.lib")

// Path to our hook DLL (placed next to the service exe)
static WCHAR g_DllPath[MAX_PATH] = { 0 };

// ================================================================
//  DllInjector_Init
//  Sets the path to the hook DLL.
// ================================================================
void DllInjector_Init(LPCWSTR dllPath)
{
    wcsncpy_s(g_DllPath, _countof(g_DllPath), dllPath, _TRUNCATE);
    printf("[Injector] Hook DLL: %ws\n", g_DllPath);
}

// ================================================================
//  InjectIntoProcess
//  Classic CreateRemoteThread → LoadLibraryW injection.
// ================================================================
static BOOL InjectIntoProcess(DWORD pid)
{
    HANDLE hProc = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION |
        PROCESS_VM_WRITE | PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
        FALSE, pid);
    if (!hProc) return FALSE;

    // Check the target is 64-bit (our DLL is 64-bit only)
    BOOL isWow64 = FALSE;
    IsWow64Process(hProc, &isWow64);
    if (isWow64) {
        // 32-bit process — would need a separate 32-bit hook DLL
        CloseHandle(hProc);
        return FALSE;
    }

    SIZE_T pathLen = (wcslen(g_DllPath) + 1) * sizeof(WCHAR);

    // Allocate memory for the DLL path in the remote process
    LPVOID remPath = VirtualAllocEx(hProc, NULL, pathLen,
                                    MEM_COMMIT | MEM_RESERVE,
                                    PAGE_READWRITE);
    if (!remPath) { CloseHandle(hProc); return FALSE; }

    WriteProcessMemory(hProc, remPath, g_DllPath, pathLen, NULL);

    // Get LoadLibraryW address (same in all processes on Windows)
    LPVOID llw = (LPVOID)GetProcAddress(
        GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");

    HANDLE hThread = CreateRemoteThread(
        hProc, NULL, 0,
        (LPTHREAD_START_ROUTINE)llw,
        remPath, 0, NULL);

    if (hThread) {
        WaitForSingleObject(hThread, 5000);
        CloseHandle(hThread);
    }

    VirtualFreeEx(hProc, remPath, 0, MEM_RELEASE);
    CloseHandle(hProc);
    return hThread != NULL;
}

// ================================================================
//  DllInjector_InjectSession
//  Injects the hook DLL into all existing processes in sessionId.
//  Call this once when the seat's session starts.
// ================================================================
ULONG DllInjector_InjectSession(ULONG sessionId)
{
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe = { sizeof(pe) };
    ULONG count = 0;

    // Skip these system processes that don't need the hook
    static const WCHAR* skipList[] = {
        L"System", L"smss.exe", L"csrss.exe", L"wininit.exe",
        L"services.exe", L"lsass.exe", L"winlogon.exe",
        L"dwm.exe", L"svchost.exe", NULL
    };

    if (Process32FirstW(hSnap, &pe)) {
        do {
            // Filter by session
            DWORD procSession = 0;
            ProcessIdToSessionId(pe.th32ProcessID, &procSession);
            if (procSession != sessionId) continue;

            // Skip system processes
            BOOL skip = FALSE;
            for (int i = 0; skipList[i]; i++) {
                if (_wcsicmp(pe.szExeFile, skipList[i]) == 0) {
                    skip = TRUE; break;
                }
            }
            if (skip) continue;

            if (InjectIntoProcess(pe.th32ProcessID)) {
                printf("[Injector] Injected into %ws (%lu)\n",
                       pe.szExeFile, pe.th32ProcessID);
                count++;
            }
        } while (Process32NextW(hSnap, &pe));
    }

    CloseHandle(hSnap);
    printf("[Injector] Injected into %lu processes in session %lu\n",
           count, sessionId);
    return count;
}

// ================================================================
//  DllInjector_WatchSession
//  Uses WMI to watch for new process creation events in the
//  target session and auto-injects the hook DLL.
//  Runs in its own thread.
// ================================================================
typedef struct { ULONG sessionId; } WATCH_CTX;

static DWORD WINAPI WatchThread(LPVOID param)
{
    WATCH_CTX* ctx = (WATCH_CTX*)param;
    ULONG targetSession = ctx->sessionId;
    free(ctx);

    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    IWbemLocator*  pLoc  = NULL;
    IWbemServices* pSvc  = NULL;

    HRESULT hr = CoCreateInstance(
        &CLSID_WbemLocator, NULL,
        CLSCTX_INPROC_SERVER, &IID_IWbemLocator,
        (void**)&pLoc);
    if (FAILED(hr)) goto cleanup;

    hr = pLoc->lpVtbl->ConnectServer(pLoc,
        (BSTR)L"ROOT\\CIMV2", NULL, NULL, NULL, 0, NULL, NULL, &pSvc);
    if (FAILED(hr)) goto cleanup;

    // Poll every second for new processes in the target session
    // (A production build would use __InstanceCreationEvent query instead)
    DWORD knownPids[4096] = { 0 };
    ULONG knownCount = 0;

    while (TRUE) {
        Sleep(1000);

        HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (hSnap == INVALID_HANDLE_VALUE) continue;

        PROCESSENTRY32W pe = { sizeof(pe) };
        if (Process32FirstW(hSnap, &pe)) {
            do {
                DWORD sess = 0;
                ProcessIdToSessionId(pe.th32ProcessID, &sess);
                if (sess != targetSession) continue;

                // Check if we've seen this PID before
                BOOL found = FALSE;
                for (ULONG i = 0; i < knownCount; i++) {
                    if (knownPids[i] == pe.th32ProcessID) { found = TRUE; break; }
                }
                if (!found) {
                    if (knownCount < _countof(knownPids))
                        knownPids[knownCount++] = pe.th32ProcessID;

                    // New process — inject
                    Sleep(200); // brief wait for the process to finish loading
                    InjectIntoProcess(pe.th32ProcessID);
                }
            } while (Process32NextW(hSnap, &pe));
        }
        CloseHandle(hSnap);
    }

cleanup:
    if (pSvc) pSvc->lpVtbl->Release(pSvc);
    if (pLoc) pLoc->lpVtbl->Release(pLoc);
    CoUninitialize();
    return 0;
}

HANDLE DllInjector_WatchSession(ULONG sessionId)
{
    WATCH_CTX* ctx = (WATCH_CTX*)malloc(sizeof(WATCH_CTX));
    ctx->sessionId = sessionId;
    HANDLE hThread = CreateThread(NULL, 0, WatchThread, ctx, 0, NULL);
    printf("[Injector] Watching session %lu for new processes\n", sessionId);
    return hThread;
}

// ================================================================
//  DllInjector_SetupIFEO
//  Alternative approach: register the DLL in IFEO so Windows
//  loads it automatically for specific game executables.
//  No remote-thread, lower friction with anti-tamper.
//
//  Usage: DllInjector_SetupIFEO(L"FortniteClient-Win64-Shipping.exe");
// ================================================================
BOOL DllInjector_SetupIFEO(LPCWSTR exeName)
{
    WCHAR keyPath[512];
    swprintf_s(keyPath, _countof(keyPath),
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\"
        L"Image File Execution Options\\%ws", exeName);

    HKEY hKey;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, keyPath, 0, NULL,
            REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL,
            &hKey, NULL) != ERROR_SUCCESS)
        return FALSE;

    // VerifierDlls causes the application verifier shim to load our DLL
    RegSetValueExW(hKey, L"VerifierDlls", 0, REG_SZ,
                   (BYTE*)g_DllPath,
                   (DWORD)((wcslen(g_DllPath) + 1) * sizeof(WCHAR)));
    DWORD globalFlag = 0x100;
    RegSetValueExW(hKey, L"GlobalFlag", 0, REG_DWORD,
                   (BYTE*)&globalFlag, sizeof(globalFlag));  // FLG_APPLICATION_VERIFIER

    RegCloseKey(hKey);
    printf("[Injector] IFEO registered for %ws\n", exeName);
    return TRUE;
}

BOOL DllInjector_RemoveIFEO(LPCWSTR exeName)
{
    WCHAR keyPath[512];
    swprintf_s(keyPath, _countof(keyPath),
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\"
        L"Image File Execution Options\\%ws", exeName);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, keyPath);
    return TRUE;
}
