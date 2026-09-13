using System;
using System.Runtime.InteropServices;
using System.IO;
using System.Threading;

// port2.cs - v2 of the VM read tool.
// Key change: reads are NEVER canceled. One large pending read is kept
// in flight; polling = waiting on the event with a timeout. The slow
// f3d3 response (4.3s) completes the pending read when it arrives.
// (port.cs v1 canceled the pending read every 300ms via CancelIoEx;
// each cancel aborts the bulk-IN transfer at the virtual USB layer and
// the radio's late response never arrives - the VM f3d3 loss.)
public class Port2 {
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
    [DllImport("kernel32.dll")]
    static extern IntPtr CreateEventW(IntPtr a, bool m, bool i, string n);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);

    [StructLayout(LayoutKind.Sequential)]
    struct OVERLAPPED { public UIntPtr i1, i2; public int o1, o2; public IntPtr ev; }

    static void Log(string m) {
        try { File.AppendAllText(@"C:\hp780\port2-prog.txt", m + "\r\n"); } catch {}
        Console.WriteLine(m);
    }

    static IntPtr pin, pout;
    static byte[] rbuf = new byte[1 << 20];
    static int rlen = 0;

    // the one pending read
    static bool rdPending = false;
    static byte[] rdbuf = new byte[65536];
    static OVERLAPPED rdOv;

    static ushort[] PREFIXES = { 0x53a4, 0x4da9, 0x47af, 0x41b5, 0x3bbb, 0x36c1, 0x30c7, 0x2acd, 0x24d2, 0x1ed8, 0x18de, 0x12e4, 0x0cea, 0x07f0, 0x01f6, 0xfbfa, 0xf600, 0xf006, 0xea0c, 0xe412, 0xde18, 0xd81e, 0xd323, 0xcd29, 0xc72f, 0xc135, 0xbb3b, 0xb541, 0xaf47, 0xa94c, 0xa452, 0x9e58, 0x985e, 0x9264, 0x8c6a, 0x8670, 0x8075, 0x7a7b, 0x7481, 0x6f87, 0x698d, 0x6393, 0x5d99, 0x579e, 0x51a5, 0x4bab, 0x45b1, 0x40b7, 0x3abd, 0x34c3, 0x2ec8, 0x28ce, 0x22d4, 0x1cda, 0x16e0, 0x10e6, 0x0bec, 0x05f1, 0xfff6, 0xf9fc, 0xf402, 0xee08, 0xe80e, 0xe413 };

    static void Wr(byte[] d) {
        uint w = 0;
        OVERLAPPED ov = new OVERLAPPED();
        ov.ev = CreateEventW(IntPtr.Zero, true, false, null);
        if (!WriteFileOv(pout, d, (uint)d.Length, out w, ref ov)) {
            if (Marshal.GetLastWin32Error() == 997) {
                uint wr = WaitForSingleObject(ov.ev, 3000);
                GetOverlappedResult(pout, ref ov, out w, false);
            }
        }
    }

    // Ensure a read is pending, then wait up to t ms for data.
    // Returns true if data arrived (copied into rbuf).
    static bool Poll(uint t) {
        if (!rdPending) {
            uint r = 0;
            rdOv = new OVERLAPPED();
            rdOv.ev = CreateEventW(IntPtr.Zero, true, false, null);
            if (!ReadFileOv(pin, rdbuf, 65536, out r, ref rdOv)) {
                if (Marshal.GetLastWin32Error() == 997) {
                    rdPending = true;
                } else {
                    return false;
                }
            } else {
                // synchronous completion
                if (r > 0) { Array.Copy(rdbuf, 0, rbuf, rlen, (int)r); rlen += (int)r; }
                return r > 0;
            }
        }
        uint wr = WaitForSingleObject(rdOv.ev, t);
        if (wr == 258) {
            return false; // still pending; read stays in flight
        }
        uint got = 0;
        GetOverlappedResult(pin, ref rdOv, out got, false);
        rdPending = false;
        if (got > 0) {
            if (rlen + (int)got > rbuf.Length) { rlen = 0; }
            Array.Copy(rdbuf, 0, rbuf, rlen, (int)got);
            rlen += (int)got;
            return true;
        }
        return false;
    }

    static byte[] GetFrame() {
        int i = Array.IndexOf(rbuf, (byte)0x7E);
        if (i < 0) return null;
        if (rlen - i < 12) return null;
        ushort l = (ushort)((rbuf[i+8] << 8) | rbuf[i+9]);
        if (l < 12 || l > 8192 || rlen - i < l) return null;
        byte[] fr = new byte[l];
        Array.Copy(rbuf, i, fr, 0, l);
        byte[] rest = new byte[rbuf.Length];
        Array.Copy(rbuf, i + l, rest, 0, rlen - i - l);
        Array.Copy(rest, rbuf, rest.Length);
        rlen -= i + l;
        return fr;
    }

    static byte[] ReadResponse(double timeout, int minsize) {
        DateTime t0 = DateTime.Now;
        while ((DateTime.Now - t0).TotalSeconds < timeout) {
            byte[] fr = GetFrame();
            if (fr != null && fr.Length >= 2 && fr[1] == 0x04 && fr.Length >= minsize) return fr;
            Poll(250);
        }
        return GetFrame();
    }

    static byte[] Hrcp(ushort ax, byte ln, byte b, byte[] data) {
        byte[] p = new byte[5 + data.Length];
        p[0] = 0x02;
        p[1] = (byte)(ax & 0xFF);
        p[2] = (byte)(ax >> 8);
        p[3] = ln;
        p[4] = b;
        Array.Copy(data, 0, p, 5, data.Length);
        int s = 0;
        for (int k = 1; k < p.Length; k++) s += p[k];
        byte cks = (byte)(((~s) + 0x33) & 0xFF);
        byte[] outr = new byte[p.Length + 2];
        Array.Copy(p, outr, p.Length);
        outr[p.Length] = cks;
        outr[p.Length + 1] = 0x03;
        return outr;
    }

    static byte[] Frame(byte ftype, ushort seq, ushort prefix, byte[] h) {
        byte[] f = new byte[12 + h.Length];
        f[0] = 0x7E; f[1] = ftype; f[2] = 0; f[3] = 0; f[4] = 0x20; f[5] = 0x10;
        f[6] = (byte)(seq >> 8); f[7] = (byte)(seq & 0xFF);
        ushort total = (ushort)(12 + h.Length);
        f[8] = (byte)(total >> 8); f[9] = (byte)(total & 0xFF);
        f[10] = (byte)(prefix >> 8); f[11] = (byte)(prefix & 0xFF);
        Array.Copy(h, 0, f, 12, h.Length);
        return f;
    }

    static byte[] Hex(string s) {
        byte[] b = new byte[s.Length / 2];
        for (int i = 0; i < b.Length; i++) b[i] = Convert.ToByte(s.Substring(i * 2, 2), 16);
        return b;
    }

    static byte[] ReadBlock(int k, int blockSize, int totalBlocks) {
        int addr = k * blockSize;
        int size = (k == totalBlocks - 1) ? 0x450 : blockSize;
        byte[] data = new byte[12];
        byte[] head = Hex("000000010000");
        Array.Copy(head, data, 6);
        data[6] = (byte)(addr & 0xFF);
        data[7] = (byte)((addr >> 8) & 0xFF);
        data[8] = (byte)((addr >> 16) & 0xFF);
        data[9] = (byte)((addr >> 24) & 0xFF);
        data[10] = (byte)(size & 0xFF);
        data[11] = (byte)(size >> 8);
        byte[] h = Hrcp(0x01C7, 12, 0, data);
        Wr(Frame(0x01, 0, PREFIXES[k], h));
        rlen = 0;
        byte[] r = ReadResponse(8.0, 100);
        if (r == null || r.Length < size + 32) return null;
        return r;
    }

    public static void Main() {
        try { File.Delete(@"C:\hp780\port2-prog.txt"); } catch {}
        Log("P1 open");
        pin = CreateFileW(@"\\.\usbbulk\pipe00", 0x80000000, 3, IntPtr.Zero, 3, 0x40000000, IntPtr.Zero);
        pout = CreateFileW(@"\\.\usbbulk\pipe01", 0x40000000, 3, IntPtr.Zero, 3, 0x40000000, IntPtr.Zero);
        Log("P2 pin=" + pin + " pout=" + pout);

        // drain: read whatever is buffered for 0.8s (no cancels)
        DateTime d0 = DateTime.Now;
        while ((DateTime.Now - d0).TotalSeconds < 0.8) Poll(100);
        rlen = 0;
        Log("P1b drained");
        string[] init = {
            "7e0000fe20100000000c60e5",
            "7e01000020100000001433d10205020100002a03",
            "7e0000fe20100000000c60e5",
            "7e01000020100000001431d30203020100002c03",
            "7e01000020100000002402f102c501110000000000000000000000000000000000005b03",
            "7e01000020100000001441c30201020100121c03",
            "7e01000020100000001c08f302d30109000000000000000000005503",
        };
        for (int i = 0; i < init.Length; i++) {
            Wr(Hex(init[i]));
            rlen = 0;
            byte[] fr = ReadResponse(6.0, 0);
            Log("P3 init" + (i+1) + " resp=" + (fr != null ? fr.Length : 0) + (fr != null ? " hex=" + BitConverter.ToString(fr).Replace("-", "") : ""));
        }
        byte[] late = ReadResponse(6.0, 0);
        Log("P4 late d3 resp=" + (late != null ? late.Length : 0) + (late != null ? " hex=" + BitConverter.ToString(late).Replace("-", "") : ""));

        int blockSize = 0x5DC;
        int total = 64;
        int ok = 0;
        for (int k = 0; k < total; k++) {
            byte[] r = ReadBlock(k, blockSize, total);
            if (r != null) ok++;
            if (k < 2 || k % 16 == 0) Log("P5 block " + k + " resp=" + (r != null ? r.Length : 0));
        }
        Log("P6 RESULT " + ok + "/64");
        CloseHandle(pin); CloseHandle(pout);
        Log("P7 DONE");
    }
}
