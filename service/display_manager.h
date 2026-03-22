#pragma once
#include <windows.h>
typedef struct _MONITOR_INFO {
    HMONITOR hMon; RECT WorkRect; BOOL IsPrimary;
    ULONG SeatIndex; WCHAR DeviceName[32];
} MONITOR_INFO;
ULONG         DisplayManager_Enumerate(void);
MONITOR_INFO* DisplayManager_GetMonitors(ULONG* n);
BOOL          DisplayManager_AssignToSeat(ULONG idx, ULONG seat);
BOOL          DisplayManager_GetRectForSeat(ULONG seat, RECT* r);
