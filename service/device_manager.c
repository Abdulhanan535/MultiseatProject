// ================================================================
//  MultiseatProject -- service/device_manager.c
//  Enumerates HID keyboards/mice and assigns them to seats.
// ================================================================
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <stdio.h>
#include <stdlib.h>
#include "device_manager.h"
#include "../shared/protocol.h"

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

#define MAX_ENUM 64
static MULTISEAT_DEVICE_INFO g_Devs[MAX_ENUM];
static ULONG                 g_DevCount = 0;

ULONG DeviceManager_Enumerate(void)
{
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO hDI = SetupDiGetClassDevsW(&hidGuid, NULL, NULL,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hDI == INVALID_HANDLE_VALUE) return 0;

    g_DevCount = 0;
    SP_DEVICE_INTERFACE_DATA ifd = { sizeof(ifd) };

    for (DWORD idx = 0;
         SetupDiEnumDeviceInterfaces(hDI, NULL, &hidGuid, idx, &ifd);
         idx++)
    {
        if (g_DevCount >= MAX_ENUM) break;
        DWORD req = 0;
        SetupDiGetDeviceInterfaceDetailW(hDI, &ifd, NULL, 0, &req, NULL);
        PSP_DEVICE_INTERFACE_DETAIL_DATA_W det =
            (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)malloc(req);
        if (!det) continue;
        det->cbSize = sizeof(*det);
        SP_DEVINFO_DATA did = { sizeof(did) };
        if (!SetupDiGetDeviceInterfaceDetailW(hDI, &ifd, det, req, NULL, &did)) {
            free(det); continue;
        }
        HANDLE h = CreateFileW(det->DevicePath, GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) { free(det); continue; }

        PHIDP_PREPARSED_DATA pp = NULL; HIDP_CAPS caps = {0};
        if (HidD_GetPreparsedData(h, &pp)) { HidP_GetCaps(pp, &caps); HidD_FreePreparsedData(pp); }
        CloseHandle(h);

        BOOL isKB = (caps.UsagePage == 1 && caps.Usage == 6);
        BOOL isMouse = (caps.UsagePage == 1 && caps.Usage == 2);
        if (!isKB && !isMouse) { free(det); continue; }

        WCHAR desc[256] = L"Unknown";
        SetupDiGetDeviceRegistryPropertyW(hDI, &did, SPDRP_DEVICEDESC,
            NULL, (PBYTE)desc, sizeof(desc)-2, NULL);
        WCHAR instId[MAX_DEVICE_ID_LEN] = {0};
        SetupDiGetDeviceInstanceIdW(hDI, &did, instId, _countof(instId), NULL);

        MULTISEAT_DEVICE_INFO* d = &g_Devs[g_DevCount++];
        wcsncpy_s(d->DevicePath,  _countof(d->DevicePath),  det->DevicePath, _TRUNCATE);
        wcsncpy_s(d->InstanceId,  _countof(d->InstanceId),  instId, _TRUNCATE);
        wcsncpy_s(d->Description, _countof(d->Description), desc,   _TRUNCATE);
        d->IsKeyboard = isKB;
        d->SeatIndex  = 0xFF;
        free(det);
    }
    SetupDiDestroyDeviceInfoList(hDI);
    printf("[DevMgr] Found %lu HID input devices\n", g_DevCount);
    return g_DevCount;
}

MULTISEAT_DEVICE_INFO* DeviceManager_GetDevices(ULONG* n) { if(n)*n=g_DevCount; return g_Devs; }

BOOL DeviceManager_AssignToSeat(ULONG idx, ULONG seat)
{
    if (idx >= g_DevCount) return FALSE;
    MULTISEAT_DEVICE_INFO* d = &g_Devs[idx];

    ASSIGN_DEVICE_REQUEST req;
    wcsncpy_s(req.Assignment.DeviceId, _countof(req.Assignment.DeviceId),
              d->InstanceId, _TRUNCATE);
    req.Assignment.DeviceType = d->IsKeyboard ? DeviceTypeKeyboard : DeviceTypeMouse;
    req.Assignment.SeatIndex  = seat;

    HANDLE hDrv = CreateFileW(MULTISEAT_WIN32_NAME,
        GENERIC_READ|GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, 0, NULL);
    if (hDrv == INVALID_HANDLE_VALUE) {
        d->SeatIndex = seat; return TRUE; // driver not yet loaded
    }
    DWORD br=0;
    BOOL ok = DeviceIoControl(hDrv, IOCTL_MS_ASSIGN_DEVICE,
                              &req, sizeof(req), NULL, 0, &br, NULL);
    CloseHandle(hDrv);
    if (ok) d->SeatIndex = seat;
    return ok;
}

BOOL DeviceManager_SaveConfig(LPCWSTR path)
{
    FILE* f; _wfopen_s(&f, path, L"w,ccs=UTF-8");
    if (!f) return FALSE;
    fwprintf(f, L"[\n");
    for (ULONG i = 0; i < g_DevCount; i++) {
        if (g_Devs[i].SeatIndex == 0xFF) continue;
        fwprintf(f, L"  {\"id\":\"%ws\",\"seat\":%lu,\"type\":\"%ws\"},\n",
            g_Devs[i].InstanceId, g_Devs[i].SeatIndex,
            g_Devs[i].IsKeyboard ? L"keyboard" : L"mouse");
    }
    fwprintf(f, L"]\n");
    fclose(f);
    return TRUE;
}

BOOL DeviceManager_LoadConfig(LPCWSTR path)
{
    FILE* f; _wfopen_s(&f, path, L"r,ccs=UTF-8");
    if (!f) return FALSE;
    WCHAR line[512];
    while (fgetws(line, _countof(line), f)) {
        WCHAR id[MAX_DEVICE_ID_LEN]; ULONG seat; WCHAR type[16];
        if (swscanf_s(line, L" {\"id\":\"%199[^\"]\",\"seat\":%lu,\"type\":\"%15[^\"]\"}",
                id, (unsigned)_countof(id), &seat, type, (unsigned)_countof(type)) == 3) {
            for (ULONG i = 0; i < g_DevCount; i++) {
                if (!_wcsicmp(g_Devs[i].InstanceId, id)) {
                    DeviceManager_AssignToSeat(i, seat); break;
                }
            }
        }
    }
    fclose(f);
    return TRUE;
}
