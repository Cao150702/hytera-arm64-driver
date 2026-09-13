
using System;
using System.Runtime.InteropServices;
using System.IO;

public class FullSeq2 {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern IntPtr CreateFileW(string f, uint a, uint s, IntPtr sa, uint c, uint fl, IntPtr t);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool WriteFile(IntPtr h, byte[] b, uint n, out uint w, IntPtr o);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool ReadFile(IntPtr h, byte[] b, uint n, out uint r, IntPtr o);
    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr h);

    static void Log(string m) {
        try { File.AppendAllText(@"C:\hp780\fs2-prog.txt", m + "\r\n"); } catch {}
        Console.WriteLine(m);
    }

    static byte[] Hex(string s) {
        byte[] b = new byte[s.Length / 2];
        for (int i = 0; i < b.Length; i++) b[i] = Convert.ToByte(s.Substring(i * 2, 2), 16);
        return b;
    }

    public static void Main() {
        try { File.Delete(@"C:\hp780\fs2-prog.txt"); } catch {}
        IntPtr hif = CreateFileW(@"\\?\USB#VID_238B&PID_0A11#6&279c56a4&0&1#{15005312-b672-4817-94bf-a507ad4e057e}", 0xC0000000, 3, IntPtr.Zero, 3, 0x40000000, IntPtr.Zero);
        Log("G1 hif=" + hif + " err=" + Marshal.GetLastWin32Error());
        IntPtr pin = CreateFileW(@"\\.\usbbulk\pipe00", 0x80000000, 3, IntPtr.Zero, 3, 0x40000000, IntPtr.Zero);
        Log("G2 pin=" + pin + " err=" + Marshal.GetLastWin32Error());
        IntPtr pout = CreateFileW(@"\\.\usbbulk\pipe01", 0x40000000, 3, IntPtr.Zero, 3, 0x40000000, IntPtr.Zero);
        Log("G3 pout=" + pout + " err=" + Marshal.GetLastWin32Error());
        byte[] f = Hex("7e0000fe20100000000c60e5");
        uint w = 0;
        bool ok = WriteFile(pout, f, (uint)f.Length, out w, IntPtr.Zero);
        Log("G4 write=" + ok + " w=" + w + " err=" + Marshal.GetLastWin32Error());
        byte[] buf = new byte[1024];
        uint r = 0;
        ok = ReadFile(pin, buf, 1024, out r, IntPtr.Zero);
        Log("G5 read=" + ok + " r=" + r + " err=" + Marshal.GetLastWin32Error());
        CloseHandle(pout); CloseHandle(pin); CloseHandle(hif);
        Log("G6 done");
    }
}
