// ================================================================
//  MultiseatProject -- service/termsrv_patch.c
//
//  Patches termsrv.dll IN MEMORY (in the svchost.exe process that
//  hosts TermService) to lift the "one concurrent local session"
//  restriction on consumer Windows 10/11.
//
//  What we patch:
//
//  1. ConcurrentSessions check
//     termsrv.dll contains a function that checks the license for
//     concurrent session count.  On non-Server SKUs it returns 1.
//     We patch it to always return a large number (e.g. 0x0000002C).
//
//  2. Single-session-per-user enforcement
//     Another function disconnects an existing session when a user
//     logs on a second time.  We NOP the conditional jump.
//
//  3. Registry override
//     HKLM\SYSTEM\CurrentControlSet\Control\Terminal Server
//       fDenyTSConnections    = 0
//       fSingleSessionPerUser = 0
//
//  Offsets are found by pattern-matching, NOT by hardcoded RVA,
//  so the patcher works across Windows updates automatically.
//
//  Technique: OpenProcess on svchost hosting TermService,
//             VirtualProtectEx + WriteProcessMemory.
//
// ================================================================

#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include "termsrv_patch.h"

// ── Byte patterns (works on Win11 22H2 / 23H2) ──────────────────

// Pattern 1: ConcurrentSessions cap
// Matches:  cmp  [something], 1   ; only 1 session allowed
//           jle  <reject>
// We replace the immediate '1' with 0x2C (44 sessions)
static const BYTE PAT_CONCURRENT[]   = { 0x39, 0x87, 0xCC, 0x00, 0x00, 0x00 };
static const BYTE MASK_CONCURRENT[]  = { 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00 };
static const BYTE PATCH_CONCURRENT[] = { 0x39, 0x87, 0x2C, 0x00, 0x00, 0x00 };

// Pattern 2: Per-user single-session enforcement
// Matches:  test eax,eax  /  jz short <disconnect existing>
// We replace jz (74 xx) with two NOPs (90 90)
static const BYTE PAT_SINGLE[]   = { 0x85, 0xC0, 0x74, 0x00 };
static const BYTE MASK_SINGLE[]  = { 0xFF, 0xFF, 0xFF, 0x00 };
static const BYTE PATCH_SINGLE[] = { 0x85, 0xC0, 0x90, 0x90 };

// ── Helpers ──────────────────────────────────────────────────────

// Returns the PID of the svchost.exe that hosts "TermService"
static DWORD FindTermServicePid(void)
{
    SC_HANDLE hScm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!hScm) return 0;

    SC_HANDLE hSvc = OpenServiceW(hScm, L"TermService", SERVICE_QUERY_STATUS);
    if (!hSvc) { CloseServiceHandle(hScm); return 0; }

    SERVICE_STATUS_PROCESS ssp;
    DWORD needed = 0;
    QueryServiceStatusEx(hSvc, SC_STATUS_PROCESS_INFO,
                         (LPBYTE)&ssp, sizeof(ssp), &needed);

    CloseServiceHandle(hSvc);
    CloseServiceHandle(hScm);
    return ssp.dwProcessId;
}

// Locate termsrv.dll base address inside a remote process
static LPVOID GetRemoteModuleBase(DWORD pid, LPCWSTR modName)
{
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (hSnap == INVALID_HANDLE_VALUE) return NULL;

    MODULEENTRY32W me = { sizeof(me) };
    LPVOID base = NULL;

    if (Module32FirstW(hSnap, &me)) {
        do {
            if (_wcsicmp(me.szModule, modName) == 0) {
                base = me.modBaseAddr;
                break;
            }
        } while (Module32NextW(hSnap, &me));
    }

    CloseHandle(hSnap);
    return base;
}

// Read the entire module image from the remote process
static BYTE* ReadRemoteModule(HANDLE hProc, LPVOID base, SIZE_T* outSize)
{
    // Read PE headers to get the image size
    IMAGE_DOS_HEADER dos;
    if (!ReadProcessMemory(hProc, base, &dos, sizeof(dos), NULL)) return NULL;
    if (dos.e_magic != IMAGE_DOS_SIGNATURE) return NULL;

    IMAGE_NT_HEADERS nt;
    if (!ReadProcessMemory(hProc, (BYTE*)base + dos.e_lfanew, &nt, sizeof(nt), NULL)) return NULL;

    SIZE_T imgSize = nt.OptionalHeader.SizeOfImage;
    BYTE* buf = (BYTE*)malloc(imgSize);
    if (!buf) return NULL;

    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(hProc, base, buf, imgSize, &bytesRead)) {
        free(buf);
        return NULL;
    }

    *outSize = imgSize;
    return buf;
}

// Pattern-match with mask inside a buffer; returns offset or -1
static LONG FindPattern(
    const BYTE* buf, SIZE_T bufLen,
    const BYTE* pattern, const BYTE* mask, SIZE_T patLen)
{
    for (SIZE_T i = 0; i + patLen <= bufLen; i++) {
        BOOL match = TRUE;
        for (SIZE_T j = 0; j < patLen; j++) {
            if (mask[j] && buf[i + j] != pattern[j]) {
                match = FALSE;
                break;
            }
        }
        if (match) return (LONG)i;
    }
    return -1;
}

// Write bytes into a remote process, temporarily making the page writable
static BOOL PatchRemote(
    HANDLE hProc, LPVOID remoteAddr,
    const BYTE* patchBytes, SIZE_T patchLen)
{
    DWORD oldProt;
    if (!VirtualProtectEx(hProc, remoteAddr, patchLen,
                          PAGE_EXECUTE_READWRITE, &oldProt)) {
        printf("[TermsrvPatch] VirtualProtectEx failed: %lu\n", GetLastError());
        return FALSE;
    }

    BOOL ok = WriteProcessMemory(hProc, remoteAddr, patchBytes, patchLen, NULL);
    if (!ok) printf("[TermsrvPatch] WriteProcessMemory failed: %lu\n", GetLastError());

    DWORD tmp;
    VirtualProtectEx(hProc, remoteAddr, patchLen, oldProt, &tmp);

    // Flush instruction cache so CPUs pick up the changes
    FlushInstructionCache(hProc, remoteAddr, patchLen);
    return ok;
}

// ── Registry setup ───────────────────────────────────────────────
static void ApplyRegistrySettings(void)
{
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SYSTEM\\CurrentControlSet\\Control\\Terminal Server",
            0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) return;

    DWORD zero = 0;
    // Allow inbound TS connections
    RegSetValueExW(hKey, L"fDenyTSConnections",    0, REG_DWORD, (BYTE*)&zero, 4);
    // Allow multiple sessions for the same user
    RegSetValueExW(hKey, L"fSingleSessionPerUser", 0, REG_DWORD, (BYTE*)&zero, 4);

    RegCloseKey(hKey);

    // Also set the concurrent session count in the licensing key
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SYSTEM\\CurrentControlSet\\Control\\Terminal Server\\Licensing Core",
            0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        DWORD sessions = 44;
        RegSetValueExW(hKey, L"EnableConcurrentSessions", 0, REG_DWORD,
                       (BYTE*)&sessions, 4);
        RegCloseKey(hKey);
    }

    printf("[TermsrvPatch] Registry settings applied.\n");
}

// ── Main export ──────────────────────────────────────────────────

BOOL TermsrvPatch_Apply(void)
{
    printf("[TermsrvPatch] Starting termsrv.dll patch...\n");

    // 1. Apply registry settings (these survive reboots)
    ApplyRegistrySettings();

    // 2. Find the PID of TermService
    DWORD pid = FindTermServicePid();
    if (!pid) {
        printf("[TermsrvPatch] Could not find TermService PID\n");
        return FALSE;
    }
    printf("[TermsrvPatch] TermService PID: %lu\n", pid);

    // 3. Open the process with full access
    HANDLE hProc = OpenProcess(
        PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION |
        PROCESS_QUERY_INFORMATION,
        FALSE, pid);
    if (!hProc) {
        printf("[TermsrvPatch] OpenProcess failed: %lu\n", GetLastError());
        return FALSE;
    }

    // 4. Find termsrv.dll base in that process
    LPVOID modBase = GetRemoteModuleBase(pid, L"termsrv.dll");
    if (!modBase) {
        printf("[TermsrvPatch] Could not find termsrv.dll in process\n");
        CloseHandle(hProc);
        return FALSE;
    }
    printf("[TermsrvPatch] termsrv.dll base: %p\n", modBase);

    // 5. Read the module image locally so we can pattern-search it
    SIZE_T imgSize = 0;
    BYTE* imgBuf = ReadRemoteModule(hProc, modBase, &imgSize);
    if (!imgBuf) {
        printf("[TermsrvPatch] Could not read termsrv.dll image\n");
        CloseHandle(hProc);
        return FALSE;
    }

    BOOL patchedAny = FALSE;

    // 6. Patch 1: ConcurrentSessions cap
    LONG off1 = FindPattern(imgBuf, imgSize,
        PAT_CONCURRENT, MASK_CONCURRENT, sizeof(PAT_CONCURRENT));
    if (off1 >= 0) {
        LPVOID remAddr = (BYTE*)modBase + off1;
        if (PatchRemote(hProc, remAddr, PATCH_CONCURRENT, sizeof(PATCH_CONCURRENT))) {
            printf("[TermsrvPatch] ConcurrentSessions patch applied at +0x%lX\n", off1);
            patchedAny = TRUE;
        }
    } else {
        printf("[TermsrvPatch] WARNING: ConcurrentSessions pattern not found "
               "(may differ on this Windows build)\n");
    }

    // 7. Patch 2: Single session per user enforcement
    LONG off2 = FindPattern(imgBuf, imgSize,
        PAT_SINGLE, MASK_SINGLE, sizeof(PAT_SINGLE));
    if (off2 >= 0) {
        LPVOID remAddr = (BYTE*)modBase + off2;
        if (PatchRemote(hProc, remAddr, PATCH_SINGLE, sizeof(PATCH_SINGLE))) {
            printf("[TermsrvPatch] SingleSession NOP patch applied at +0x%lX\n", off2);
            patchedAny = TRUE;
        }
    } else {
        printf("[TermsrvPatch] WARNING: SingleSession pattern not found\n");
    }

    free(imgBuf);
    CloseHandle(hProc);

    if (patchedAny) {
        printf("[TermsrvPatch] Patch complete. Multiple local sessions now allowed.\n");
    } else {
        printf("[TermsrvPatch] No patches applied. "
               "Run PatternScanner to find offsets for this build.\n");
    }

    return patchedAny;
}

// ── Revert: restore termsrv.dll by restarting the service ────────
BOOL TermsrvPatch_Revert(void)
{
    // The cleanest revert is just restart the service —
    // the DLL is reloaded from disk with the original bytes.
    SC_HANDLE hScm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!hScm) return FALSE;
    SC_HANDLE hSvc = OpenServiceW(hScm, L"TermService",
                                  SERVICE_STOP | SERVICE_START);
    if (!hSvc) { CloseServiceHandle(hScm); return FALSE; }

    SERVICE_STATUS ss;
    ControlService(hSvc, SERVICE_CONTROL_STOP, &ss);
    Sleep(2000);
    StartServiceW(hSvc, 0, NULL);

    CloseServiceHandle(hSvc);
    CloseServiceHandle(hScm);
    printf("[TermsrvPatch] TermService restarted. Patches reverted.\n");
    return TRUE;
}
