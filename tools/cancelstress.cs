
using System;
using System.Runtime.InteropServices;
using System.IO;
using System.Threading;

public class CancelStress {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern IntPtr CreateFileW(string f, uint a, uint s, IntPtr sa, uint c, uint fl, IntPtr t);
    [DllImport("kernel32.dll", EntryPoint="WriteFile", SetLastError=true)]
    static extern bool WriteFileOv(IntPtr h, byte[] b, uint n, out uint w, ref OVERLAPPED o);
    [DllImport("kernel32.dll", EntryPoint="ReadFile", SetLastError=true)]
    static extern bool ReadFileOv(IntPtr h, byte[] b, uint n, out uint r, ref OVERLAPPED o);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern uint WaitForSingleObject(IntPtr h, uint ms);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool CancelIoEx(IntPtr h, IntPtr o);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool GetOverlappedResult(IntPtr h, ref OVERLAPPED o, out uint r, bool wait);
    [DllImport("kernel32.dll")]
    static extern IntPtr CreateEventW(IntPtr a, bool m, bool i, string n);
    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr h);

    [StructLayout(LayoutKind.Sequential)]
    struct OVERLAPPED { public UIntPtr i1, i2; public int o1, o2; public IntPtr ev; }

    static void Log(string m) {
        try { File.AppendAllText(@"C:\hp780\cancel-prog.txt", m + "\r\n"); } catch {}
        Console.WriteLine(m);
    }

    static byte[] Hex(string s) {
        byte[] b = new byte[s.Length / 2];
        for (int i = 0; i < b.Length; i++) b[i] = Convert.ToByte(s.Substring(i * 2, 2), 16);
        return b;
    }

    static int iter = 0, done = 0;

    static void Worker(IntPtr pin, IntPtr pout) {
        try {
            for (int k = 0; k < 30; k++) {
                byte[] f = Hex("7e0000fe20100000000c60e5");
                uint w = 0;
                OVERLAPPED ovw = new OVERLAPPED();
                ovw.ev = CreateEventW(IntPtr.Zero, false, false, null);
                if (!WriteFileOv(pout, f, (uint)f.Length, out w, ref ovw)) {
                    if (Marshal.GetLastWin32Error() == 997) {
                        uint wr = WaitForSingleObject(ovw.ev, 1500);
                        if (wr == 258) { CancelIoEx(pout, IntPtr.Zero); GetOverlappedResult(pout, ref ovw, out w, true); }
                    }
                }
                byte[] buf = new byte[1024];
                uint r = 0;
                OVERLAPPED ovr = new OVERLAPPED();
                ovr.ev = CreateEventW(IntPtr.Zero, false, false, null);
                if (!ReadFileOv(pin, buf, 1024, out r, ref ovr)) {
                    if (Marshal.GetLastWin32Error() == 997) {
                        uint wr = WaitForSingleObject(ovr.ev, 1500);
                        if (wr == 258) { CancelIoEx(pin, IntPtr.Zero); GetOverlappedResult(pin, ref ovr, out r, true); }
                    }
                }
                Interlocked.Increment(ref iter);
            }
        } catch { }
        Interlocked.Increment(ref done);
    }

    public static void Main() {
        try { File.Delete(@"C:\hp780\cancel-prog.txt"); } catch {}
        Log("C1 opening");
        IntPtr pin = CreateFileW(@"\\.\usbbulk\pipe00", 0x80000000, 3, IntPtr.Zero, 3, 0x40000000, IntPtr.Zero);
        IntPtr pout = CreateFileW(@"\\.\usbbulk\pipe01", 0x40000000, 3, IntPtr.Zero, 3, 0x40000000, IntPtr.Zero);
        Log("C2 pin=" + pin + " pout=" + pout);
        Thread[] ts = new Thread[2];
        for (int i = 0; i < ts.Length; i++) { ts[i] = new Thread(() => Worker(pin, pout)); ts[i].Start(); }
        for (int i = 0; i < ts.Length; i++) ts[i].Join();
        Log("C3 done iter=" + iter + " threads=" + done);
        CloseHandle(pin); CloseHandle(pout);
        Log("C4 SURVIVED");
    }
}
