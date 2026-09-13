/*
 * hyterabulk.c - ARM64 KMDF driver for Hytera DMR programming cable
 *
 * Implements the bkrwbus-compatible device model that the Hytera CPS expects:
 *   - DeviceDesc "USB Bulk Device" (from INF)
 *   - Symbolic link \\.\usbbulk
 *   - CreateFile("\\\\.\\usbbulk\\pipe00") + ReadFile  -> bulk IN  (EP 0x81)
 *   - CreateFile("\\\\.\\usbbulk\\pipe01") + WriteFile -> bulk OUT (EP 0x01)
 *   - CreateFile("\\\\.\\usbbulk") + IOCTL -> control (rejected)
 *
 * v39: auto-run the codeplug block-read phase after the machine-info
 * exchange, exactly like the real bkrwbus (QEMU capture: CPS sends ONE
 * MCCI command and the driver performs the entire 5-packet init exchange
 * plus 64 block reads plus the end frame on its own).
 *
 * v41: defer reads on an empty inbox. The real MCCI driver pends read
 * requests until data arrives instead of returning 0 bytes immediately;
 * CPS's parser assumes blocking reads. The deferred request is
 * completed from the pump as soon as a packet is wrapped.
 *
 * v42: pace the block-read phase with a WDFTIMER (50ms per block
 * command). The reference radio answers block commands every ~50ms;
 * ours answers in ~5ms, so the driver was streaming the codeplug into
 * CPS 10x faster than the stack CPS was built against (CPS aborts the
 * read at ~block 8). The QEMU ground-truth run succeeded because TCG
 * emulation slowed the whole session back down.
 *
 * v43: fix the packet length field: wLen = flen + 2 (CPS's read
 * thread reads wLen-2 payload bytes after the 2-byte length read, so
 * wLen counts the length field itself). v39-v42 wrote wLen = flen,
 * leaving 2 stray bytes per frame that desynchronized CPS's parser
 * and caused the read abort.
 *
 * Routing is done per-request in the I/O callbacks by inspecting the
 * request's file object name; a single parallel default queue serves
 * everything.
 */
#include <ntddk.h>
#include <ntstrsafe.h>
#include <usb.h>
#include <wdf.h>
#include <wdfusb.h>

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD HyteraEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE HyteraEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE HyteraEvtReleaseHardware;
EVT_WDF_DEVICE_SURPRISE_REMOVAL HyteraEvtSurpriseRemoval;
EVT_WDF_IO_QUEUE_IO_READ HyteraEvtRead;
EVT_WDF_IO_QUEUE_IO_WRITE HyteraEvtWrite;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL HyteraEvtControl;
EVT_WDF_IO_QUEUE_IO_DEFAULT HyteraEvtDefault;
EVT_WDF_DEVICE_FILE_CREATE HyteraEvtFileCreate;
EVT_WDF_FILE_CLEANUP HyteraEvtFileCleanup;
EVT_WDF_REQUEST_COMPLETION_ROUTINE HyteraIoComplete;
EVT_WDF_REQUEST_COMPLETION_ROUTINE HyteraPumpComplete;
EVT_WDF_REQUEST_COMPLETION_ROUTINE HyteraPreambleComplete;
EVT_WDF_TIMER HyteraBlkTimer;
EVT_WDF_TIMER HyteraInitTimer;

static VOID
HyteraStartPump(
    _In_ WDFDEVICE Device
    );

#define HYTERA_FIFO_SIZE 131072
#define HYTERA_PUMP_SIZE 4096

// Frame templates for the session preamble the MCCI driver performs
// before the first command (QEMU capture: keepalive, init2, keepalive,
// then the command frame).
static const UCHAR HyteraPreamble1[] = {
    0x7e,0x00,0x00,0xfe,0x20,0x10,0x00,0x00,0x00,0x0c,0x60,0xe5
};
static const UCHAR HyteraPreamble2[] = {
    0x7e,0x01,0x00,0x00,0x20,0x10,0x00,0x00,0x00,0x14,0x33,0xd1,
    0x02,0x05,0x02,0x01,0x00,0x00,0x2a,0x03
};

// Remaining init frames, sent after init4 once its response arrives
// (QEMU capture pacing: each frame goes out after the previous one's
// response).
static const UCHAR HyteraInit5[] = {
    0x7e,0x01,0x00,0x00,0x20,0x10,0x00,0x00,0x00,0x24,0x02,0xf1,
    0x02,0xc5,0x01,0x11,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x5b,0x03
};
static const UCHAR HyteraInit6[] = {
    0x7e,0x01,0x00,0x00,0x20,0x10,0x00,0x00,0x00,0x14,0x41,0xc3,
    0x02,0x01,0x02,0x01,0x00,0x12,0x1c,0x03
};
static const UCHAR HyteraInit7[] = {
    0x7e,0x01,0x00,0x00,0x20,0x10,0x00,0x00,0x00,0x1c,0x08,0xf3,
    0x02,0xd3,0x01,0x09,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x55,0x03
};

// Block read prefixes indexed by addr/1500 (reverse-engineered from the
// CPS read session capture).
static const USHORT HyteraReadPrefixes[64] = {
    0x53a4,0x4da9,0x47af,0x41b5,0x3bbb,0x36c1,0x30c7,0x2acd,
    0x24d2,0x1ed8,0x18de,0x12e4,0x0cea,0x07f0,0x01f6,0xfbfa,
    0xf600,0xf006,0xea0c,0xe412,0xde18,0xd81e,0xd323,0xcd29,
    0xc72f,0xc135,0xbb3b,0xb541,0xaf47,0xa94c,0xa452,0x9e58,
    0x985e,0x9264,0x8c6a,0x8670,0x8075,0x7a7b,0x7481,0x6f87,
    0x698d,0x6393,0x5d99,0x579e,0x51a5,0x4bab,0x45b1,0x40b7,
    0x3abd,0x34c3,0x2ec8,0x28ce,0x22d4,0x1cda,0x16e0,0x10e6,
    0x0bec,0x05f1,0xfff6,0xf9fc,0xf402,0xee08,0xe80e,0xe413
};

// End-of-codeplug command (QEMU capture, byte-exact).
static const UCHAR HyteraEndCmd[] = {
    0x7e,0x01,0x00,0x00,0x20,0x10,0x00,0x00,0x00,0x14,0xf4,0x0f,
    0x02,0xc6,0x01,0x01,0x00,0x00,0x6a,0x03
};

// v70: canned 12B keepalive response (golden capture, byte-exact).
static const UCHAR HyteraKaResp[] = {
    0x7e,0x04,0x00,0xfd,0x10,0x20,0x00,0x00,0x00,0x0c,0x70,0xd2
};

// Codeplug layout (QEMU capture): 63 full blocks of 1500 bytes plus a
// final 1104-byte block; the radio returns the requested bytes prefixed
// by a 13-byte response header.
#define HYTERA_BLOCK_SIZE   1500
#define HYTERA_LAST_SIZE    1104
#define HYTERA_BLOCK_COUNT  64

// Build a block-read command frame (31 bytes). Data field (13 bytes):
// [00000000][0100][00][addr 3B LE][00][size 2B LE]; checksum covers
// ax + len + data: cks = (~sum + 0x33) & 0xff (verified on capture).
static void
HyteraBuildBlockCmd(
    _Out_ UCHAR frame[31],
    _In_ ULONG addr,
    _In_ BOOLEAN last
    )
{
    UCHAR data[13];
    ULONG sum;
    ULONG i;
    USHORT px;
    ULONG size = last ? HYTERA_LAST_SIZE : HYTERA_BLOCK_SIZE;

    data[0] = 0x00; data[1] = 0x00; data[2] = 0x00; data[3] = 0x00;
    data[4] = 0x01; data[5] = 0x00; data[6] = 0x00;
    data[7] = (UCHAR)(addr & 0xff);
    data[8] = (UCHAR)((addr >> 8) & 0xff);
    data[9] = (UCHAR)((addr >> 16) & 0xff);
    data[10] = 0x00;
    data[11] = (UCHAR)(size & 0xff);
    data[12] = (UCHAR)((size >> 8) & 0xff);

    frame[0] = 0x7e; frame[1] = 0x01; frame[2] = 0x00; frame[3] = 0x00;
    frame[4] = 0x20; frame[5] = 0x10; frame[6] = 0x00; frame[7] = 0x00;
    frame[8] = 0x00; frame[9] = 0x1f;                 // frame length 31
    px = HyteraReadPrefixes[(addr / HYTERA_BLOCK_SIZE) & 63];
    frame[10] = (UCHAR)(px >> 8);
    frame[11] = (UCHAR)(px & 0xff);
    frame[12] = 0x02;
    frame[13] = 0xc7; frame[14] = 0x01;               // ax 0x01c7
    frame[15] = 0x0c;                                 // HRCP data length
    for (i = 0; i < 13; i++) {
        frame[16 + i] = data[i];
    }
    sum = 0x01 + 0xc7 + 0x0c;
    for (i = 0; i < 13; i++) {
        sum += data[i];
    }
    frame[29] = (UCHAR)(((~sum) + 0x33) & 0xff);
    frame[30] = 0x03;
}

typedef struct _DEVICE_CONTEXT {
    WDFUSBDEVICE UsbDevice;
    WDFUSBPIPE BulkInPipe;   // EP 0x81
    WDFUSBPIPE BulkOutPipe;  // EP 0x01

    // Receive FIFO. A background pump keeps a read in flight on the bulk
    // IN pipe at all times and appends data here. Application reads draw
    // from the FIFO and return 0 bytes immediately when it is empty.
    // CPS issues 1- and 2-byte probe reads which WDF rejects on a bulk
    // pipe (STATUS_INVALID_BUFFER_SIZE: buffer must be a multiple of the
    // maximum packet size), so the driver never forwards user reads to
    // the pipe directly.
    UCHAR Fifo[HYTERA_FIFO_SIZE];
    ULONG FifoLen;
    WDFSPINLOCK FifoLock;

    // Pump state: one driver-created request in flight on the bulk IN
    // pipe, plus its buffer.
    WDFMEMORY PumpMem;
    BOOLEAN PumpActive;

    // MCCI translation state: preamble done flag.
    BOOLEAN SessionStarted;

    // v86: set when the 20-class stack (hfc_portusb/hfc_hrcp10) writes a
    // raw wire frame (0x7e...) on the device-interface handle. The app
    // then drives its own session (keepalives, queries) - the driver
    // must not inject its 10-class preamble on top.
    BOOLEAN AppDriven;

    // v88: keepalive-answer tracking for the 20-class connect handshake.
    BOOLEAN KaWait;
    BOOLEAN KaAcked;
    BOOLEAN KaInjected;

    // v90: staged raw frames for the 20-class app (paced delivery).
    UCHAR Stage[16384];
    ULONG StageLen;

    // v91: set once a non-keepalive frame has been delivered; later 12B
    // frames are stale keepalive acks the app's parser must not see.
    BOOLEAN SawBigFrame;

    // v93: only ONE 12B frame per session reaches the app (the connect
    // ack). Any later 12B frame (stray/delayed radio keepalive answer)
    // desyncs the app's frame-expectation and is dropped.
    BOOLEAN KaDelivered;

    // v74: set when the PnP manager reports surprise removal (or the
    // orderly ReleaseHardware runs). Every path that touches the USB
    // pipes, the pump, or the deferred-read list must bail out while
    // this is set: the 0x2B PANIC_STACK_SWITCH BSODs on detach were
    // completion routines racing the teardown. Reset in PrepareHardware.
    BOOLEAN Removing;

    // Init sequence state machine: after cmd1 (init4) the driver keeps
    // driving the machine-info exchange, sending the next init frame as
    // each response arrives (QEMU capture pacing).
    // 0 = idle, 1 = init4 sent, 2 = init5 sent, 3 = init6 sent,
    // 4 = f3d3 sent, 5 = done.
    ULONG InitStep;

    // Block-read phase (v39): after the exchange response the driver
    // auto-runs the codeplug read like the real bkrwbus. BlkIdx = next
    // command (0..63 block, 64 end, 65 stop); BlkQueue = responses seen
    // minus commands sent; BlkPending = waiting for FIFO space.
    ULONG BlkIdx;
    ULONG BlkQueue;
    BOOLEAN BlkPending;

    // v42: block-phase pacing timer (50ms/block, reference-radio rate).
    WDFTIMER BlkTimer;

    // v75: init-sequence pacing. The reference capture shows the real
    // driver sending each next init frame ~40-90ms AFTER the preceding
    // response; at our 1-4ms rate the radio rejects the init7 security
    // exchange with an f001 error frame instead of the 81d3 data frame,
    // and CPS's machine-info stage (which expects the 81d3) fails with
    // S0107403. PendingInit holds the next init frame; InitTimer is a
    // dedicated one-shot delay so the block path is not disturbed.
    ULONG PendingInit;
    WDFTIMER InitTimer;

    // Open-handle count; the whole session resets when the last handle
    // closes so the next CPS run starts with a clean preamble.
    ULONG OpenCount;

    // v68: deferred reads live in a private list, NOT a WDF queue.
    // A WDF-managed queue races the framework's automatic cancellation
    // (file close / surprise removal) against our own completion and
    // double-completes requests -> 0x2B PANIC_STACK_SWITCH BSODs on
    // unplug and on session teardown. With a private list all access
    // is under FifoLock and the lifecycle is fully driver-owned.
    WDFREQUEST PendReq[8];
    PVOID PendBuf[8];
    size_t PendLen[8];
    BOOLEAN PendMarked[8];   // v74: slot's request has MarkCancelable applied
    ULONG PendCount;

    // Frame assembly for the pump: partial USB transfers accumulate here
    // until a complete Hytera frame is available. Complete frames are
    // wrapped as MCCI packets ([wLen 2B LE][frame]) and appended to the
    // FIFO, because CPS parses its reads as length-prefixed packets
    // (it reads 2 bytes for the length, then the payload).
    UCHAR WrapBuf[8192];
    ULONG WrapLen;

    CHAR DbgLog[32768];
    ULONG DbgIdx;
    ULONG DbgWall;          // v97: one-time wall-clock stamp (FILETIME secs)
    LARGE_INTEGER DbgBase;
} DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetDeviceContext)

EVT_WDF_REQUEST_CANCEL HyteraPendCancel;

static void
HyteraInjectFrame(
    _In_ DEVICE_CONTEXT* ctx,
    _In_reads_bytes_(flen) const UCHAR* frame,
    _In_ ULONG flen
    );

// MCCI bkrwbus device interface GUID. CPS enumerates this interface
// and opens the device through it (\\?\usb#vid_238b...#{GUID}).
// Found in the CPS binary (20 occurrences).
static const GUID MCCI_BKRWBUS_INTERFACE_GUID = {
    0x15005312, 0xB672, 0x4817,
    { 0x94, 0xBF, 0xA5, 0x07, 0xAD, 0x4E, 0x05, 0x7E }
};

// Second MCCI GUID seen in the CPS binary (12 occurrences); register
// both so CPS finds the device regardless of which one it queries.
static const GUID MCCI_BKRWBUS_INTERFACE_GUID2 = {
    0x87E5A6EA, 0xD48B, 0x4883,
    { 0x84, 0x40, 0x81, 0xD8, 0xA2, 0x25, 0x08, 0xD7 }
};

// USB-port device interface GUID registered by MCCI's bkrwbk.sys.
// The 20-class user-mode stack (hfc_portusb.dll) enumerates devices
// with SetupDiGetClassDevsA(DIGCF_DEVICEINTERFACE) on this GUID and
// opens the resulting device path for overlapped read/write.
static const GUID HYTERA_USBPORT_INTERFACE_GUID = {
    0x8FF89775, 0xCB39, 0x41C3,
    { 0xA1, 0x70, 0xA5, 0x2E, 0x99, 0x0F, 0xA3, 0x31 }
};

static void
HyteraDbg(
    _In_ WDFDEVICE Device,
    _In_ PCSTR Msg,
    _In_ NTSTATUS Status
    )
{
    DEVICE_CONTEXT* ctx = GetDeviceContext(Device);
    ULONG i;
    LARGE_INTEGER now;
    ULONG ms;

    // v44: prefix every event with a 4-hex-digit millisecond timestamp
    // (low 16 bits, 65s window) so we can measure CPS's pacing and where
    // it gives up.
    KeQuerySystemTime(&now);
    if (ctx->DbgBase.QuadPart == 0) {
        ctx->DbgBase.QuadPart = now.QuadPart;
    }
    ms = (ULONG)((now.QuadPart - ctx->DbgBase.QuadPart) / 10000);

    // v96: circular wrap. Once the log is full (CPS block reads emit
    // tens of KB), restart from 0 so the log always holds the most
    // recent 32KB. All appends below stay individually bounds-checked.
    if (ctx->DbgIdx > sizeof(ctx->DbgLog) - 64) {
        ctx->DbgIdx = 0;
    }

    // v97: one-time wall-clock stamp (low 32 bits of FILETIME seconds).
    // A ring dump otherwise only carries the 16-bit ms tick; this maps
    // the log onto run-log wall times. Bounded: at most 12 bytes past
    // the guarded loops, well inside the buffer.
    if (ctx->DbgWall == 0) {
        ULONG wsec = (ULONG)(now.QuadPart / 10000000ULL);
        ctx->DbgWall = wsec ? wsec : 1;
        for (i = 0; i < 4 && ctx->DbgIdx < sizeof(ctx->DbgLog) - 40; i++) {
            ctx->DbgLog[ctx->DbgIdx++] = "0123456789abcdef"[(ms >> (12 - i * 4)) & 0xF];
        }
        ctx->DbgLog[ctx->DbgIdx++] = 'w';
        ctx->DbgLog[ctx->DbgIdx++] = 'l';
        ctx->DbgLog[ctx->DbgIdx++] = ':';
        for (i = 0; i < 8 && ctx->DbgIdx < sizeof(ctx->DbgLog) - 40; i++) {
            ctx->DbgLog[ctx->DbgIdx++] = "0123456789abcdef"[(wsec >> (28 - i * 4)) & 0xF];
        }
        ctx->DbgLog[ctx->DbgIdx++] = ';';
    }

    for (i = 0; i < 4 && ctx->DbgIdx < sizeof(ctx->DbgLog) - 40; i++) {
        ctx->DbgLog[ctx->DbgIdx++] = "0123456789abcdef"[(ms >> (12 - i * 4)) & 0xF];
    }

    // Manual logger: no RtlStringCchPrintfA dependency.
    // EVERY append must be bounds-checked. The ':' and ';' separators
    // used to be unconditional: once DbgLog filled up (a few hundred
    // I/Os - CPS does thousands per read), each call wrote 2 bytes past
    // the context into the adjacent non-paged pool, corrupting pool
    // headers/lookaside lists (";:;:" pattern). The corruption surfaced
    // later on unrelated threads (0x7E in nt!ExpScanGeneralLookasideList,
    // 0x139 in KiTryUnwaitThread) - the stack never showed hyterabulk.
    for (i = 0; Msg[i] != 0 && ctx->DbgIdx < sizeof(ctx->DbgLog) - 40; i++) {
        ctx->DbgLog[ctx->DbgIdx++] = Msg[i];
    }
    if (ctx->DbgIdx < sizeof(ctx->DbgLog)) {
        ctx->DbgLog[ctx->DbgIdx++] = ':';
    }
    for (i = 0; i < 8 && ctx->DbgIdx < sizeof(ctx->DbgLog) - 40; i++) {
        ULONG nibble = ((ULONG)Status >> (28 - i * 4)) & 0xF;
        ctx->DbgLog[ctx->DbgIdx++] = "0123456789abcdef"[nibble];
    }
    if (ctx->DbgIdx < sizeof(ctx->DbgLog)) {
        ctx->DbgLog[ctx->DbgIdx++] = ';';
    }
}

// Send a raw frame on the bulk OUT pipe. Driver-created requests use
// HyteraPreambleComplete, which WdfObjectDelete's them.
static void
HyteraSendBuf(
    _In_ WDFDEVICE Device,
    _In_ DEVICE_CONTEXT* ctx,
    _In_ const UCHAR* bytes,
    _In_ ULONG len
    )
{
    WDFMEMORY mem;
    WDFREQUEST preq;
    NTSTATUS status;

    if (ctx->BulkOutPipe == WDF_NO_HANDLE) {
        return;
    }
    status = WdfMemoryCreate(WDF_NO_OBJECT_ATTRIBUTES,
                             NonPagedPool, 0, len, &mem, NULL);
    if (!NT_SUCCESS(status)) {
        return;
    }
    RtlCopyMemory(WdfMemoryGetBuffer(mem, NULL), bytes, len);
    status = WdfRequestCreate(WDF_NO_OBJECT_ATTRIBUTES,
                              WdfUsbTargetPipeGetIoTarget(ctx->BulkOutPipe),
                              &preq);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(mem);
        return;
    }
    status = WdfUsbTargetPipeFormatRequestForWrite(
        ctx->BulkOutPipe, preq, mem, NULL);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(preq);
        WdfObjectDelete(mem);
        return;
    }
    WdfRequestSetCompletionRoutine(preq, HyteraPreambleComplete,
                                   (WDFCONTEXT)ctx);
    if (WdfRequestSend(preq, WdfUsbTargetPipeGetIoTarget(ctx->BulkOutPipe),
                       WDF_NO_SEND_OPTIONS) == FALSE) {
        // v51: the send already completed synchronously (or failed).
        HyteraDbg(Device, "snd0", (NTSTATUS)len);
    } else {
        HyteraDbg(Device, "snd", (NTSTATUS)len);
    }
}

// Drain the queued block-phase sends. One block command goes out per
// received block response (1:1 radio pacing). Sends wait for FIFO space
// so CPS's reads can never fall behind and lose codeplug data.
static void
HyteraFlushBlockQueue(
    _In_ WDFDEVICE Device,
    _In_ DEVICE_CONTEXT* ctx
    )
{
    UCHAR frame[31];
    BOOLEAN send = FALSE;
    BOOLEAN isEnd = FALSE;

    for (;;) {
        WdfSpinLockAcquire(ctx->FifoLock);
        if (ctx->BlkQueue == 0) {
            ctx->BlkPending = FALSE;
            WdfSpinLockRelease(ctx->FifoLock);
            return;
        }
        if (HYTERA_FIFO_SIZE - ctx->FifoLen < 4000) {
            ctx->BlkPending = TRUE;
            WdfSpinLockRelease(ctx->FifoLock);
            return;
        }
        if (ctx->BlkIdx < HYTERA_BLOCK_COUNT) {
            ULONG addr = ctx->BlkIdx * HYTERA_BLOCK_SIZE;
            BOOLEAN last = (ctx->BlkIdx == HYTERA_BLOCK_COUNT - 1);
            HyteraBuildBlockCmd(frame, addr, last);
            ctx->BlkIdx++;
            ctx->BlkQueue--;
            send = TRUE;
            isEnd = FALSE;
        } else if (ctx->BlkIdx == HYTERA_BLOCK_COUNT) {
            RtlCopyMemory(frame, HyteraEndCmd, sizeof(HyteraEndCmd));
            ctx->BlkIdx++;
            ctx->BlkQueue--;
            send = TRUE;
            isEnd = TRUE;
        } else {
            ctx->BlkQueue = 0;
            ctx->BlkPending = FALSE;
            WdfSpinLockRelease(ctx->FifoLock);
            return;
        }
        WdfSpinLockRelease(ctx->FifoLock);

        if (send) {
            ULONG n = isEnd ? sizeof(HyteraEndCmd) : 31;
            HyteraSendBuf(Device, ctx, frame, n);
            HyteraDbg(Device, isEnd ? "end" : "blk",
                      (NTSTATUS)(isEnd ? 0 : ctx->BlkIdx - 1));
            send = FALSE;
        }
    }
}

// v71: cancellation for deferred reads. Without this, an application
// cancel/exit left the pended IRP forever incomplete and the process
// became unkillable (CPS froze and taskkill could not terminate it).
// The request is not in any WDF queue, so this callback is the ONLY
// cancellation path - no framework race.
VOID
HyteraPendCancel(
    _In_ WDFREQUEST Request
    )
{
    DEVICE_CONTEXT* ctx;
    ULONG i;

    ctx = GetDeviceContext(WdfIoQueueGetDevice(WdfRequestGetIoQueue(Request)));
    WdfSpinLockAcquire(ctx->FifoLock);
    for (i = 0; i < ctx->PendCount; i++) {
        if (ctx->PendReq[i] == Request) {
            ULONG j;
            for (j = i + 1; j < ctx->PendCount; j++) {
                ctx->PendReq[j - 1] = ctx->PendReq[j];
                ctx->PendBuf[j - 1] = ctx->PendBuf[j];
                ctx->PendLen[j - 1] = ctx->PendLen[j];
                ctx->PendMarked[j - 1] = ctx->PendMarked[j];
            }
            ctx->PendCount--;
            break;
        }
    }
    WdfSpinLockRelease(ctx->FifoLock);

    WdfRequestComplete(Request, STATUS_CANCELLED);
}

// v68: serve deferred reads from the private list with whatever the
// FIFO holds. All list access happens under FifoLock; requests are
// completed after the lock is dropped (one at a time).
// v92: length of the frame at the FIFO head (raw wire frame: BE u16 at [8:10]).
static ULONG
HyteraFirstFrameLen(
    _In_ DEVICE_CONTEXT* ctx
    )
{
    ULONG len;
    if (ctx->FifoLen < 12) {
        return ctx->FifoLen;   // incomplete header; hand out what we have
    }
    len = ((ULONG)ctx->Fifo[8] << 8) | (ULONG)ctx->Fifo[9];
    if (len < 12 || len > ctx->FifoLen) {
        return ctx->FifoLen;   // garbage header; don't deadlock
    }
    return len;
}

static void
HyteraCompletePendingRead(
    _In_ WDFDEVICE Device,
    _In_ DEVICE_CONTEXT* ctx
    )
{
    for (;;) {
        WDFREQUEST req = WDF_NO_HANDLE;
        PVOID buf;
        size_t len;
        ULONG copy = 0;
        NTSTATUS status;

        UNREFERENCED_PARAMETER(Device);

        WdfSpinLockAcquire(ctx->FifoLock);
        if (ctx->FifoLen == 0 || ctx->PendCount == 0) {
            WdfSpinLockRelease(ctx->FifoLock);
            return;
        }
        // v74: a slot is only servable once its request has completed
        // WdfRequestMarkCancelable. An unmarked slot belongs to an
        // EvtRead that is still in flight; touching it would call
        // WdfRequestUnmarkCancelable on an unmarked request (framework
        // fast-fail). That EvtRead calls us back after marking.
        if (!ctx->PendMarked[0]) {
            WdfSpinLockRelease(ctx->FifoLock);
            return;
        }
        req = ctx->PendReq[0];
        buf = ctx->PendBuf[0];
        len = ctx->PendLen[0];
        copy = (ULONG)(ctx->FifoLen < len ? ctx->FifoLen : len);
        if (ctx->AppDriven) {
            ULONG fl2 = HyteraFirstFrameLen(ctx);
            if (copy > fl2) {
                copy = fl2;
            }
        }
        RtlCopyMemory(buf, ctx->Fifo, copy);
        RtlMoveMemory(ctx->Fifo, ctx->Fifo + copy, ctx->FifoLen - copy);
        ctx->FifoLen -= copy;
        {
            ULONG i;
            for (i = 1; i < ctx->PendCount; i++) {
                ctx->PendReq[i - 1] = ctx->PendReq[i];
                ctx->PendBuf[i - 1] = ctx->PendBuf[i];
                ctx->PendLen[i - 1] = ctx->PendLen[i];
                ctx->PendMarked[i - 1] = ctx->PendMarked[i];
            }
            ctx->PendCount--;
        }
        WdfSpinLockRelease(ctx->FifoLock);

        status = WdfRequestUnmarkCancelable(req);
        if (status != STATUS_CANCELLED) {
            WdfRequestSetInformation(req, copy);
            WdfRequestComplete(req, STATUS_SUCCESS);
        }
    }
}

// v68: complete every deferred read with the given status (teardown
// paths only; the list is driver-private so there is no framework race).
// v74: only slots whose request is already marked cancelable may be
// claimed here. Unmarked slots belong to an EvtRead still between list
// insertion and WdfRequestMarkCancelable; that path owns them and the
// close/removal cancellation will deliver the callback once marked.
static void
HyteraCancelPendingReads(
    _In_ DEVICE_CONTEXT* ctx,
    _In_ NTSTATUS Status
    )
{
    WDFREQUEST reqs[8];
    ULONG n = 0;
    ULONG i;

    WdfSpinLockAcquire(ctx->FifoLock);
    for (i = 0; i < ctx->PendCount; ) {
        if (!ctx->PendMarked[i]) {
            i++;
            continue;
        }
        reqs[n++] = ctx->PendReq[i];
        {
            ULONG j;
            for (j = i + 1; j < ctx->PendCount; j++) {
                ctx->PendReq[j - 1] = ctx->PendReq[j];
                ctx->PendBuf[j - 1] = ctx->PendBuf[j];
                ctx->PendLen[j - 1] = ctx->PendLen[j];
                ctx->PendMarked[j - 1] = ctx->PendMarked[j];
            }
        }
        ctx->PendCount--;
    }
    WdfSpinLockRelease(ctx->FifoLock);

    for (i = 0; i < n; i++) {
        if (WdfRequestUnmarkCancelable(reqs[i]) != STATUS_CANCELLED) {
            WdfRequestComplete(reqs[i], Status);
        }
    }
}

// v58: session preamble (keepalive, model query, keepalive). The real
// driver sends this as soon as CPS opens the read pipe, so CPS's
// connection probe (a 1-byte read right after open) gets answered
// immediately. Waiting for the 12-byte write left that probe pending
// for a full second and CPS reported "连接失败" at the end of the read.
static void
HyteraSendPreamble(
    _In_ WDFDEVICE Device,
    _In_ DEVICE_CONTEXT* ctx
    )
{
    ULONG plen[4];
    const UCHAR* pframes[4];
    ULONG i;
    NTSTATUS status;

    if (ctx->BulkOutPipe == WDF_NO_HANDLE) {
        return;
    }

    plen[0] = sizeof(HyteraPreamble1);
    pframes[0] = HyteraPreamble1;
    plen[1] = sizeof(HyteraPreamble2);
    pframes[1] = HyteraPreamble2;
    plen[2] = sizeof(HyteraPreamble1);
    pframes[2] = HyteraPreamble1;
    // v75: the reference session shows exactly [keepalive][model query]
    // [keepalive] before the serial query; v69.1's extra keepalive put
    // an extra keepalive RESPONSE in CPS's delivered stream where the
    // reference has the serial response (8203). Back to three frames.
    for (i = 0; i < 3; i++) {
        WDFMEMORY mem;
        WDFREQUEST preq;

        status = WdfMemoryCreate(WDF_NO_OBJECT_ATTRIBUTES,
                                 NonPagedPool, 0, plen[i], &mem, NULL);
        if (!NT_SUCCESS(status)) {
            continue;
        }
        RtlCopyMemory(WdfMemoryGetBuffer(mem, NULL), (PVOID)pframes[i],
                      plen[i]);
        status = WdfRequestCreate(WDF_NO_OBJECT_ATTRIBUTES,
                                  WdfUsbTargetPipeGetIoTarget(ctx->BulkOutPipe),
                                  &preq);
        if (!NT_SUCCESS(status)) {
            WdfObjectDelete(mem);
            continue;
        }
        status = WdfUsbTargetPipeFormatRequestForWrite(
            ctx->BulkOutPipe, preq, mem, NULL);
        if (!NT_SUCCESS(status)) {
            WdfObjectDelete(preq);
            WdfObjectDelete(mem);
            continue;
        }
        WdfRequestSetCompletionRoutine(preq, HyteraPreambleComplete,
                                       (WDFCONTEXT)ctx);
        WdfRequestSend(preq, WdfUsbTargetPipeGetIoTarget(ctx->BulkOutPipe),
                       WDF_NO_SEND_OPTIONS);
    }
    HyteraDbg(Device, "pre", STATUS_SUCCESS);
}

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    WDF_DRIVER_CONFIG config;
    NTSTATUS status;

    WDF_DRIVER_CONFIG_INIT(&config, HyteraEvtDeviceAdd);
    config.EvtDriverUnload = WDF_NO_EVENT_CALLBACK;

    status = WdfDriverCreate(DriverObject,
                             RegistryPath,
                             WDF_NO_OBJECT_ATTRIBUTES,
                             &config,
                             WDF_NO_HANDLE);
    return status;
}

NTSTATUS
HyteraEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
    )
{
    NTSTATUS status;
    WDFDEVICE device;
    WDF_OBJECT_ATTRIBUTES attrs;
    WDF_IO_QUEUE_CONFIG qcfg;
    DECLARE_CONST_UNICODE_STRING(linkName, L"\\DosDevices\\usbbulk");

    UNREFERENCED_PARAMETER(Driver);

    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_UNKNOWN);
    WdfDeviceInitSetExclusive(DeviceInit, FALSE);

    // PnP/power callbacks must be registered before WdfDeviceCreate.
    {
        WDF_PNPPOWER_EVENT_CALLBACKS pnp;
        WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
        pnp.EvtDevicePrepareHardware = HyteraEvtPrepareHardware;
        pnp.EvtDeviceReleaseHardware = HyteraEvtReleaseHardware;
        pnp.EvtDeviceSurpriseRemoval = HyteraEvtSurpriseRemoval;
        WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);
    }

    // File create callback for logging only; framework completes the
    // request automatically after the callback returns.
    // NOTE: WDF_FILEOBJECT_CONFIG_INIT slots are (Create, Close, Cleanup).
    // Registering HyteraEvtFileCreate in the Cleanup slot was the CPS BSOD
    // root cause: cleanup fires on every CloseHandle and called the
    // three-argument create callback with garbage parameters.
    {
        WDF_FILEOBJECT_CONFIG foc;
        WDF_FILEOBJECT_CONFIG_INIT(&foc, HyteraEvtFileCreate, WDF_NO_EVENT_CALLBACK, HyteraEvtFileCleanup);
        WdfDeviceInitSetFileObjectConfig(DeviceInit, &foc, WDF_NO_OBJECT_ATTRIBUTES);
    }

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, DEVICE_CONTEXT);
    status = WdfDeviceCreate(&DeviceInit, &attrs, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // Single parallel default queue for all I/O; routing happens per-request.
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&qcfg, WdfIoQueueDispatchParallel);
    qcfg.EvtIoRead = HyteraEvtRead;
    qcfg.EvtIoWrite = HyteraEvtWrite;
    qcfg.EvtIoDeviceControl = HyteraEvtControl;
    qcfg.EvtIoDefault = HyteraEvtDefault;
    status = WdfIoQueueCreate(device, &qcfg, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = WdfDeviceCreateSymbolicLink(device, &linkName);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // CPS discovers the device through the MCCI interface GUID(s).
    status = WdfDeviceCreateDeviceInterface(device, &MCCI_BKRWBUS_INTERFACE_GUID, NULL);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = WdfDeviceCreateDeviceInterface(device, &MCCI_BKRWBUS_INTERFACE_GUID2, NULL);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    // v85: the 20-class stack (hfc_portusb) enumerates this bkrwbk GUID.
    status = WdfDeviceCreateDeviceInterface(device, &HYTERA_USBPORT_INTERFACE_GUID, NULL);
    HyteraDbg(device, "dvi", status);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
HyteraEvtPrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    NTSTATUS status;
    DEVICE_CONTEXT* ctx = GetDeviceContext(Device);
    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS selCfg;
    UCHAR i, j, numInterfaces, numPipes;

    UNREFERENCED_PARAMETER(ResourcesRaw);
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    status = WdfUsbTargetDeviceCreate(Device, WDF_NO_OBJECT_ATTRIBUTES, &ctx->UsbDevice);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // Configure ALL interfaces in the active configuration; the radio's
    // bulk pipes live on a non-zero interface number, and single-interface
    // select would grab the wrong one.
    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS_INIT_MULTIPLE_INTERFACES(&selCfg, 0, NULL);
    status = WdfUsbTargetDeviceSelectConfig(ctx->UsbDevice, WDF_NO_OBJECT_ATTRIBUTES, &selCfg);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    numInterfaces = WdfUsbTargetDeviceGetNumInterfaces(ctx->UsbDevice);
    for (i = 0; i < numInterfaces; i++) {
        WDFUSBINTERFACE iface = WdfUsbTargetDeviceGetInterface(ctx->UsbDevice, i);
        numPipes = WdfUsbInterfaceGetNumConfiguredPipes(iface);
        for (j = 0; j < numPipes; j++) {
            WDFUSBPIPE pipe;
            WDF_USB_PIPE_INFORMATION info;
            WDF_USB_PIPE_INFORMATION_INIT(&info);
            pipe = WdfUsbInterfaceGetConfiguredPipe(iface, j, &info);
            if (pipe == WDF_NO_HANDLE) {
                continue;
            }
            if (info.PipeType == WdfUsbPipeTypeBulk) {
                if (WDF_USB_PIPE_DIRECTION_IN(info.EndpointAddress)) {
                    ctx->BulkInPipe = pipe;
                } else {
                    ctx->BulkOutPipe = pipe;
                }
            }
        }
    }

    // FIFO + background pump so user reads are non-blocking drains.
    {
        WDF_OBJECT_ATTRIBUTES lockAttrs;
        WDF_OBJECT_ATTRIBUTES memAttrs;
        WDFMEMORY mem;

        ctx->FifoLen = 0;
        ctx->PumpActive = FALSE;
        ctx->Removing = FALSE;   // v74: device restarted (e.g. disable/enable)
        ctx->SessionStarted = FALSE;
        ctx->AppDriven = FALSE;
        ctx->StageLen = 0;
        ctx->SawBigFrame = FALSE;
        ctx->KaDelivered = FALSE;
        ctx->KaWait = FALSE;
        ctx->KaAcked = FALSE;
        ctx->KaInjected = FALSE;
        ctx->InitStep = 0;
        ctx->BlkQueue = 0;
        ctx->BlkPending = FALSE;
        ctx->PendCount = 0;
        RtlZeroMemory(ctx->PendMarked, sizeof(ctx->PendMarked));

        WDF_OBJECT_ATTRIBUTES_INIT(&lockAttrs);
        lockAttrs.ParentObject = Device;
        status = WdfSpinLockCreate(&lockAttrs, &ctx->FifoLock);
        if (!NT_SUCCESS(status)) {
            return status;
        }

        WDF_OBJECT_ATTRIBUTES_INIT(&memAttrs);
        memAttrs.ParentObject = Device;
        status = WdfMemoryCreate(&memAttrs, NonPagedPool, 0,
                                HYTERA_PUMP_SIZE, &mem, NULL);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        ctx->PumpMem = mem;
        HyteraStartPump(Device);
    }

    // Block-phase pacing timer (parented to the device so WDF deletes it
    // with the device; default execution level is passive).
    {
        WDF_OBJECT_ATTRIBUTES tAttrs;
        WDF_TIMER_CONFIG tcfg;
        WDF_OBJECT_ATTRIBUTES_INIT(&tAttrs);
        tAttrs.ParentObject = Device;
        WDF_TIMER_CONFIG_INIT(&tcfg, HyteraBlkTimer);
        // NOTE: this WDK's WdfTimerCreate takes (Config, Attributes, Timer).
        status = WdfTimerCreate(&tcfg, &tAttrs, &ctx->BlkTimer);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }

    // v75: dedicated one-shot timer for init-sequence pacing.
    {
        WDF_OBJECT_ATTRIBUTES tAttrs;
        WDF_TIMER_CONFIG tcfg;
        WDF_OBJECT_ATTRIBUTES_INIT(&tAttrs);
        tAttrs.ParentObject = Device;
        WDF_TIMER_CONFIG_INIT(&tcfg, HyteraInitTimer);
        status = WdfTimerCreate(&tcfg, &tAttrs, &ctx->InitTimer);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }

    return STATUS_SUCCESS;
}

// v74: surprise removal (USB unplug / VM USB reassignment) is called
// BEFORE the USB stack aborts in-flight pipe requests. Setting Removing
// here makes the pump completion (which still fires afterwards, at
// DISPATCH_LEVEL, when the aborted read completes) take the early-exit
// path instead of touching the FIFO, completing deferred reads, and
// re-arming the pump against a half-dead device stack.
VOID
HyteraEvtSurpriseRemoval(
    _In_ WDFDEVICE Device
    )
{
    DEVICE_CONTEXT* ctx = GetDeviceContext(Device);

    ctx->Removing = TRUE;
    ctx->PumpActive = TRUE;
    ctx->BulkInPipe = WDF_NO_HANDLE;
    ctx->BulkOutPipe = WDF_NO_HANDLE;
    ctx->SessionStarted = FALSE;
        ctx->AppDriven = FALSE;
        ctx->StageLen = 0;
        ctx->SawBigFrame = FALSE;
        ctx->KaDelivered = FALSE;
        ctx->KaWait = FALSE;
        ctx->KaAcked = FALSE;
        ctx->KaInjected = FALSE;

    HyteraCancelPendingReads(ctx, STATUS_CANCELLED);
    HyteraDbg(Device, "surprise", STATUS_SUCCESS);
}

// v57: device removal (USB unplug / VM re-enumeration). Stop the pump
// and cancel deferred reads so no completion routine runs against a
// half-torn-down device. The 0x2B PANIC_STACK_SWITCH BSODs (16:18 and
// 22:38) both happened while the radio was being detached from the VM.
NTSTATUS
HyteraEvtReleaseHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    DEVICE_CONTEXT* ctx = GetDeviceContext(Device);

    UNREFERENCED_PARAMETER(ResourcesTranslated);

    // v74: same hardening as surprise removal (this also runs on the
    // orderly path, where SurpriseRemoval was never called).
    ctx->Removing = TRUE;

    // Block pump re-arms; in-flight USB reads get cancelled by the USB
    // stack during removal and their completion routine returns early.
    ctx->PumpActive = TRUE;
    ctx->BulkInPipe = WDF_NO_HANDLE;
    ctx->BulkOutPipe = WDF_NO_HANDLE;
    ctx->SessionStarted = FALSE;
        ctx->AppDriven = FALSE;
        ctx->StageLen = 0;
        ctx->SawBigFrame = FALSE;
        ctx->KaDelivered = FALSE;
        ctx->KaWait = FALSE;
        ctx->KaAcked = FALSE;
        ctx->KaInjected = FALSE;

    // v68: complete private deferred reads on removal.
    HyteraCancelPendingReads(ctx, STATUS_CANCELLED);
    HyteraDbg(Device, "relhw", STATUS_SUCCESS);
    return STATUS_SUCCESS;
}

// v42: fires after each block response; sends the queued block command.
// One-shot, re-armed by the pump when the next response arrives, so
// commands go out ~50ms after each response (radio answers ~5ms later,
// ~55ms command spacing: the reference radio's rate).
// v75: send the init frame that was held back for pacing.
VOID
HyteraInitTimer(
    _In_ WDFTIMER Timer
    )
{
    WDFDEVICE device = (WDFDEVICE)WdfTimerGetParentObject(Timer);
    DEVICE_CONTEXT* ctx = GetDeviceContext(device);
    ULONG nextSend;

    WdfSpinLockAcquire(ctx->FifoLock);
    nextSend = ctx->PendingInit;
    ctx->PendingInit = 0;
    WdfSpinLockRelease(ctx->FifoLock);

    if (nextSend != 0) {
        const UCHAR* f = NULL;
        ULONG fl = 0;
        WDFMEMORY mem;
        WDFREQUEST preq;

        if (nextSend == 5) { f = HyteraInit5; fl = sizeof(HyteraInit5); }
        else if (nextSend == 6) { f = HyteraInit6; fl = sizeof(HyteraInit6); }
        else if (nextSend == 7) { f = HyteraInit7; fl = sizeof(HyteraInit7); }

        if (f != NULL && ctx->BulkOutPipe != WDF_NO_HANDLE &&
            NT_SUCCESS(WdfMemoryCreate(WDF_NO_OBJECT_ATTRIBUTES,
                                       NonPagedPool, 0, fl, &mem, NULL))) {
            RtlCopyMemory(WdfMemoryGetBuffer(mem, NULL), f, fl);
            if (NT_SUCCESS(WdfRequestCreate(
                    WDF_NO_OBJECT_ATTRIBUTES,
                    WdfUsbTargetPipeGetIoTarget(ctx->BulkOutPipe),
                    &preq))) {
                if (NT_SUCCESS(WdfUsbTargetPipeFormatRequestForWrite(
                        ctx->BulkOutPipe, preq, mem, NULL))) {
                    WdfRequestSetCompletionRoutine(
                        preq, HyteraPreambleComplete, (WDFCONTEXT)ctx);
                    WdfRequestSend(preq,
                        WdfUsbTargetPipeGetIoTarget(ctx->BulkOutPipe),
                        WDF_NO_SEND_OPTIONS);
                    mem = NULL;
                } else {
                    WdfObjectDelete(preq);
                }
            }
            if (mem != NULL) {
                WdfObjectDelete(mem);
            }
        }
        HyteraDbg(device, "ns", (NTSTATUS)nextSend);
    }
}

VOID
HyteraBlkTimer(
    _In_ WDFTIMER Timer
    )
{
    WDFDEVICE device = (WDFDEVICE)WdfTimerGetParentObject(Timer);
    DEVICE_CONTEXT* ctx = GetDeviceContext(device);

    // v90: release staged raw frames to the app FIFO (paced delivery).
    WdfSpinLockAcquire(ctx->FifoLock);
    if (ctx->StageLen > 0 &&
        HYTERA_FIFO_SIZE - ctx->FifoLen >= ctx->StageLen) {
        RtlCopyMemory(ctx->Fifo + ctx->FifoLen, ctx->Stage, ctx->StageLen);
        ctx->FifoLen += ctx->StageLen;
        HyteraDbg(device, "rel", (NTSTATUS)ctx->StageLen);
        ctx->StageLen = 0;
    }
    WdfSpinLockRelease(ctx->FifoLock);
    HyteraCompletePendingRead(device, ctx);

    if (ctx->BlkQueue > 0) {
        HyteraFlushBlockQueue(device, ctx);
    }
}

VOID
HyteraIoComplete(
    _In_ WDFREQUEST Request,
    _In_ WDFIOTARGET Target,
    _In_ PWDF_REQUEST_COMPLETION_PARAMS Params,
    _In_ WDFCONTEXT Context
    )
{
    UNREFERENCED_PARAMETER(Target);
    UNREFERENCED_PARAMETER(Context);

    // NOTE (v24): do NOT call WdfRequestMarkCancelable on queue-delivered
    // requests. The framework already registers them for cancellation and
    // automatically cancels the forwarded request at the USB target when
    // the application calls CancelIoEx. Marking them again double-inserts
    // the request into the queue's cancel list -> FxIoQueue::
    // RequestCancelable corrupts the list on removal -> 0x139 BSOD
    // (crash stack: hyterabulk!WdfRequestMarkCancelable -> Wdf01000!
    // FxIoQueue::RequestCancelable -> FAST_FAIL_CORRUPT_LIST_ENTRY).
    if (NT_SUCCESS(Params->IoStatus.Status)) {
        WdfRequestSetInformation(Request, Params->IoStatus.Information);
    }
    WdfRequestComplete(Request, Params->IoStatus.Status);
}

// Completion routine for the driver-created preamble requests. Driver-
// created requests are deleted when the target completes them; calling
// WdfRequestComplete here would double-complete (WDF verifier bugcheck
// "framework object deleted incorrectly", seen with v30).
VOID
HyteraPreambleComplete(
    _In_ WDFREQUEST Request,
    _In_ WDFIOTARGET Target,
    _In_ PWDF_REQUEST_COMPLETION_PARAMS Params,
    _In_ WDFCONTEXT Context
    )
{
    NTSTATUS status = Params->IoStatus.Status;

    UNREFERENCED_PARAMETER(Target);
    UNREFERENCED_PARAMETER(Context);

    // v51: log write completions so a failed/aborted END command is
    // visible (the radio answers END only if it actually received it).
    HyteraDbg(WdfIoTargetGetDevice(Target), "wc", status);

    WdfObjectDelete(Request);
}

// Start (or restart) the background receive pump: a driver-created
// request reading HYTERA_PUMP_SIZE bytes from the bulk IN pipe. The
// pump keeps data flowing into the FIFO regardless of what user mode
// is doing, so user reads can be non-blocking FIFO drains.
static VOID
HyteraStartPump(
    _In_ WDFDEVICE Device
    )
{
    NTSTATUS status;
    DEVICE_CONTEXT* ctx = GetDeviceContext(Device);
    WDFREQUEST request;
    WDF_OBJECT_ATTRIBUTES attrs;

    if (ctx->Removing || ctx->PumpActive ||
        ctx->BulkInPipe == WDF_NO_HANDLE) {
        // v52: log pump re-arm attempts so a stalled pump after the
        // block phase is visible.
        HyteraDbg(Device, "spx",
                  ctx->PumpActive ? STATUS_SUCCESS : STATUS_INVALID_DEVICE_STATE);
        return;
    }

    HyteraDbg(Device, "sp", STATUS_SUCCESS);

    status = WdfRequestCreate(WDF_NO_OBJECT_ATTRIBUTES,
                              WdfUsbTargetPipeGetIoTarget(ctx->BulkInPipe),
                              &request);
    if (!NT_SUCCESS(status)) {
        return;
    }

    status = WdfUsbTargetPipeFormatRequestForRead(ctx->BulkInPipe,
                                                  request,
                                                  ctx->PumpMem,
                                                  NULL);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(request);
        return;
    }

    WdfRequestSetCompletionRoutine(request, HyteraPumpComplete,
                                   (WDFCONTEXT)Device);
    ctx->PumpActive = TRUE;

    if (WdfRequestSend(request,
                       WdfUsbTargetPipeGetIoTarget(ctx->BulkInPipe),
                       WDF_NO_SEND_OPTIONS) == FALSE) {
        // completed synchronously; the completion routine already ran
        // and re-issued the pump
    }
}

VOID
HyteraPumpComplete(
    _In_ WDFREQUEST Request,
    _In_ WDFIOTARGET Target,
    _In_ PWDF_REQUEST_COMPLETION_PARAMS Params,
    _In_ WDFCONTEXT Context
    )
{
    WDFDEVICE device = (WDFDEVICE)Context;
    DEVICE_CONTEXT* ctx = GetDeviceContext(device);
    PVOID buf;
    size_t got;
    ULONG space;
    ULONG nextSend = 0;

    UNREFERENCED_PARAMETER(Target);

    ctx->PumpActive = FALSE;

    // v74: the device is going away. This completion fires while the
    // teardown (SurpriseRemoval / ReleaseHardware / file cleanup) is in
    // progress; touching the FIFO, completing deferred reads, or
    // re-arming the pump here is what produced the 0x2B BSODs. Drop the
    // request and leave all teardown to the PnP path.
    if (ctx->Removing) {
        HyteraDbg(device, "pm-rm", Params->IoStatus.Status);
        WdfObjectDelete(Request);
        return;
    }

    if (NT_SUCCESS(Params->IoStatus.Status)) {
        got = Params->IoStatus.Information;
        buf = WdfMemoryGetBuffer(ctx->PumpMem, NULL);
        if (buf != NULL && got > 0) {
            WdfSpinLockAcquire(ctx->FifoLock);

            // Append to the assembly buffer.
            if (ctx->WrapLen + got <= sizeof(ctx->WrapBuf)) {
                RtlCopyMemory(ctx->WrapBuf + ctx->WrapLen, buf, got);
                ctx->WrapLen += (ULONG)got;
                // v50: log every USB read so we can see whether the
                // radio answers the end command (20B 0x81c6 response).
                HyteraDbg(device, "ub", (NTSTATUS)got);
                if (got >= 16) {
                    UCHAR* rb = (UCHAR*)buf;
                    HyteraDbg(device, "ux0", (NTSTATUS)(rb[0] | (rb[1]<<8) | (rb[2]<<16) | ((ULONG)rb[3]<<24)));
                    HyteraDbg(device, "ux4", (NTSTATUS)(rb[4] | (rb[5]<<8) | (rb[6]<<16) | ((ULONG)rb[7]<<24)));
                    HyteraDbg(device, "ux8", (NTSTATUS)(rb[8] | (rb[9]<<8) | (rb[10]<<16) | ((ULONG)rb[11]<<24)));
                    HyteraDbg(device, "uxc", (NTSTATUS)(rb[12] | (rb[13]<<8) | (rb[14]<<16) | ((ULONG)rb[15]<<24)));
                }
            } else {
                ctx->WrapLen = 0;   // overflow: drop everything
            }

            // Extract complete frames; wrap as MCCI packets.
            while (ctx->WrapLen >= 12) {
                ULONG flen = ((ULONG)ctx->WrapBuf[8] << 8) |
                             ctx->WrapBuf[9];
                BOOLEAN isPreamble = FALSE;

                if (flen < 12 || flen > 8192 || flen > ctx->WrapLen) {
                    break;   // incomplete frame, wait for more
                }
                // v88: any complete radio frame proves the link is alive;
                // cancel the keepalive-answer watchdog.
                ctx->KaAcked = TRUE;

                // v89: kind-20 (hfc_hrcp + hfc_portusb) speaks RAW WIRE
                // frames on the device interface: the connect wait reads
                // 12B raw (checks 7e..fd) and the post-connect reader
                // parses the 12B header + BE length. Append verbatim -
                // no [L][chk] wrap, no keepalive drop.
                if (ctx->AppDriven && flen <= 12) {
                    // v93: at most one 12B frame per session.
                    if (ctx->KaDelivered) {
                        HyteraDbg(device, "drop12", (NTSTATUS)flen);
                        RtlMoveMemory(ctx->WrapBuf, ctx->WrapBuf + flen,
                                      ctx->WrapLen - flen);
                        ctx->WrapLen -= flen;
                        continue;
                    }
                    ctx->KaDelivered = TRUE;
                }
                if (ctx->AppDriven && flen > 12) {
                    ctx->SawBigFrame = TRUE;
                }
                if (ctx->AppDriven) {
                    // v97: deliver immediately. v90's ~100 ms staging delay
                    // was a workaround for a suspected arm/send race; the
                    // stalls that motivated it later turned out to be
                    // debug-tooling interference, and the port stack always
                    // arms its response wait before handing a frame to the
                    // transport, so a fast answer cannot miss the waiter.
                    // Staging cost ~112 ms of every ~133 ms block cycle
                    // (~17 s per 126-block sweep vs ~25 ms/cycle at cable
                    // speed). The stage buffer stays as an overflow fallback.
                    if (HYTERA_FIFO_SIZE - ctx->FifoLen >= flen) {
                        RtlCopyMemory(ctx->Fifo + ctx->FifoLen,
                                      ctx->WrapBuf, flen);
                        ctx->FifoLen += flen;
                        HyteraDbg(device, "dir", (NTSTATUS)flen);
                    } else if (sizeof(ctx->Stage) - ctx->StageLen >= flen) {
                        RtlCopyMemory(ctx->Stage + ctx->StageLen,
                                      ctx->WrapBuf, flen);
                        ctx->StageLen += flen;
                        WdfTimerStart(ctx->BlkTimer,
                                      WDF_REL_TIMEOUT_IN_MS(5));
                        HyteraDbg(device, "stg", (NTSTATUS)flen);
                    }
                    RtlMoveMemory(ctx->WrapBuf, ctx->WrapBuf + flen,
                                  ctx->WrapLen - flen);
                    ctx->WrapLen -= flen;
                    continue;
                }

                // No session active: drop anything the radio still has
                // in flight from an aborted run so the next CPS attempt
                // starts clean.
                if (!ctx->SessionStarted) {
                    HyteraDbg(device, "nx", (NTSTATUS)flen);
                    RtlMoveMemory(ctx->WrapBuf, ctx->WrapBuf + flen,
                                  ctx->WrapLen - flen);
                    ctx->WrapLen -= flen;
                    continue;
                }

                // Discard only the 12B keepalive responses from the
                // preamble. CPS chokes on keepalive packets but needs the
                // model-query response (59B) FIRST in its machine-info
                // stream: [model][serial][info][init6][exchange].
                if (ctx->InitStep == 1) {
                    if (flen == 12 && ctx->WrapBuf[1] == 0x04 &&
                        ctx->WrapBuf[3] == 0xfd) {
                        isPreamble = TRUE;   // keepalive response
                    }
                }
                // v49: keep the 12B keepalive responses in the stream.
                // The reference capture shows the radio's preamble as
                // [keepalive][model][keepalive][serial][info][init6]
                // [exchange] and the real driver feeds CPS everything;
                // dropping keepalives may break CPS's state machine.
                if (isPreamble) {
                    HyteraDbg(device, "ka", (NTSTATUS)flen);
                }

                // Init sequence state machine: match the response's HRCP
                // ax to the current step and advance. Step 5 extends it
                // into the block-read phase: every 0x81c7 response queues
                // the next block command.
                if (ctx->InitStep >= 1 && ctx->InitStep <= 5 &&
                    flen >= 14 && ctx->WrapBuf[12] == 0x02) {
                    USHORT rax = (USHORT)ctx->WrapBuf[13] |
                                 ((USHORT)ctx->WrapBuf[14] << 8);
                    BOOLEAN match = FALSE;
                    if (ctx->InitStep == 1 && rax == 0x8203) match = TRUE;
                    if (ctx->InitStep == 2 && rax == 0x81c5) match = TRUE;
                    if (ctx->InitStep == 3 && rax == 0x8201) match = TRUE;
                    if (ctx->InitStep == 4 &&
                        (rax == 0xf001 || rax == 0x81d3)) match = TRUE;
                    if (ctx->InitStep == 5 && rax == 0x81c7) {
                        HyteraDbg(device, "b81c7", (NTSTATUS)rax);
                    }
                    if (match) {
                        ctx->InitStep++;
                        if (ctx->InitStep == 2) nextSend = 5;
                        else if (ctx->InitStep == 3) nextSend = 6;
                        else if (ctx->InitStep == 4) nextSend = 7;
                        else if (ctx->InitStep == 5) {
                            // Machine-info exchange complete. v78: do NOT
                            // auto-run the block phase - the real CPS
                            // drives its own 0x1c7 block requests.
                        }
                        HyteraDbg(device, "st", (NTSTATUS)ctx->InitStep);
                    }
                }

                // v78: deliver ONLY the HRCP packet to CPS. hfc_hrcp10
                // strips [L][chk] and hands the app everything after it:
                // cmd = u16 at payload+1, data_len = payload_len - 7. The
                // 12-byte [7e 04 00 xx][10 20 00 yy][len BE][px] header is
                // the official driver's private USB framing - CPS must
                // never see it. Shipping the whole frame made CPS read
                // cmd 0x0004 (from "7e 04") instead of 0x8203, so its
                // SendRecv check failed at the first sub-request and the
                // app raised S0107403 (读取机器信息失败).
                // Packet = [u16 L = (flen-12)+4 LE][u16 chk][hrcp...];
                // chk solves sum(0xFFFF - LE16 word) == 0xFFFF over all
                // L bytes (odd tail byte counted as byte<<8).
                if (flen < 12 + 7) {
                    // Header-only frames (12B keepalives) carry no HRCP.
                    HyteraDbg(device, "hshort", (NTSTATUS)flen);
                    // v87: the 20-class app waits for the radio's keepalive
                    // answer before it declares the connection up. Wrap the
                    // whole short frame as a valid HRNP packet
                    // [u16 L = flen+4][u16 chk][frame] so the app's
                    // CheckHrnpPackage passes and its connect wait sees it.
                    if (ctx->AppDriven && flen >= 12 &&
                        HYTERA_FIFO_SIZE - ctx->FifoLen >= flen + 4) {
                        ULONG L3 = flen + 4;
                        ULONG s3 = 0xFFFF - (L3 & 0xffff);
                        ULONG j3;
                        for (j3 = 0; j3 + 1 < flen; j3 += 2) {
                            ULONG w3 = (ULONG)ctx->WrapBuf[j3] |
                                       ((ULONG)ctx->WrapBuf[j3 + 1] << 8);
                            s3 += 0xFFFF - w3;
                        }
                        s3 &= 0xffff;
                        ctx->Fifo[ctx->FifoLen] = (UCHAR)(L3 & 0xff);
                        ctx->Fifo[ctx->FifoLen + 1] = (UCHAR)(L3 >> 8);
                        ctx->Fifo[ctx->FifoLen + 2] = (UCHAR)(s3 & 0xff);
                        ctx->Fifo[ctx->FifoLen + 3] = (UCHAR)(s3 >> 8);
                        RtlCopyMemory(ctx->Fifo + ctx->FifoLen + 4,
                                      ctx->WrapBuf, flen);
                        ctx->FifoLen += L3;
                        HyteraDbg(device, "kw", (NTSTATUS)L3);
                    }
                    RtlMoveMemory(ctx->WrapBuf, ctx->WrapBuf + flen,
                                  ctx->WrapLen - flen);
                    ctx->WrapLen -= flen;
                    continue;
                }
                space = HYTERA_FIFO_SIZE - ctx->FifoLen;
                {
                    ULONG hlen = flen - 12;
                    ULONG L2 = hlen + 4;
                    ULONG s2 = 0xFFFF - (L2 & 0xffff);
                    ULONG j2;
                    UCHAR c1, c2;

                    // v81: ALWAYS log the 0x81c6 answer bytes (diagnostic),
                    // then normalize the data byte to 00 regardless of its
                    // original value.
                    // The radio replies to 0x1c6(data=01) with 0x81c6
                    // carrying data byte 0x01 ("now in mode 1"); the app
                    // hard-requires 0x00 (wrapper 0x577791 checks
                    // buf[0]==0, else 进入编程模式失败). The reference
                    // session's answer carried 0x00 with HRCP checksum
                    // 0xEA. Rewrite data+checksum so the app's check
                    // passes; the read itself needs no radio-side mode
                    // change (the 22:35 forged session served all 64
                    // blocks with the radio in its default state).
                    if (hlen >= 8 &&
                        ctx->WrapBuf[12] == 0x02 &&
                        ctx->WrapBuf[13] == 0xc6 &&
                        ctx->WrapBuf[14] == 0x81) {
                        ULONG orig = ((ULONG)ctx->WrapBuf[15] << 24) |
                                     ((ULONG)ctx->WrapBuf[16] << 16) |
                                     ((ULONG)ctx->WrapBuf[17] << 8) |
                                     (ULONG)ctx->WrapBuf[18];
                        HyteraDbg(device, "g81c6", (NTSTATUS)orig);
                        if (ctx->WrapBuf[15] == 0x01 &&
                            ctx->WrapBuf[16] == 0x00 &&
                            ctx->WrapBuf[17] == 0x01 &&
                            ctx->WrapBuf[19] == 0x03) {
                            // v82: the radio answers the mode-enter request
                            // (0x1c6 data=01) with 0x81c6 data=01. The golden
                            // session shows the app instead receives 0xf001
                            // (dlen=0) at this step - the official MCCI stack
                            // answers the mode-enter itself without the radio.
                            // Substitute the exact golden reply.
                            ctx->WrapBuf[12] = 0x02;
                            ctx->WrapBuf[13] = 0x01;
                            ctx->WrapBuf[14] = 0xf0;
                            ctx->WrapBuf[15] = 0x00;
                            ctx->WrapBuf[16] = 0x00;
                            ctx->WrapBuf[17] = 0x41;
                            ctx->WrapBuf[18] = 0x03;
                            hlen = 7;
                            L2 = hlen + 4;
                            s2 = 0xFFFF - (L2 & 0xffff);
                            HyteraDbg(device, "f001x", (NTSTATUS)hlen);
                        } else if (ctx->WrapBuf[15] == 0x01 &&
                            ctx->WrapBuf[16] == 0x00 &&
                            ctx->WrapBuf[17] != 0x00 &&
                            ctx->WrapBuf[19] == 0x03) {
                            ULONG s8 = 0;
                            ULONG k8;
                            ctx->WrapBuf[17] = 0x00;
                            for (k8 = 1; k8 <= 5; k8++) {
                                s8 += ctx->WrapBuf[12 + k8];
                            }
                            ctx->WrapBuf[18] = (UCHAR)(((~s8) + 0x33) & 0xff);
                            HyteraDbg(device, "nz81c6", (NTSTATUS)hlen);
                        }
                    }

                    // v84: log the first 8 payload bytes of the 0x81c5
                    // response (the app hard-checks data[0]!=1, data[1]==arg,
                    // data[2:4]==0x00b9 -> else 1522/进入编程模式失败).
                    if (hlen >= 13 &&
                        ctx->WrapBuf[12] == 0x02 &&
                        ctx->WrapBuf[13] == 0xc5 &&
                        ctx->WrapBuf[14] == 0x81) {
                        ULONG q0 = ((ULONG)ctx->WrapBuf[17] << 24) |
                                   ((ULONG)ctx->WrapBuf[18] << 16) |
                                   ((ULONG)ctx->WrapBuf[19] << 8) |
                                   (ULONG)ctx->WrapBuf[20];
                        ULONG q1 = ((ULONG)ctx->WrapBuf[21] << 24) |
                                   ((ULONG)ctx->WrapBuf[22] << 16) |
                                   ((ULONG)ctx->WrapBuf[23] << 8) |
                                   (ULONG)ctx->WrapBuf[24];
                        HyteraDbg(device, "e81c5", (NTSTATUS)q0);
                        HyteraDbg(device, "f81c5", (NTSTATUS)q1);
                    }

                    for (j2 = 0; j2 + 1 < hlen; j2 += 2) {
                        ULONG w2 = (ULONG)ctx->WrapBuf[12 + j2] |
                                   ((ULONG)ctx->WrapBuf[12 + j2 + 1] << 8);
                        s2 += 0xFFFF - w2;
                    }
                    if (hlen & 1) {
                        s2 += 0xFFFF -
                              (((ULONG)ctx->WrapBuf[12 + hlen - 1]) << 8);
                    }
                    s2 &= 0xffff;
                    c1 = (UCHAR)(s2 & 0xff);   /* chk word = partial sum */
                    c2 = (UCHAR)(s2 >> 8);

                    if (space >= L2) {
                        ctx->Fifo[ctx->FifoLen] = (UCHAR)(L2 & 0xff);
                        ctx->Fifo[ctx->FifoLen + 1] = (UCHAR)(L2 >> 8);
                        ctx->Fifo[ctx->FifoLen + 2] = c1;
                        ctx->Fifo[ctx->FifoLen + 3] = c2;
                        RtlCopyMemory(ctx->Fifo + ctx->FifoLen + 4,
                                      ctx->WrapBuf + 12, hlen);
                        ctx->FifoLen += L2;
                    } else {
                        // v48: dropping a frame silently desynchronizes
                        // the stream CPS is parsing; make it visible.
                        HyteraDbg(device, "drop", (NTSTATUS)flen);
                    }
                    RtlMoveMemory(ctx->WrapBuf, ctx->WrapBuf + flen,
                                  ctx->WrapLen - flen);
                    ctx->WrapLen -= flen;
                    HyteraDbg(device, "pw", (NTSTATUS)L2);
                }
            }

            WdfSpinLockRelease(ctx->FifoLock);
        }
    }

    WdfObjectDelete(Request);

    // v41: serve a deferred read with the data just wrapped.
    HyteraCompletePendingRead(device, ctx);

    // v78: no driver-generated requests. The whole session (0x205 model
    // query, 0x1c5 device info, 0x201, 0x1c7 block reads, 0x1c6 end) is
    // sent by CPS itself through the write path; forging it here
    // preempted the app's own request/response matching. Kept as a log
    // so the matched response codes stay visible in the trace.
    if (nextSend != 0) {
        HyteraDbg(device, "nsw", (NTSTATUS)nextSend);
    }

    // Keep the pipe drained unless the device is going away.
    HyteraStartPump(device);
}

static BOOLEAN
HyteraRequestIsOnPipe(
    _In_ WDFREQUEST Request,
    _In_ PCWSTR PipeName
    )
{
    PUNICODE_STRING name;
    WDFFILEOBJECT fo = WdfRequestGetFileObject(Request);

    if (fo == WDF_NO_HANDLE) {
        return FALSE;
    }
    name = WdfFileObjectGetFileName(fo);
    if (name == NULL || name->Buffer == NULL) {
        return FALSE;
    }
    return wcsstr(name->Buffer, PipeName) != NULL;
}

VOID
HyteraEvtRead(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t Length
    )
{
    NTSTATUS status;
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    DEVICE_CONTEXT* ctx = GetDeviceContext(device);
    PVOID buf;
    size_t bufLen;
    ULONG copy;

    UNREFERENCED_PARAMETER(Length);

    // Application reads drain the FIFO (which holds MCCI-wrapped
    // packets: [wLen 2B LE][frame]). Empty FIFO -> 0 bytes immediately.
    // v85: requests on the bkrwbk device interface (20-class stack) carry
    // no pipe name; they read the same FIFO. Only a read on the explicit
    // write pipe (pipe01) is bogus.
    if (!HyteraRequestIsOnPipe(Request, L"pipe00") &&
        HyteraRequestIsOnPipe(Request, L"pipe01")) {
        HyteraDbg(device, "rd-route", STATUS_INVALID_DEVICE_REQUEST);
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }

    // v74: during surprise removal / release the pending list is no
    // longer serviced; fail fast instead of piling up dead requests.
    if (ctx->Removing) {
        WdfRequestComplete(Request, STATUS_DEVICE_NOT_CONNECTED);
        return;
    }

    // Sparse logging during the block phase: CPS polls with 851-byte
    // reads and the log would drown in empty polls.
    if (ctx->InitStep < 5 || ctx->FifoLen > 0) {
        HyteraDbg(device, "rd", (NTSTATUS)Length);
    }

    status = WdfRequestRetrieveOutputBuffer(Request, 0, &buf, &bufLen);
    if (!NT_SUCCESS(status)) {
        HyteraDbg(device, "rd-buf", status);
        WdfRequestComplete(Request, status);
        return;
    }

    copy = 0;
    WdfSpinLockAcquire(ctx->FifoLock);
    if (ctx->FifoLen > 0 && bufLen > 0) {
        copy = (ULONG)(ctx->FifoLen < bufLen ? ctx->FifoLen : bufLen);
        if (ctx->AppDriven) {
            ULONG fl = HyteraFirstFrameLen(ctx);
            if (copy > fl) {
                copy = fl;   // v92: one whole frame per read
            }
        }
        RtlCopyMemory(buf, ctx->Fifo, copy);
        RtlMoveMemory(ctx->Fifo, ctx->Fifo + copy, ctx->FifoLen - copy);
        ctx->FifoLen -= copy;
    }
    WdfSpinLockRelease(ctx->FifoLock);

    // v55: empty FIFO -> defer the read on the pending queue. The real
    // MCCI driver never returns 0 bytes on an empty inbox (reads block
    // until data); CPS's parser counts empty reads and aborts the
    // session when it sees too many (that is what killed v46-v54 runs:
    // the death point moved earlier exactly as pacing made more empty
    // reads).
    if (copy > 0) {
        if (ctx->InitStep < 5) {
            HyteraDbg(device, "rc", (NTSTATUS)copy);
        }
        WdfRequestSetInformation(Request, copy);

        // If the block phase is stalled on FIFO space, this read just
        // made room: send the queued block command.
        if (ctx->BlkPending && ctx->BlkQueue > 0) {
            HyteraFlushBlockQueue(device, ctx);
        }

        WdfRequestComplete(Request, STATUS_SUCCESS);
        return;
    }

    // v88: the 20-class app blocks its first read waiting for the radio's
    // keepalive answer. The radio only answers the first keepalive after
    // a long idle (observed: answers once, then ignores while the aborted
    // session stays open on its side). If the real answer has not arrived
    // by the time the app reads, inject the golden keepalive answer as a
    // valid HRNP packet so the app's connect wait completes and it drives
    // the session (init2 -> 0x205 query) which the radio does answer.
    if (ctx->AppDriven && ctx->KaWait && !ctx->KaAcked && !ctx->KaInjected) {
        WdfSpinLockAcquire(ctx->FifoLock);
        if (HYTERA_FIFO_SIZE - ctx->FifoLen >= sizeof(HyteraKaResp)) {
            // v89: kind-20 reads raw bytes - inject the golden keepalive
            // answer frame verbatim (12B, checks 7e..fd on the app side).
            RtlCopyMemory(ctx->Fifo + ctx->FifoLen, HyteraKaResp,
                          sizeof(HyteraKaResp));
            ctx->FifoLen += sizeof(HyteraKaResp);
            ctx->KaInjected = TRUE;
            ctx->KaDelivered = TRUE;
            HyteraDbg(device, "kai", (NTSTATUS)sizeof(HyteraKaResp));
        }
        WdfSpinLockRelease(ctx->FifoLock);
    }

    if (ctx->InitStep < 5) {
        HyteraDbg(device, "rd-pend", STATUS_PENDING);
    }
    WdfSpinLockAcquire(ctx->FifoLock);
    if (ctx->FifoLen > 0 && ctx->PendCount == 0) {
        // Data raced in: serve it now instead of pending.
        copy = (ULONG)(ctx->FifoLen < bufLen ? ctx->FifoLen : bufLen);
        if (ctx->AppDriven) {
            ULONG fl = HyteraFirstFrameLen(ctx);
            if (copy > fl) {
                copy = fl;
            }
        }
        RtlCopyMemory(buf, ctx->Fifo, copy);
        RtlMoveMemory(ctx->Fifo, ctx->Fifo + copy, ctx->FifoLen - copy);
        ctx->FifoLen -= copy;
        WdfSpinLockRelease(ctx->FifoLock);
        WdfRequestSetInformation(Request, copy);
        WdfRequestComplete(Request, STATUS_SUCCESS);
        return;
    }
    if (ctx->PendCount < 8) {
        ctx->PendReq[ctx->PendCount] = Request;
        ctx->PendBuf[ctx->PendCount] = buf;
        ctx->PendLen[ctx->PendCount] = bufLen;
        ctx->PendMarked[ctx->PendCount] = FALSE;
        ctx->PendCount++;
        WdfSpinLockRelease(ctx->FifoLock);
        // v71: mark AFTER inserting. If the request was already
        // cancelled, the framework calls the callback synchronously,
        // which removes it from the list and completes it.
        WdfRequestMarkCancelable(Request, HyteraPendCancel);
        // v74: record that the request is cancelable, looked up by
        // pointer (a synchronous cancel callback may have removed the
        // slot and shifted the list). Then re-check the FIFO: data may
        // have arrived while we were marking, and the serve path skips
        // unmarked slots, so this call is what gets us served.
        {
            ULONG i;
            WdfSpinLockAcquire(ctx->FifoLock);
            for (i = 0; i < ctx->PendCount; i++) {
                if (ctx->PendReq[i] == Request) {
                    ctx->PendMarked[i] = TRUE;
                    break;
                }
            }
            WdfSpinLockRelease(ctx->FifoLock);
        }
        HyteraCompletePendingRead(device, ctx);
        return;   // otherwise completed later by HyteraCompletePendingRead
    }
    WdfSpinLockRelease(ctx->FifoLock);
    // List full (cannot happen with CPS's two readers); fail fast.
    WdfRequestComplete(Request, STATUS_INSUFFICIENT_RESOURCES);
}

VOID
HyteraEvtWrite(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t Length
    )
{
    NTSTATUS status;
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    DEVICE_CONTEXT* ctx = GetDeviceContext(device);
    PVOID buf;
    size_t bufLen;
    PUCHAR p;
    ULONG wCmd;
    USHORT ax;
    ULONG frameLen;
    PUCHAR frame;
    WDFMEMORY mem;
    ULONG i;

    UNREFERENCED_PARAMETER(Length);

    // v85: interface-opened handles (20-class stack) write the same
    // OUT channel; only a write on the explicit read pipe (pipe00) is bogus.
    if (ctx->BulkOutPipe == WDF_NO_HANDLE ||
        (HyteraRequestIsOnPipe(Request, L"pipe00") &&
         !HyteraRequestIsOnPipe(Request, L"pipe01"))) {
        HyteraDbg(device, "wr-route", STATUS_INVALID_DEVICE_REQUEST);
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }

    HyteraDbg(device, "wr", (NTSTATUS)Length);

    status = WdfRequestRetrieveInputBuffer(Request, 0, &buf, &bufLen);
    if (!NT_SUCCESS(status)) {
        HyteraDbg(device, "wr-buf", status);
        WdfRequestComplete(Request, status);
        return;
    }

    // Log the payload content (up to 12 bytes, 4 per entry).
    if (bufLen >= 4) {
        ULONG h = ((ULONG)((PUCHAR)buf)[0] << 24) |
                  ((ULONG)((PUCHAR)buf)[1] << 16) |
                  ((ULONG)((PUCHAR)buf)[2] << 8) |
                  ((ULONG)((PUCHAR)buf)[3]);
        HyteraDbg(device, "wd", (NTSTATUS)h);
    }
    if (bufLen >= 8) {
        ULONG h = ((ULONG)((PUCHAR)buf)[4] << 24) |
                  ((ULONG)((PUCHAR)buf)[5] << 16) |
                  ((ULONG)((PUCHAR)buf)[6] << 8) |
                  ((ULONG)((PUCHAR)buf)[7]);
        HyteraDbg(device, "we", (NTSTATUS)h);
    }
    if (bufLen >= 12) {
        ULONG h = ((ULONG)((PUCHAR)buf)[8] << 24) |
                  ((ULONG)((PUCHAR)buf)[9] << 16) |
                  ((ULONG)((PUCHAR)buf)[10] << 8) |
                  ((ULONG)((PUCHAR)buf)[11]);
        HyteraDbg(device, "wf", (NTSTATUS)h);
    }

    // v86: the 20-class stack writes fully-formed wire frames itself
    // (keepalives 7e 00/01/04 xx 20 10 ...). They arrive on the device
    // interface handle (no pipe name). Pass them verbatim to the bulk
    // OUT pipe - no [L][chk] parsing, no re-framing, no preamble: the
    // app owns the session from here on.
    if (bufLen >= 12 && ((PUCHAR)buf)[0] == 0x7e &&
        ((PUCHAR)buf)[4] == 0x20 && ((PUCHAR)buf)[5] == 0x10) {
        WDFMEMORY rmem;
        NTSTATUS rstatus;

        ctx->SessionStarted = TRUE;
        ctx->AppDriven = TRUE;
        if (((PUCHAR)buf)[1] == 0x00 && ((PUCHAR)buf)[3] == 0xfe) {
            // Host keepalive (7e 00 00 fe ...): arm the answer watchdog.
            ctx->KaWait = TRUE;
            ctx->KaAcked = FALSE;
            ctx->KaInjected = FALSE;
        }
        rstatus = WdfMemoryCreate(WDF_NO_OBJECT_ATTRIBUTES, NonPagedPool, 0,
                                  bufLen, &rmem, NULL);
        if (!NT_SUCCESS(rstatus)) {
            WdfRequestComplete(Request, rstatus);
            return;
        }
        RtlCopyMemory(WdfMemoryGetBuffer(rmem, NULL), buf, bufLen);
        HyteraDbg(device, "rw", (NTSTATUS)bufLen);
        rstatus = WdfUsbTargetPipeFormatRequestForWrite(ctx->BulkOutPipe,
                                                        Request, rmem, NULL);
        if (!NT_SUCCESS(rstatus)) {
            WdfObjectDelete(rmem);
            WdfRequestComplete(Request, rstatus);
            return;
        }
        WdfRequestSetCompletionRoutine(Request, HyteraIoComplete,
                                       (WDFCONTEXT)ctx);
        WdfRequestSend(Request, WdfUsbTargetPipeGetIoTarget(ctx->BulkOutPipe),
                       WDF_NO_SEND_OPTIONS);
        // v95: deterministic connect fallback. Deliver the golden keepalive
        // answer right after the keepalive goes out - the app may pend its
        // 12-byte read before the radio answers (or before it answers at
        // all). If the real answer also arrives, the one-12B-per-session
        // rule drops it.
        if (ctx->KaWait && !ctx->KaInjected) {
            WdfSpinLockAcquire(ctx->FifoLock);
            if (HYTERA_FIFO_SIZE - ctx->FifoLen >= sizeof(HyteraKaResp)) {
                RtlCopyMemory(ctx->Fifo + ctx->FifoLen, HyteraKaResp,
                              sizeof(HyteraKaResp));
                ctx->FifoLen += sizeof(HyteraKaResp);
                ctx->KaInjected = TRUE;
                ctx->KaDelivered = TRUE;
                HyteraDbg(device, "kai2", (NTSTATUS)sizeof(HyteraKaResp));
            }
            WdfSpinLockRelease(ctx->FifoLock);
        }
        HyteraCompletePendingRead(device, ctx);
        return;
    }

    //
    // MCCI command translation. CPS writes [wLen 2B LE][wCmd 2B][payload]
    // to pipe01; the payload is the Hytera HRCP packet. The real bkrwbus
    // driver wraps it into a Hytera frame (12-byte header + HRCP) and
    // sends it to the radio; responses flow back through pipe00 wrapped
    // as MCCI packets.
    //
    if (bufLen < 6) {
        HyteraDbg(device, "wr-short", STATUS_INVALID_PARAMETER);
        WdfRequestComplete(Request, STATUS_SUCCESS);
        return;
    }

    p = (PUCHAR)buf;
    wCmd = p[2] | (p[3] << 8);
    p += 4;
    bufLen -= 4;

    if (bufLen < 3 || p[0] != 0x02) {
        HyteraDbg(device, "wr-cmd", (NTSTATUS)wCmd);
        WdfRequestComplete(Request, STATUS_SUCCESS);
        return;
    }

    ax = p[1] | (p[2] << 8);

    // On the first MCCI command, run the session preamble the real
    // driver performs: keepalive, init2, keepalive. (v58: also sent
    // from EvtFileCreate so CPS's connection probe gets data at once.)
    // v86: skipped when the 20-class app drives the session itself.
    if (!ctx->SessionStarted && !ctx->AppDriven) {
        ctx->SessionStarted = TRUE;
        HyteraSendPreamble(device, ctx);
    }

    // v79: no prefix table. The frame's word at [10:12] is the frame's own
    // ones'-complement check (see below), so any payload CPS sends gets a
    // correct frame - unknown commands are no longer dropped. The HRCP
    // payload rides verbatim; ax is logged for the trace only.
    HyteraDbg(device, "tk", (NTSTATUS)(ULONG)(KeQueryInterruptTime() / 10000));
    HyteraDbg(device, "ax", (NTSTATUS)ax);

    // Build the Hytera frame: 12-byte header + HRCP payload.
    frameLen = 12 + (ULONG)bufLen;
    status = WdfMemoryCreate(WDF_NO_OBJECT_ATTRIBUTES, NonPagedPool, 0,
                             frameLen, &mem, NULL);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, STATUS_SUCCESS);
        return;
    }
    frame = (PUCHAR)WdfMemoryGetBuffer(mem, NULL);
    frame[0] = 0x7e;
    frame[1] = 0x01;
    frame[2] = 0x00;
    frame[3] = 0x00;
    frame[4] = 0x20;
    frame[5] = 0x10;
    frame[6] = 0x00;
    frame[7] = 0x00;
    frame[8] = (UCHAR)(frameLen >> 8);
    frame[9] = (UCHAR)(frameLen & 0xff);
    frame[10] = 0x00;
    frame[11] = 0x00;
    RtlCopyMemory(frame + 12, p, bufLen);

    // v79: frame[10:12] = ones'-complement check of the frame with the
    // field zeroed: sum big-endian u16 words, fold end-around carries,
    // invert. Verified: this reproduces the captures' values for every
    // known frame (0x60e5/0x33d1/0x31d3/0x02f1/0x41c3/0x08f3/0xf40f are
    // exactly this function of their payloads), and block-read frames get
    // their per-address values from the same formula. The radio silently
    // drops frames whose check is wrong (that is what blocked the 0x1c6
    // enter-programming-mode request: the old table held the data=00
    // frame's value 0xf40f; the data=01 frame needs 0xf50e).
    {
        ULONG s = 0;
        ULONG i2;
        for (i2 = 0; i2 + 1 < frameLen; i2 += 2) {
            s += ((ULONG)frame[i2] << 8) | frame[i2 + 1];
        }
        if (frameLen & 1) {
            s += (ULONG)frame[frameLen - 1] << 8;
        }
        while (s >> 16) {
            s = (s & 0xffff) + (s >> 16);
        }
        s = (~s) & 0xffff;
        frame[10] = (UCHAR)(s >> 8);
        frame[11] = (UCHAR)(s & 0xff);
    }

    status = WdfUsbTargetPipeFormatRequestForWrite(ctx->BulkOutPipe,
                                                   Request, mem, NULL);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(mem);
        WdfRequestComplete(Request, status);
        return;
    }

    // Start the init sequence state machine: init4 is going out now;
    // subsequent responses drive init5/init6/f3d3. Reset the whole
    // session here so a re-run without reopening handles starts clean
    // (the close-reset covers the reopen case).
    if (ax == 0x0203) {
        WdfSpinLockAcquire(ctx->FifoLock);
        ctx->InitStep = 1;
        ctx->BlkIdx = 0;
        ctx->BlkQueue = 0;
        ctx->BlkPending = FALSE;
        ctx->FifoLen = 0;
        ctx->WrapLen = 0;
        WdfSpinLockRelease(ctx->FifoLock);
    }

    WdfRequestSetCompletionRoutine(Request, HyteraIoComplete, (WDFCONTEXT)ctx);
    if (WdfRequestSend(Request,
                       WdfUsbTargetPipeGetIoTarget(ctx->BulkOutPipe),
                       WDF_NO_SEND_OPTIONS)) {
        return;
    }
}

VOID
HyteraEvtFileCreate(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ WDFFILEOBJECT FileObject
    )
{
    PUNICODE_STRING name;
    DEVICE_CONTEXT* ctx = GetDeviceContext(Device);

    InterlockedIncrement((volatile LONG*)&ctx->OpenCount);

    name = WdfFileObjectGetFileName(FileObject);
    if (name != NULL && name->Buffer != NULL) {
        CHAR buf[64];
        int n = 0;
        ULONG i;

        for (i = 0; i < name->Length / sizeof(WCHAR) && n < 60; i++) {
            WCHAR ch = name->Buffer[i];
            if (ch >= 32 && ch < 127) {
                buf[n++] = (CHAR)ch;
            }
        }
        buf[n] = 0;
        HyteraDbg(Device, buf, STATUS_SUCCESS);

        // v58: start the session preamble as soon as CPS opens the read
        // pipe. CPS probes the connection with a 1-byte read right after
        // open; with the real driver the radio's keepalive answers it.
        // v70: inject the canned 12B keepalive response first so the
        // leading [ka][model] order CPS expects is deterministic.
        if (wcsstr(name->Buffer, L"pipe00") != NULL && !ctx->SessionStarted) {
            ctx->SessionStarted = TRUE;
            HyteraInjectFrame(ctx, HyteraKaResp, sizeof(HyteraKaResp));
            HyteraSendPreamble(Device, ctx);
        }
    } else {
        HyteraDbg(Device, "create-noname", STATUS_SUCCESS);
    }

    // With EvtDeviceFileCreate registered, the driver owns the create
    // request: the framework does NOT auto-complete it. Returning without
    // completing leaves CreateFile hanging forever (v16 open hang).
    WdfRequestComplete(Request, STATUS_SUCCESS);
}

// v70: the radio answers keepalives only sporadically, and CPS's
// byte-scan needs the stream to START with the 12B keepalive response
// followed by the 59B model response (golden sequence). The 12B
// response is a fixed frame, so the driver injects it at open time -
// making the leading order deterministic regardless of whether the
// radio deigns to answer its keepalive.
static void
HyteraInjectFrame(
    _In_ DEVICE_CONTEXT* ctx,
    _In_reads_bytes_(flen) const UCHAR* frame,
    _In_ ULONG flen
    )
{
    if (flen < 12 || flen > 512) {
        return;
    }

    WdfSpinLockAcquire(ctx->FifoLock);
    if (HYTERA_FIFO_SIZE - ctx->FifoLen < flen) {
        WdfSpinLockRelease(ctx->FifoLock);
        return;
    }
    // v72: append RAW - the pre-write scan phase consumes raw radio
    // frames, so the injected keepalive response matches that format.
    {
        ULONG i;
        for (i = 0; i < flen; i++) {
            ctx->Fifo[ctx->FifoLen + i] = frame[i];
        }
        ctx->FifoLen += flen;
    }
    WdfSpinLockRelease(ctx->FifoLock);
}

VOID
HyteraEvtFileCleanup(
    _In_ WDFFILEOBJECT FileObject
    )
{
    WDFDEVICE device = WdfFileObjectGetDevice(FileObject);
    DEVICE_CONTEXT* ctx = GetDeviceContext(device);
    LONG remaining;

    remaining = InterlockedDecrement((volatile LONG*)&ctx->OpenCount);
    if (remaining <= 0) {
        // Last handle closed: reset the session so the next CPS run
        // starts with a clean preamble, empty FIFO and no stray radio
        // frames from an aborted codeplug read.
        WdfSpinLockAcquire(ctx->FifoLock);
        ctx->SessionStarted = FALSE;
        ctx->AppDriven = FALSE;
        ctx->StageLen = 0;
        ctx->SawBigFrame = FALSE;
        ctx->KaDelivered = FALSE;
        ctx->KaWait = FALSE;
        ctx->KaAcked = FALSE;
        ctx->KaInjected = FALSE;
        ctx->InitStep = 0;
        ctx->BlkIdx = 0;
        ctx->BlkQueue = 0;
        ctx->BlkPending = FALSE;
        ctx->FifoLen = 0;
        ctx->WrapLen = 0;
        WdfSpinLockRelease(ctx->FifoLock);

        // v68: complete our private deferred reads; the list is
        // driver-owned so this cannot race anything the framework does.
        HyteraCancelPendingReads(ctx, STATUS_CANCELLED);
        HyteraDbg(device, "reset", STATUS_SUCCESS);
    }
}

VOID
HyteraEvtDefault(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request
    )
{
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    HyteraDbg(device, "default", STATUS_UNSUCCESSFUL);
    WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
}

VOID
HyteraEvtControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
    )
{
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    DEVICE_CONTEXT* ctx = GetDeviceContext(device);

    UNREFERENCED_PARAMETER(InputBufferLength);

    HyteraDbg(device, "ctl", (NTSTATUS)IoControlCode);

    // Debug IOCTL: return driver debug log
    // v62: clear the debug log so each CPS attempt starts fresh.
    if (IoControlCode == 0x220001) {
        ctx->DbgIdx = 0;
        ctx->DbgBase.QuadPart = 0;
        WdfRequestSetInformation(Request, 0);
        WdfRequestComplete(Request, STATUS_SUCCESS);
        return;
    }

    if (IoControlCode == 0x220000) {
        PVOID outBuf;
        size_t outLen;
        NTSTATUS status = WdfRequestRetrieveOutputBuffer(Request, 0, &outBuf, &outLen);
        if (!NT_SUCCESS(status)) {
            WdfRequestComplete(Request, status);
            return;
        }
        // Magic marker proves the handler and buffer path work,
        // followed by the debug log contents.
        ULONG copy = (ULONG)ctx->DbgIdx;
        if (outLen >= 16) {
            RtlCopyMemory(outBuf, "HYTERA-MAGIC-v20", 16);
            if (copy > outLen - 16) {
                copy = (ULONG)(outLen - 16);
            }
            if (copy > (ULONG)OutputBufferLength - 16) {
                copy = (ULONG)OutputBufferLength - 16;
            }
            RtlCopyMemory((PUCHAR)outBuf + 16, ctx->DbgLog, copy);
            WdfRequestSetInformation(Request, 16 + (size_t)copy);
            WdfRequestComplete(Request, STATUS_SUCCESS);
            return;
        }
        WdfRequestComplete(Request, STATUS_SUCCESS);
        return;
    }

    // CPS mostly does not use IOCTLs on the control handle; reject anything
    // it sends rather than fabricating success (which can confuse callers).
    WdfRequestComplete(Request, STATUS_NOT_SUPPORTED);
    UNREFERENCED_PARAMETER(OutputBufferLength);
}
