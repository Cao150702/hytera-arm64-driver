
using System;
using System.Runtime.InteropServices;
using System.IO;

public class Flow2 {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern IntPtr CreateFileW(string f, uint a, uint s, IntPtr sa, uint c, uint fl, IntPtr t);
    [DllImport("kernel32.dll", EntryPoint="WriteFile", SetLastError=true)]
    static extern bool WriteFileOv(IntPtr h, byte[] b, uint n, out uint w, ref OVERLAPPED o);
    [DllImport("kernel32.dll", EntryPoint="ReadFile", SetLastError=true)]
    static extern bool ReadFileOv(IntPtr h, byte[] b, uint n, out uint r, ref OVERLAPPED o);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern uint WaitForSingleObject(IntPtr h, uint ms);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool GetOverlappedResult(IntPtr h, ref OVERLAPPED o, out uint r, bool wait);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool CancelIoEx(IntPtr h, IntPtr o);
    [DllImport("kernel32.dll")]
    static extern IntPtr CreateEventW(IntPtr a, bool m, bool i, string n);
    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr h);

    [StructLayout(LayoutKind.Sequential)]
    struct OVERLAPPED { public UIntPtr i1, i2; public int o1, o2; public IntPtr ev; }

    static void Log(string m) {
        try { File.AppendAllText(@"C:\hp780\flow2-prog.txt", m + "\r\n"); } catch {}
        Console.WriteLine(m);
    }

    static byte[] Hex(string s) {
        byte[] b = new byte[s.Length / 2];
        for (int i = 0; i < b.Length; i++) b[i] = Convert.ToByte(s.Substring(i * 2, 2), 16);
        return b;
    }

    static IntPtr pin, pout;

    static uint ReadTimed(byte[] buf, uint n, uint ms) {
        uint r = 0;
        OVERLAPPED ov = new OVERLAPPED();
        ov.ev = CreateEventW(IntPtr.Zero, false, false, null);
        if (!ReadFileOv(pin, buf, n, out r, ref ov)) {
            if (Marshal.GetLastWin32Error() == 997) {
                uint wr = WaitForSingleObject(ov.ev, ms);
                if (wr == 258) { CancelIoEx(pin, IntPtr.Zero); GetOverlappedResult(pin, ref ov, out r, true); return 0; }
                GetOverlappedResult(pin, ref ov, out r, true);
            } else return 0;
        }
        return r;
    }

    static void WriteFrame(string hex) {
        byte[] f = Hex(hex);
        uint w = 0;
        OVERLAPPED ov = new OVERLAPPED();
        ov.ev = CreateEventW(IntPtr.Zero, false, false, null);
        if (!WriteFileOv(pout, f, (uint)f.Length, out w, ref ov)) {
            if (Marshal.GetLastWin32Error() == 997) {
                uint wr = WaitForSingleObject(ov.ev, 3000);
                if (wr == 258) { CancelIoEx(pout, IntPtr.Zero); }
                GetOverlappedResult(pout, ref ov, out w, true);
            }
        }
    }

    public static void Main() {
        try { File.Delete(@"C:\hp780\flow2-prog.txt"); } catch {}
        Log("V1 open");
        pin = CreateFileW(@"\\.\usbbulk\pipe00", 0x80000000, 3, IntPtr.Zero, 3, 0x40000000, IntPtr.Zero);
        pout = CreateFileW(@"\\.\usbbulk\pipe01", 0x40000000, 3, IntPtr.Zero, 3, 0x40000000, IntPtr.Zero);
        byte[] buf = new byte[2048];

        Log("V2 send END frame (0x01C6) to release stuck session");
        WriteFrame("7e0100002010000000f40f02c6010100006a03");
        uint r = ReadTimed(buf, 2048, 5000);
        Log("V3 end resp=" + r);
        if (r > 0) Log("V4 end data: " + BitConverter.ToString(buf, 0, (int)Math.Min(r, 24)));

        Log("V5 keepalive");
        WriteFrame("7e0000fe20100000000c60e5");
        r = ReadTimed(buf, 2048, 5000);
        Log("V6 ka resp=" + r);

        Log("V7 security exchange f3d3, wait 30s");
        WriteFrame("7e01000020100000001c08f302d30109000000000000000000000005503");
        r = ReadTimed(buf, 2048, 30000);
        Log("V8 sec read1=" + r);
        if (r == 0) {
            for (int s = 0; s < 3; s++) {
                r = ReadTimed(buf, 2048, 10000);
                Log("V9 supp" + s + "=" + r + (r>0 ? " first=" + buf[0].ToString("X2") : ""));
                if (r > 0) break;
            }
        }
        if (r > 0) Log("V10 sec resp: " + BitConverter.ToString(buf, 0, (int)Math.Min(r, 40)));

        Log("V11 block reads x4");
        string blockFrame = "7e01000020100000001f53a402c7010c0000000001000000000000dc057c03";
        for (int k = 0; k < 4; k++) {
            WriteFrame(blockFrame);
            r = ReadTimed(buf, 2048, 8000);
            Log("V12 block " + k + " read=" + r);
        }
        CloseHandle(pin); CloseHandle(pout);
        Log("V13 DONE");
    }
}
