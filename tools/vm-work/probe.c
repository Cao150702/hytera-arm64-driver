#include <windows.h>
#include <stdio.h>

// Ground-truth probe for the real MCCI driver (QEMU): sends CPS's 12-byte
// session command, reads the driver's response stream, saves it + a log.
static void logmsg(const char* m, DWORD v) {
    HANDLE h = CreateFileA("D:\\probe-log.txt", FILE_APPEND_DATA, 0, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        h = CreateFileA("C:\\probe-log.txt", FILE_APPEND_DATA, 0, NULL,
                        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    char line[256];
    wsprintfA(line, "%s %lu\r\n", m, v);
    DWORD w;
    WriteFile(h, line, lstrlenA(line), &w, NULL);
    CloseHandle(h);
}

int main(void) {
    logmsg("probe start", 0);
    HANDLE pin = CreateFileA("\\\\.\\usbbulk\\pipe00", GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                             OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    HANDLE pout = CreateFileA("\\\\.\\usbbulk\\pipe01", GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                              OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (pin == INVALID_HANDLE_VALUE || pout == INVALID_HANDLE_VALUE) {
        logmsg("OPEN FAIL", GetLastError());
        return 1;
    }
    logmsg("opened", 0);

    unsigned char cmd[12] = {0x0c,0x00,0xbf,0xf8,0x02,0x03,0x02,0x01,
                             0x00,0x00,0x2c,0x03};
    DWORD w = 0;
    WriteFile(pout, cmd, 12, &w, NULL);
    logmsg("wrote", w);

    static unsigned char buf[131072];
    DWORD total = 0;
    DWORD lastdata = GetTickCount();
    while (total < 110000 && GetTickCount() - lastdata < 5000) {
        OVERLAPPED ov; memset(&ov, 0, sizeof(ov));
        ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
        DWORD r = 0;
        if (ReadFile(pin, buf + total, 8192, &r, &ov)) {
            total += r;
            if (r > 0) lastdata = GetTickCount();
        } else if (GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(ov.hEvent, 5000) == WAIT_OBJECT_0) {
                GetOverlappedResult(pin, &ov, &r, TRUE);
                total += r;
                if (r > 0) lastdata = GetTickCount();
            }
        }
        CloseHandle(ov.hEvent);
    }
    logmsg("read total", total);

    char hex[256];
    int off = 0;
    for (int i = 0; i < 64 && i < (int)total; i++)
        off += wsprintfA(hex + off, "%02x ", buf[i]);
    hex[off] = 0;
    logmsg(hex, 0);

    HANDLE out = CreateFileA("D:\\probe-out.bin", GENERIC_WRITE, 0, NULL,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE)
        out = CreateFileA("C:\\probe-out.bin", GENERIC_WRITE, 0, NULL,
                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out != INVALID_HANDLE_VALUE) {
        WriteFile(out, buf, total, &w, NULL);
        CloseHandle(out);
        logmsg("saved", total);
    }
    logmsg("done", 0);
    return 0;
}
