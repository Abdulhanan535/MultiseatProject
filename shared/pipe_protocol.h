#pragma once
// ================================================================
//  MultiseatProject -- shared/pipe_protocol.h
//  Named pipe IPC between MultiseatCtrlPanel (UI) and MultiseatSvc.
// ================================================================

#include <windows.h>

#define MULTISEAT_PIPE_NAME  L"\\\\.\\pipe\\MultiseatSvc"

// ── Command IDs ─────────────────────────────────────────────────
#define PIPE_CMD_START_SEAT   1
#define PIPE_CMD_STOP_SEAT    2
#define PIPE_CMD_PING         3

// ── Request (UI → Service) ──────────────────────────────────────
typedef struct _PIPE_REQUEST {
    DWORD  Command;
    ULONG  SeatIndex;
    WCHAR  Username[64];
    WCHAR  Password[64];
    WCHAR  MonitorDevice[64];
} PIPE_REQUEST;

// ── Response (Service → UI) ─────────────────────────────────────
typedef struct _PIPE_RESPONSE {
    BOOL   Success;
    ULONG  SessionId;
    WCHAR  ErrorMsg[128];
} PIPE_RESPONSE;
