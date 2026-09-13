using System;
using System.Runtime.InteropServices;
using System.IO;
public class BlkDump {
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
    [DllImport("kernel32.dll")] static extern IntPtr CreateEventW(IntPtr a, bool m, bool i, string n);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [StructLayout(LayoutKind.Sequential)]
    struct OVERLAPPED { public UIntPtr i1, i2; public int o1, o2; public IntPtr ev; }
    static IntPtr pin, pout;
    static byte[] rbuf = new byte[1 << 20]; static int rlen = 0;
    static bool rdPending = false; static byte[] rdbuf = new byte[65536]; static OVERLAPPED rdOv;
    static void Log(string m) { Console.WriteLine(m); }
    static void Wr(byte[] d) {
        uint w = 0; OVERLAPPED ov = new OVERLAPPED(); ov.ev = CreateEventW(IntPtr.Zero, true, false, null);
        if (!WriteFileOv(pout, d, (uint)d.Length, out w, ref ov)) {
            if (Marshal.GetLastWin32Error() == 997) { uint wr = WaitForSingleObject(ov.ev, 3000); GetOverlappedResult(pout, ref ov, out w, false); }
        }
    }
    static bool Poll(uint t) {
        if (!rdPending) {
            uint r = 0; rdOv = new OVERLAPPED(); rdOv.ev = CreateEventW(IntPtr.Zero, true, false, null);
            if (!ReadFileOv(pin, rdbuf, 65536, out r, ref rdOv)) {
                if (Marshal.GetLastWin32Error() == 997) rdPending = true; else return false;
            } else { if (r > 0) { Array.Copy(rdbuf, 0, rbuf, rlen, (int)r); rlen += (int)r; } return r > 0; }
        }
        uint wr = WaitForSingleObject(rdOv.ev, t);
        if (wr == 258) return false;
        uint got = 0; GetOverlappedResult(pin, ref rdOv, out got, false); rdPending = false;
        if (got > 0) { Array.Copy(rdbuf, 0, rbuf, rlen, (int)got); rlen += (int)got; return true; }
        return false;
    }
    static byte[] GetFrame() {
        int i = Array.IndexOf(rbuf, (byte)0x7E);
        if (i < 0 || rlen - i < 12) return null;
        ushort l = (ushort)((rbuf[i+8] << 8) | rbuf[i+9]);
        if (l < 12 || l > 8192 || rlen - i < l) return null;
        byte[] fr = new byte[l]; Array.Copy(rbuf, i, fr, 0, l);
        byte[] rest = new byte[rbuf.Length]; Array.Copy(rbuf, i + l, rest, 0, rlen - i - l); Array.Copy(rest, rbuf, rest.Length);
        rlen -= i + l; return fr;
    }
    static byte[] ReadResponse(double timeout) {
        DateTime t0 = DateTime.Now;
        while ((DateTime.Now - t0).TotalSeconds < timeout) {
            byte[] fr = GetFrame();
            if (fr != null && fr[1] == 0x04) return fr;
            Poll(250);
        }
        return null;
    }
    static byte[] Hex(string s) { byte[] b = new byte[s.Length/2]; for (int i=0;i<b.Length;i++) b[i]=Convert.ToByte(s.Substring(i*2,2),16); return b; }
    static byte[] Frame(byte ftype, ushort seq, ushort prefix, byte[] h) {
        byte[] f = new byte[12 + h.Length];
        f[0]=0x7E; f[1]=ftype; f[2]=0; f[3]=0; f[4]=0x20; f[5]=0x10;
        f[6]=(byte)(seq>>8); f[7]=(byte)(seq&0xFF);
        ushort total=(ushort)(12+h.Length); f[8]=(byte)(total>>8); f[9]=(byte)(total&0xFF);
        f[10]=(byte)(prefix>>8); f[11]=(byte)(prefix&0xFF);
        Array.Copy(h, 0, f, 12, h.Length); return f;
    }
    static byte[] Hrcp(ushort ax, byte ln, byte b, byte[] data) {
        byte[] p = new byte[5 + data.Length];
        p[0]=0x02; p[1]=(byte)(ax&0xFF); p[2]=(byte)(ax>>8); p[3]=ln; p[4]=b;
        Array.Copy(data, 0, p, 5, data.Length);
        int s=0; for (int k=1;k<p.Length;k++) s+=p[k];
        byte[] outr = new byte[p.Length+2]; Array.Copy(p, outr, p.Length);
        outr[p.Length]=(byte)(((~s)+0x33)&0xFF); outr[p.Length+1]=0x03; return outr;
    }
    public static void Main() {
        pin = CreateFileW(@"\\.\usbbulk\pipe00", 0x80000000, 3, IntPtr.Zero, 3, 0x40000000, IntPtr.Zero);
        pout = CreateFileW(@"\\.\usbbulk\pipe01", 0x40000000, 3, IntPtr.Zero, 3, 0x40000000, IntPtr.Zero);
        Log("opened " + pin + " " + pout);
        DateTime d0 = DateTime.Now;
        while ((DateTime.Now - d0).TotalSeconds < 0.8) Poll(100);
        rlen = 0;
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
            byte[] fr = ReadResponse(6.0);
            Log("init" + (i+1) + " resp=" + (fr != null ? BitConverter.ToString(fr).Replace("-","") : "0"));
        }
        // wait up to 6s for the late d3 response
        byte[] late = ReadResponse(6.0);
        Log("late resp=" + (late != null ? BitConverter.ToString(late).Replace("-","") : "0"));
        // block 0 read
        int addr = 0, size = 0x5DC;
        byte[] data = new byte[12];
        byte[] head = Hex("000000010000");
        Array.Copy(head, data, 6);
        data[6]=(byte)(addr&0xFF); data[7]=(byte)((addr>>8)&0xFF); data[8]=(byte)((addr>>16)&0xFF); data[9]=(byte)((addr>>24)&0xFF);
        data[10]=(byte)(size&0xFF); data[11]=(byte)(size>>8);
        Wr(Frame(0x01, 0, 0x53a4, Hrcp(0x01C7, 12, 0, data)));
        rlen = 0;
        byte[] r = ReadResponse(8.0);
        Log("block0 resp=" + (r != null ? BitConverter.ToString(r).Replace("-","") : "0"));
        // second block read
        addr = 1500;
        data[6]=(byte)(addr&0xFF); data[7]=(byte)((addr>>8)&0xFF); data[8]=(byte)((addr>>16)&0xFF); data[9]=(byte)((addr>>24)&0xFF);
        Wr(Frame(0x01, 0, 0x4da9, Hrcp(0x01C7, 12, 0, data)));
        rlen = 0;
        r = ReadResponse(8.0);
        Log("block1 resp=" + (r != null ? BitConverter.ToString(r).Replace("-","") : "0"));
        Log("DONE");
        CloseHandle(pin); CloseHandle(pout);
    }
}
