#pragma once
#include <windows.h>

BOOL  SessionManager_Init(void);
BOOL  SessionManager_CreateSeat(ULONG seatIndex, LPCWSTR username,
                                LPCWSTR password, LPCWSTR monitorDeviceName);
BOOL  SessionManager_AssignDisplay(ULONG seatIndex, LPCWSTR monitorDeviceName);
ULONG SessionManager_GetSessionId(ULONG seatIndex);
BOOL  SessionManager_TerminateSeat(ULONG seatIndex);
void  SessionManager_Shutdown(void);
BOOL  NotifyDriverSeatSession(ULONG seatIndex, ULONG sessionId);
