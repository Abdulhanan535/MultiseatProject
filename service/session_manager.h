#pragma once
#include <windows.h>

BOOL  SessionManager_Init(void);
BOOL  SessionManager_CreateSeat(ULONG seatIndex, LPCWSTR username,
                                LPCWSTR password, LPCWSTR monitorDeviceName);
ULONG SessionManager_GetSessionId(ULONG seatIndex);
BOOL  SessionManager_TerminateSeat(ULONG seatIndex);
void  SessionManager_Shutdown(void);
