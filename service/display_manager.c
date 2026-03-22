#include <windows.h>
#include <stdio.h>
#include "display_manager.h"
#define MAX_MONS 8
static MONITOR_INFO g_Mons[MAX_MONS];
static ULONG        g_MonCount = 0;
static BOOL CALLBACK EnumProc(HMONITOR h, HDC dc, LPRECT r, LPARAM lp) {
    UNREFERENCED_PARAMETER(dc); UNREFERENCED_PARAMETER(lp); UNREFERENCED_PARAMETER(r);
    if (g_MonCount >= MAX_MONS) return FALSE;
    MONITORINFOEXW mi = { sizeof(mi) };
    GetMonitorInfoW(h, (LPMONITORINFO)&mi);
    MONITOR_INFO* m = &g_Mons[g_MonCount++];
    m->hMon = h; m->WorkRect = mi.rcMonitor;
    m->IsPrimary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    m->SeatIndex = 0xFF;
    wcsncpy_s(m->DeviceName, _countof(m->DeviceName), mi.szDevice, _TRUNCATE);
    return TRUE;
}
ULONG DisplayManager_Enumerate(void) {
    g_MonCount = 0;
    EnumDisplayMonitors(NULL, NULL, EnumProc, 0);
    printf("[DispMgr] %lu monitors\n", g_MonCount);
    return g_MonCount;
}
MONITOR_INFO* DisplayManager_GetMonitors(ULONG* n) { if(n)*n=g_MonCount; return g_Mons; }
BOOL DisplayManager_AssignToSeat(ULONG idx, ULONG seat) {
    if (idx >= g_MonCount) return FALSE;
    g_Mons[idx].SeatIndex = seat;
    printf("[DispMgr] Monitor %ws → seat %lu\n", g_Mons[idx].DeviceName, seat);
    return TRUE;
}
BOOL DisplayManager_GetRectForSeat(ULONG seat, RECT* r) {
    for (ULONG i = 0; i < g_MonCount; i++)
        if (g_Mons[i].SeatIndex == seat) { *r = g_Mons[i].WorkRect; return TRUE; }
    return FALSE;
}
