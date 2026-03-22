#pragma once

// ================================================================
//  MultiseatProject -- shared/protocol.h
//  Shared between the kernel filter driver and usermode service.
// ================================================================

#define MULTISEAT_DEVICE_NAME    L"\\Device\\MultiseatFilter"
#define MULTISEAT_DOS_NAME       L"\\DosDevices\\MultiseatFilter"
#define MULTISEAT_WIN32_NAME     L"\\\\.\\MultiseatFilter"

#define MAX_SEATS                4
#define MAX_DEVICES              64
#define MAX_DEVICE_ID_LEN        200

// ── IOCTL codes ─────────────────────────────────────────────────
#define IOCTL_MS_BASE            0x8000

#define IOCTL_MS_ASSIGN_DEVICE   \
    CTL_CODE(IOCTL_MS_BASE, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_MS_REMOVE_DEVICE   \
    CTL_CODE(IOCTL_MS_BASE, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_MS_QUERY_DEVICES   \
    CTL_CODE(IOCTL_MS_BASE, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_MS_SET_SEAT_SESSION \
    CTL_CODE(IOCTL_MS_BASE, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)

// ── Device types ────────────────────────────────────────────────
typedef enum _MS_DEVICE_TYPE {
    DeviceTypeKeyboard = 0,
    DeviceTypeMouse    = 1,
} MS_DEVICE_TYPE;

// ── Per-device assignment ────────────────────────────────────────
typedef struct _DEVICE_ASSIGNMENT {
    WCHAR          DeviceId[MAX_DEVICE_ID_LEN];
    MS_DEVICE_TYPE DeviceType;
    ULONG          SeatIndex;
} DEVICE_ASSIGNMENT, *PDEVICE_ASSIGNMENT;

typedef struct _ASSIGN_DEVICE_REQUEST {
    DEVICE_ASSIGNMENT Assignment;
} ASSIGN_DEVICE_REQUEST, *PASSIGN_DEVICE_REQUEST;

typedef struct _REMOVE_DEVICE_REQUEST {
    WCHAR DeviceId[MAX_DEVICE_ID_LEN];
} REMOVE_DEVICE_REQUEST, *PREMOVE_DEVICE_REQUEST;

typedef struct _QUERY_DEVICES_RESPONSE {
    ULONG            Count;
    DEVICE_ASSIGNMENT Entries[MAX_DEVICES];
} QUERY_DEVICES_RESPONSE, *PQUERY_DEVICES_RESPONSE;

typedef struct _SET_SEAT_SESSION_REQUEST {
    ULONG SeatIndex;
    ULONG SessionId;
} SET_SEAT_SESSION_REQUEST, *PSET_SEAT_SESSION_REQUEST;

// ── Seat descriptor ──────────────────────────────────────────────
typedef struct _SEAT_INFO {
    ULONG  SeatIndex;
    ULONG  SessionId;
    WCHAR  MonitorDevice[64];
    BOOL   Active;
} SEAT_INFO, *PSEAT_INFO;
