// ================================================================
//  MultiseatProject -- driver/multiseat_filter.c
//
//  KMDF upper-level HID filter driver.
//  - Sits above every keyboard and mouse device
//  - Routes input to the correct session based on device→seat mapping
//  - Accepts IOCTLs from the usermode service
//
//  Build with WDK 11, KMDF 1.33, x64 target
// ================================================================

#include <ntddk.h>
#include <wdf.h>
#include <hidport.h>
#include <ntstrsafe.h>
#include "../shared/protocol.h"

// ── Per-device context ───────────────────────────────────────────
typedef struct _DEVICE_CTX {
    WCHAR   DeviceId[MAX_DEVICE_ID_LEN];
    ULONG   SeatIndex;   // 0xFF = unassigned
    BOOLEAN IsKeyboard;
} DEVICE_CTX;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CTX, GetDevCtx)

// ── Global state ─────────────────────────────────────────────────
static WDFDEVICE  g_ControlDevice = NULL;
static KSPIN_LOCK g_Lock;
static DEVICE_ASSIGNMENT g_Table[MAX_DEVICES];
static ULONG             g_TableCount = 0;
static ULONG             g_SeatSession[MAX_SEATS];  // seat → WTS session ID

// ── Forwards ─────────────────────────────────────────────────────
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD           Evt_DeviceAdd;
EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL Evt_InternalIoctl;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL  Evt_ControlIoctl;
EVT_WDF_REQUEST_COMPLETION_ROUTINE  Evt_ReadComplete;

static NTSTATUS CreateControlDevice(WDFDRIVER Driver);
static VOID     UpdateCtxFromTable(DEVICE_CTX* Ctx);
static VOID     RouteInput(DEVICE_CTX* Ctx, PVOID Report, SIZE_T ReportLen);

// ================================================================
//  DriverEntry
// ================================================================
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObj, PUNICODE_STRING RegPath)
{
    WDF_DRIVER_CONFIG cfg;
    NTSTATUS status;

    KeInitializeSpinLock(&g_Lock);
    RtlZeroMemory(g_Table,       sizeof(g_Table));
    RtlZeroMemory(g_SeatSession, sizeof(g_SeatSession));

    WDF_DRIVER_CONFIG_INIT(&cfg, Evt_DeviceAdd);
    status = WdfDriverCreate(DriverObj, RegPath,
                             WDF_NO_OBJECT_ATTRIBUTES, &cfg, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) return status;

    return CreateControlDevice(WdfGetDriver());
}

// ================================================================
//  CreateControlDevice  –  \\.\MultiseatFilter
// ================================================================
static NTSTATUS CreateControlDevice(WDFDRIVER Driver)
{
    PWDFDEVICE_INIT pInit;
    WDFDEVICE       ctrlDev;
    WDF_IO_QUEUE_CONFIG qCfg;
    UNICODE_STRING  ntName, dosName;
    NTSTATUS        status;

    RtlInitUnicodeString(&ntName,  MULTISEAT_DEVICE_NAME);
    RtlInitUnicodeString(&dosName, MULTISEAT_DOS_NAME);

    pInit = WdfControlDeviceInitAllocate(
        Driver, &SDDL_DEVOBJ_SYS_ALL_ADM_RWX_WORLD_RW_RES_R);
    if (!pInit) return STATUS_INSUFFICIENT_RESOURCES;

    WdfDeviceInitSetExclusive(pInit, FALSE);
    status = WdfDeviceInitAssignName(pInit, &ntName);
    if (!NT_SUCCESS(status)) { WdfDeviceInitFree(pInit); return status; }

    status = WdfDeviceCreate(&pInit, WDF_NO_OBJECT_ATTRIBUTES, &ctrlDev);
    if (!NT_SUCCESS(status)) { WdfDeviceInitFree(pInit); return status; }

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&qCfg, WdfIoQueueDispatchParallel);
    qCfg.EvtIoDeviceControl = Evt_ControlIoctl;
    status = WdfIoQueueCreate(ctrlDev, &qCfg, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) return status;

    status = WdfDeviceCreateSymbolicLink(ctrlDev, &dosName);
    if (!NT_SUCCESS(status)) return status;

    WdfControlFinishInitializing(ctrlDev);
    g_ControlDevice = ctrlDev;
    return STATUS_SUCCESS;
}

// ================================================================
//  Evt_DeviceAdd  –  called for each HID keyboard/mouse
// ================================================================
NTSTATUS Evt_DeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DevInit)
{
    NTSTATUS            status;
    WDFDEVICE           device;
    WDF_OBJECT_ATTRIBUTES attribs;
    WDF_IO_QUEUE_CONFIG qCfg;
    DEVICE_CTX*         ctx;
    WCHAR               idBuf[MAX_DEVICE_ID_LEN];

    UNREFERENCED_PARAMETER(Driver);
    WdfFdoInitSetFilter(DevInit);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attribs, DEVICE_CTX);
    status = WdfDeviceCreate(&DevInit, &attribs, &device);
    if (!NT_SUCCESS(status)) return status;

    ctx = GetDevCtx(device);
    ctx->SeatIndex = 0xFF;

    // Get device instance path as stable ID
    if (NT_SUCCESS(WdfDeviceQueryProperty(device,
            DevicePropertyDeviceDescription,
            sizeof(idBuf) - sizeof(WCHAR), idBuf, NULL)))
        RtlStringCbCopyW(ctx->DeviceId, sizeof(ctx->DeviceId), idBuf);

    // Queue for IRP_MJ_INTERNAL_DEVICE_CONTROL (HID read path)
    WDF_IO_QUEUE_CONFIG_INIT(&qCfg, WdfIoQueueDispatchParallel);
    qCfg.EvtIoInternalDeviceControl = Evt_InternalIoctl;
    status = WdfIoQueueCreate(device, &qCfg, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) return status;

    UpdateCtxFromTable(ctx);
    KdPrint(("Multiseat: Attached [%ws] seat=%u\n", ctx->DeviceId, ctx->SeatIndex));
    return STATUS_SUCCESS;
}

// ================================================================
//  Evt_InternalIoctl  –  intercept HID reads
// ================================================================
VOID Evt_InternalIoctl(
    WDFQUEUE Queue, WDFREQUEST Req,
    SIZE_T OutLen, SIZE_T InLen, ULONG IoCode)
{
    UNREFERENCED_PARAMETER(OutLen); UNREFERENCED_PARAMETER(InLen);

    WDFDEVICE   dev    = WdfIoQueueGetDevice(Queue);
    DEVICE_CTX* ctx    = GetDevCtx(dev);
    WDFIOTARGET target = WdfDeviceGetIoTarget(dev);

    if (IoCode == IOCTL_HID_READ_REPORT && ctx->SeatIndex != 0xFF)
        WdfRequestSetCompletionRoutine(Req, Evt_ReadComplete, ctx);

    WDF_REQUEST_SEND_OPTIONS opts;
    WDF_REQUEST_SEND_OPTIONS_INIT(&opts, WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);
    if (!WdfRequestSend(Req, target, &opts))
        WdfRequestComplete(Req, WdfRequestGetStatus(Req));
}

// ================================================================
//  Evt_ReadComplete  –  HID report arrived; tag it with seat
// ================================================================
VOID Evt_ReadComplete(
    WDFREQUEST Req, WDFIOTARGET Target,
    PWDF_REQUEST_COMPLETION_PARAMS Params, WDFCONTEXT Context)
{
    DEVICE_CTX* ctx = (DEVICE_CTX*)Context;
    UNREFERENCED_PARAMETER(Target);

    if (NT_SUCCESS(Params->IoStatus.Status)) {
        // Get the output buffer (the HID report)
        PVOID  buf = NULL;
        SIZE_T len = 0;
        WdfRequestRetrieveOutputBuffer(Req, 1, &buf, &len);

        if (buf && len > 0)
            RouteInput(ctx, buf, len);
    }
    WdfRequestComplete(Req, Params->IoStatus.Status);
}

// ================================================================
//  RouteInput
//  Decides whether to pass the input to the current foreground
//  session or suppress it (because it belongs to another seat).
//
//  The full routing implementation writes the raw HID report
//  plus seat tag into a kernel shared memory ring buffer that
//  the usermode service reads via an IOCTL/event, then calls
//  SendInput() in the correct session's context.
//
//  Stub here logs the routing decision.
// ================================================================
static VOID RouteInput(DEVICE_CTX* Ctx, PVOID Report, SIZE_T ReportLen)
{
    KIRQL irql;
    ULONG targetSession;

    KeAcquireSpinLock(&g_Lock, &irql);
    targetSession = (Ctx->SeatIndex < MAX_SEATS)
                    ? g_SeatSession[Ctx->SeatIndex]
                    : 0;
    KeReleaseSpinLock(&g_Lock, irql);

    KdPrint(("Multiseat: Input seat=%u session=%u size=%zu\n",
             Ctx->SeatIndex, targetSession, ReportLen));
    // In full build: write to shared ring buffer here
    UNREFERENCED_PARAMETER(Report);
}

// ================================================================
//  Evt_ControlIoctl  –  IOCTL handler on the control device
// ================================================================
VOID Evt_ControlIoctl(
    WDFQUEUE Queue, WDFREQUEST Req,
    SIZE_T OutLen, SIZE_T InLen, ULONG IoCode)
{
    NTSTATUS status  = STATUS_SUCCESS;
    ULONG    written = 0;
    KIRQL    irql;
    PVOID    inBuf = NULL, outBuf = NULL;
    UNREFERENCED_PARAMETER(Queue);

    switch (IoCode) {

    case IOCTL_MS_ASSIGN_DEVICE: {
        PASSIGN_DEVICE_REQUEST req;
        if (InLen < sizeof(*req)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        WdfRequestRetrieveInputBuffer(Req, sizeof(*req), &inBuf, NULL);
        req = (PASSIGN_DEVICE_REQUEST)inBuf;

        KeAcquireSpinLock(&g_Lock, &irql);
        BOOL found = FALSE;
        for (ULONG i = 0; i < g_TableCount; i++) {
            if (_wcsicmp(g_Table[i].DeviceId, req->Assignment.DeviceId) == 0) {
                g_Table[i] = req->Assignment; found = TRUE; break;
            }
        }
        if (!found && g_TableCount < MAX_DEVICES)
            g_Table[g_TableCount++] = req->Assignment;
        KeReleaseSpinLock(&g_Lock, irql);
        break;
    }

    case IOCTL_MS_REMOVE_DEVICE: {
        PREMOVE_DEVICE_REQUEST req;
        if (InLen < sizeof(*req)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        WdfRequestRetrieveInputBuffer(Req, sizeof(*req), &inBuf, NULL);
        req = (PREMOVE_DEVICE_REQUEST)inBuf;

        KeAcquireSpinLock(&g_Lock, &irql);
        for (ULONG i = 0; i < g_TableCount; i++) {
            if (_wcsicmp(g_Table[i].DeviceId, req->DeviceId) == 0) {
                RtlMoveMemory(&g_Table[i], &g_Table[i+1],
                    (g_TableCount - i - 1) * sizeof(DEVICE_ASSIGNMENT));
                g_TableCount--; break;
            }
        }
        KeReleaseSpinLock(&g_Lock, irql);
        break;
    }

    case IOCTL_MS_QUERY_DEVICES: {
        PQUERY_DEVICES_RESPONSE resp;
        if (OutLen < sizeof(*resp)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        WdfRequestRetrieveOutputBuffer(Req, sizeof(*resp), &outBuf, NULL);
        resp = (PQUERY_DEVICES_RESPONSE)outBuf;
        KeAcquireSpinLock(&g_Lock, &irql);
        resp->Count = g_TableCount;
        RtlCopyMemory(resp->Entries, g_Table, g_TableCount * sizeof(DEVICE_ASSIGNMENT));
        KeReleaseSpinLock(&g_Lock, irql);
        written = sizeof(*resp);
        break;
    }

    case IOCTL_MS_SET_SEAT_SESSION: {
        PSET_SEAT_SESSION_REQUEST req;
        if (InLen < sizeof(*req)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        WdfRequestRetrieveInputBuffer(Req, sizeof(*req), &inBuf, NULL);
        req = (PSET_SEAT_SESSION_REQUEST)inBuf;
        if (req->SeatIndex < MAX_SEATS) {
            KeAcquireSpinLock(&g_Lock, &irql);
            g_SeatSession[req->SeatIndex] = req->SessionId;
            KeReleaseSpinLock(&g_Lock, irql);
        } else {
            status = STATUS_INVALID_PARAMETER;
        }
        break;
    }

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
    }

    WdfRequestCompleteWithInformation(Req, status, written);
}

// ================================================================
//  UpdateCtxFromTable
// ================================================================
static VOID UpdateCtxFromTable(DEVICE_CTX* Ctx)
{
    KIRQL irql;
    KeAcquireSpinLock(&g_Lock, &irql);
    for (ULONG i = 0; i < g_TableCount; i++) {
        if (_wcsicmp(g_Table[i].DeviceId, Ctx->DeviceId) == 0) {
            Ctx->SeatIndex  = g_Table[i].SeatIndex;
            Ctx->IsKeyboard = (g_Table[i].DeviceType == DeviceTypeKeyboard);
            break;
        }
    }
    KeReleaseSpinLock(&g_Lock, irql);
}
