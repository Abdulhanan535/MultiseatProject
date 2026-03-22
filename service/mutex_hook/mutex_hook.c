// ================================================================
//  MultiseatProject -- service/mutex_hook/mutex_hook.c
//
//  DLL injected into every process in Seat 2's session.
//  It hooks CreateMutexW/A and OpenMutexW/A so that any
//  "Global\" mutex a game creates is silently redirected to
//  a session-local name instead.
//
//  WHY THIS IS NEEDED:
//    Windows named-object namespaces:
//      - Regular name (e.g. "GameSingleInstance")
//        → lives in \Sessions\<N>\BaseNamedObjects\
//        → already isolated per session ✓ (no hook needed)
//
//      - Global name (e.g. "Global\GameSingleInstance")
//        → lives in \BaseNamedObjects\ (machine-wide)
//        → SAME object visible in all sessions ✗
//        → second game instance sees the mutex and exits!
//
//    We intercept CreateMutexW/A calls, detect the "Global\"
//    prefix, strip it (or replace with a session-scoped name),
//    and let the call through with the modified name.
//    Result: each session's game gets its own mutex ✓
//
//  INJECTION:
//    The service uses CreateRemoteThread → LoadLibraryW to inject
//    this DLL into every new process in Seat 2's session.
//    Alternatively, use the AppInit_DLLs or IFEO approach for
//    automatic injection into all processes.
//
//  BUILD:
//    cl /LD /W4 /O2 mutex_hook.c /link /DLL
//       /OUT:multiseat_mutex_hook.dll kernel32.lib
//
// ================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _countof
#define _countof(a) (sizeof(a)/sizeof((a)[0]))
#endif

// ── MinHook or our own inline hook ──────────────────────────────
// We implement a minimal x64 trampoline hook ourselves to avoid
// external dependencies.  The technique:
//   1. Save the first 14 bytes of the target function.
//   2. Write a 14-byte absolute JMP to our hook function.
//   3. The trampoline calls the saved bytes + JMP back.

#define HOOK_SIZE 14  // FF 25 00000000 <8-byte-addr>  (x64 indirect JMP)

typedef struct _HOOK {
    LPVOID  target;
    LPVOID  hook;
    BYTE    original[HOOK_SIZE];
    BYTE    trampoline[HOOK_SIZE + HOOK_SIZE];
    BOOL    installed;
} HOOK;

static BOOL HookInstall(HOOK* h)
{
    // Save original bytes
    DWORD oldProt;
    if (!VirtualProtect(h->target, HOOK_SIZE, PAGE_EXECUTE_READWRITE, &oldProt))
        return FALSE;
    memcpy(h->original, h->target, HOOK_SIZE);

    // Build trampoline: original bytes + jmp back past the patch
    memcpy(h->trampoline, h->original, HOOK_SIZE);
    // Absolute JMP back:  FF 25 00 00 00 00  <addr>
    BYTE* t = h->trampoline + HOOK_SIZE;
    t[0] = 0xFF; t[1] = 0x25;
    *(DWORD*)(t + 2) = 0;
    *(LPVOID*)(t + 6) = (BYTE*)h->target + HOOK_SIZE;

    DWORD tmp;
    VirtualProtect(h->trampoline, sizeof(h->trampoline), PAGE_EXECUTE_READWRITE, &tmp);

    // Write the JMP patch into the target
    BYTE patch[HOOK_SIZE] = { 0xFF, 0x25, 0, 0, 0, 0 };
    *(LPVOID*)(patch + 6) = h->hook;
    memcpy(h->target, patch, HOOK_SIZE);
    VirtualProtect(h->target, HOOK_SIZE, oldProt, &tmp);
    FlushInstructionCache(GetCurrentProcess(), h->target, HOOK_SIZE);

    h->installed = TRUE;
    return TRUE;
}

static BOOL HookRemove(HOOK* h)
{
    if (!h->installed) return TRUE;
    DWORD oldProt, tmp;
    VirtualProtect(h->target, HOOK_SIZE, PAGE_EXECUTE_READWRITE, &oldProt);
    memcpy(h->target, h->original, HOOK_SIZE);
    VirtualProtect(h->target, HOOK_SIZE, oldProt, &tmp);
    FlushInstructionCache(GetCurrentProcess(), h->target, HOOK_SIZE);
    h->installed = FALSE;
    return TRUE;
}

// ── Trampoline typedefs ──────────────────────────────────────────
typedef HANDLE (WINAPI *PFN_CreateMutexW)(
    LPSECURITY_ATTRIBUTES, BOOL, LPCWSTR);
typedef HANDLE (WINAPI *PFN_CreateMutexA)(
    LPSECURITY_ATTRIBUTES, BOOL, LPCSTR);
typedef HANDLE (WINAPI *PFN_OpenMutexW)(
    DWORD, BOOL, LPCWSTR);
typedef HANDLE (WINAPI *PFN_OpenMutexA)(
    DWORD, BOOL, LPCSTR);
typedef HANDLE (WINAPI *PFN_CreateEventW)(
    LPSECURITY_ATTRIBUTES, BOOL, BOOL, LPCWSTR);
typedef HANDLE (WINAPI *PFN_CreateSemaphoreW)(
    LPSECURITY_ATTRIBUTES, LONG, LONG, LPCWSTR);

static HOOK g_HookCreateMutexW;
static HOOK g_HookCreateMutexA;
static HOOK g_HookOpenMutexW;
static HOOK g_HookOpenMutexA;
static HOOK g_HookCreateEventW;
static HOOK g_HookCreateSemaphoreW;

// Our session index (set by the injector before loading this DLL)
static ULONG g_SeatIndex = 1;

// ── Name transformation ──────────────────────────────────────────
// Strips "Global\" prefix so the object becomes session-local.
// Also handles "Local\" (already local, no change needed).
static void StripGlobalPrefix_W(LPCWSTR in, LPWSTR out, ULONG outLen)
{
    if (!in) { out[0] = 0; return; }
    if (_wcsnicmp(in, L"Global\\", 7) == 0) {
        // Replace with session-scoped name: "Local\<name>"
        swprintf_s(out, outLen, L"Local\\%ws", in + 7);
    } else {
        wcsncpy_s(out, outLen, in, _TRUNCATE);
    }
}

static void StripGlobalPrefix_A(LPCSTR in, LPSTR out, ULONG outLen)
{
    if (!in) { out[0] = 0; return; }
    if (_strnicmp(in, "Global\\", 7) == 0) {
        _snprintf_s(out, outLen, _TRUNCATE, "Local\\%s", in + 7);
    } else {
        strncpy_s(out, outLen, in, _TRUNCATE);
    }
}

// ── Hook functions ───────────────────────────────────────────────

static HANDLE WINAPI Hook_CreateMutexW(
    LPSECURITY_ATTRIBUTES lpAttr, BOOL bInitial, LPCWSTR lpName)
{
    WCHAR newName[512];
    StripGlobalPrefix_W(lpName, newName, _countof(newName));
    LPCWSTR useName = (lpName && newName[0]) ? newName : lpName;

    return ((PFN_CreateMutexW)g_HookCreateMutexW.trampoline)(
        lpAttr, bInitial, useName);
}

static HANDLE WINAPI Hook_CreateMutexA(
    LPSECURITY_ATTRIBUTES lpAttr, BOOL bInitial, LPCSTR lpName)
{
    CHAR newName[512];
    StripGlobalPrefix_A(lpName, newName, (ULONG)sizeof(newName));
    LPCSTR useName = (lpName && newName[0]) ? newName : lpName;

    return ((PFN_CreateMutexA)g_HookCreateMutexA.trampoline)(
        lpAttr, bInitial, useName);
}

static HANDLE WINAPI Hook_OpenMutexW(
    DWORD dwAccess, BOOL bInherit, LPCWSTR lpName)
{
    WCHAR newName[512];
    StripGlobalPrefix_W(lpName, newName, _countof(newName));
    LPCWSTR useName = (lpName && newName[0]) ? newName : lpName;

    return ((PFN_OpenMutexW)g_HookOpenMutexW.trampoline)(
        dwAccess, bInherit, useName);
}

static HANDLE WINAPI Hook_OpenMutexA(
    DWORD dwAccess, BOOL bInherit, LPCSTR lpName)
{
    CHAR newName[512];
    StripGlobalPrefix_A(lpName, newName, (ULONG)sizeof(newName));
    LPCSTR useName = (lpName && newName[0]) ? newName : lpName;

    return ((PFN_OpenMutexA)g_HookOpenMutexA.trampoline)(
        dwAccess, bInherit, useName);
}

// Games also use CreateEvent and CreateSemaphore for single-instance checks
static HANDLE WINAPI Hook_CreateEventW(
    LPSECURITY_ATTRIBUTES lpAttr, BOOL bManual, BOOL bInitial, LPCWSTR lpName)
{
    WCHAR newName[512];
    StripGlobalPrefix_W(lpName, newName, _countof(newName));
    LPCWSTR useName = (lpName && newName[0]) ? newName : lpName;

    return ((PFN_CreateEventW)g_HookCreateEventW.trampoline)(
        lpAttr, bManual, bInitial, useName);
}

static HANDLE WINAPI Hook_CreateSemaphoreW(
    LPSECURITY_ATTRIBUTES lpAttr, LONG lInit, LONG lMax, LPCWSTR lpName)
{
    WCHAR newName[512];
    StripGlobalPrefix_W(lpName, newName, _countof(newName));
    LPCWSTR useName = (lpName && newName[0]) ? newName : lpName;

    return ((PFN_CreateSemaphoreW)g_HookCreateSemaphoreW.trampoline)(
        lpAttr, lInit, lMax, useName);
}

// ── Install / remove all hooks ───────────────────────────────────

static void InstallHooks(void)
{
    HMODULE hKernel = GetModuleHandleW(L"kernel32.dll");
    if (!hKernel) hKernel = GetModuleHandleW(L"kernelbase.dll");

#define SETUP_HOOK(h, name, hookfn) \
    (h).target = (LPVOID)(UINT_PTR)GetProcAddress(hKernel, name); \
    (h).hook   = (LPVOID)(UINT_PTR)(hookfn);                       \
    if ((h).target) HookInstall(&(h))

    SETUP_HOOK(g_HookCreateMutexW,    "CreateMutexW",    Hook_CreateMutexW);
    SETUP_HOOK(g_HookCreateMutexA,    "CreateMutexA",    Hook_CreateMutexA);
    SETUP_HOOK(g_HookOpenMutexW,      "OpenMutexW",      Hook_OpenMutexW);
    SETUP_HOOK(g_HookOpenMutexA,      "OpenMutexA",      Hook_OpenMutexA);
    SETUP_HOOK(g_HookCreateEventW,    "CreateEventW",    Hook_CreateEventW);
    SETUP_HOOK(g_HookCreateSemaphoreW,"CreateSemaphoreW",Hook_CreateSemaphoreW);

#undef SETUP_HOOK
}

static void RemoveHooks(void)
{
    HookRemove(&g_HookCreateMutexW);
    HookRemove(&g_HookCreateMutexA);
    HookRemove(&g_HookOpenMutexW);
    HookRemove(&g_HookOpenMutexA);
    HookRemove(&g_HookCreateEventW);
    HookRemove(&g_HookCreateSemaphoreW);
}

// ── DllMain ──────────────────────────────────────────────────────
BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID reserved)
{
    UNREFERENCED_PARAMETER(hInst);
    UNREFERENCED_PARAMETER(reserved);

    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hInst);
        InstallHooks();
        break;
    case DLL_PROCESS_DETACH:
        RemoveHooks();
        break;
    }
    return TRUE;
}

// ── Export for the injector to set the seat index ────────────────
__declspec(dllexport) void MultiseatHook_SetSeat(ULONG seatIndex)
{
    g_SeatIndex = seatIndex;
}
