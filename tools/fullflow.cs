
using System;
using System.Runtime.InteropServices;
using System.IO;

public class FullFlow {
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
        try { File.AppendAllText(@"C:\hp780\fullflow-prog.txt", m + "\r\n"); } catch {}
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
        try { File.Delete(@"C:\hp780\fullflow-prog.txt"); } catch {}
        Log("FF1 open");
        pin = CreateFileW(@"\\.\usbbulk\pipe00", 0x80000000, 3, IntPtr.Zero, 3, 0x40000000, IntPtr.Zero);
        pout = CreateFileW(@"\\.\usbbulk\pipe01", 0x40000000, 3, IntPtr.Zero, 3, 0x40000000, IntPtr.Zero);
        Log("FF2 pin=" + pin + " pout=" + pout);

        string[] pre = {
            "7e0000fe20100000000c60e5",
            "7e01000020100000001433d10205020100002a03",
            "7e0000fe20100000000c60e5",
            "7e01000020100000001431d30203020100002c03",
            "7e01000020100000002402f102c501110000000000000000000000000000000000005b03",
            "7e01000020100000001441c30201020100121c03",
        };
        byte[] buf = new byte[2048];
        for (int i = 0; i < pre.Length; i++) {
            WriteFrame(pre[i]);
            uint r = ReadTimed(buf, 2048, 5000);
            Log("FF3 pre" + (i+1) + " read=" + r);
        }
        Log("FF4 security exchange f3d3");
        WriteFrame("7e01000020100000001c08f302d30109000000000000000000000005503");
        uint r1 = ReadTimed(buf, 2048, 8000);
        Log("FF5 sec read1=" + r1);
        if (r1 == 0) {
            uint r2 = ReadTimed(buf, 2048, 8000);
            Log("FF6 sec supplemental read=" + r2 + " first=" + (r2>0 ? buf[0].ToString("X2") : "-"));
            if (r2 > 0) Log("FF7 resp: " + BitConverter.ToString(buf, 0, (int)Math.Min(r2, 40)));
        } else {
            Log("FF6 first=" + buf[0].ToString("X2"));
            Log("FF7 resp: " + BitConverter.ToString(buf, 0, (int)Math.Min(r1, 40)));
        }

        string blockFrame = "7e01000020100000001f53a402c7010c0000000001000000000000dc057c03";
        int ok = 0;
        for (int k = 0; k < 64; k++) {
            WriteFrame(blockFrame);
            uint r = ReadTimed(buf, 2048, 8000);
            if (r > 1500) ok++;
            if (k < 2 || k % 16 == 0) Log("FF8 block " + k + " read=" + r);
        }
        Log("FF9 blocks ok=" + ok + "/64");
        CloseHandle(pin); CloseHandle(pout);
        Log("FF10 DONE");
    }
}
