#pragma once
#include <windows.h>
#define MAX_DEVICE_ID_LEN 200
typedef struct _MULTISEAT_DEVICE_INFO {
    WCHAR DevicePath[512]; WCHAR InstanceId[200];
    WCHAR Description[256]; BOOL IsKeyboard; ULONG SeatIndex;
} MULTISEAT_DEVICE_INFO;
ULONG                  DeviceManager_Enumerate(void);
MULTISEAT_DEVICE_INFO* DeviceManager_GetDevices(ULONG* n);
BOOL                   DeviceManager_AssignToSeat(ULONG idx, ULONG seat);
BOOL                   DeviceManager_SaveConfig(LPCWSTR path);
BOOL                   DeviceManager_LoadConfig(LPCWSTR path);
