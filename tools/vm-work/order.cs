
using System;
using System.Runtime.InteropServices;
using System.IO;

public class Order {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern IntPtr CreateFileW(string f, uint a, uint s, IntPtr sa, uint c, uint fl, IntPtr t);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool WriteFile(IntPtr h, byte[] b, uint n, out uint w, IntPtr o);
    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr h);

    static void Log(string m) {
        try { File.AppendAllText(@"C:\hp780\order-prog.txt", m + "\r\n"); } catch {}
        Console.WriteLine(m);
    }

    static byte[] Hex(string s) {
        byte[] b = new byte[s.Length / 2];
        for (int i = 0; i < b.Length; i++) b[i] = Convert.ToByte(s.Substring(i * 2, 2), 16);
        return b;
    }

    static void Wr(IntPtr pout, string tag) {
        byte[] f = Hex("7e0000fe20100000000c60e5");
        uint w = 0;
        bool ok = WriteFile(pout, f, (uint)f.Length, out w, IntPtr.Zero);
        Log(tag + " write=" + ok + " w=" + w + " err=" + Marshal.GetLastWin32Error());
    }

    public static void Main() {
        try { File.Delete(@"C:\hp780\order-prog.txt"); } catch {}
        // Case A: pipes only (no interface handle)
        IntPtr pin = CreateFileW(@"\\.\usbbulk\pipe00", 0x80000000, 3, IntPtr.Zero, 3, 0x40000000, IntPtr.Zero);
        IntPtr pout = CreateFileW(@"\\.\usbbulk\pipe01", 0x40000000, 3, IntPtr.Zero, 3, 0x40000000, IntPtr.Zero);
        Log("A1 pipes only, pout=" + pout);
        Wr(pout, "A2");
        CloseHandle(pin); CloseHandle(pout);
        Log("A3 closed");
        // Case B: pipes first, then interface
        pin = CreateFileW(@"\\.\usbbulk\pipe00", 0x80000000, 3, IntPtr.Zero, 3, 0x40000000, IntPtr.Zero);
        pout = CreateFileW(@"\\.\usbbulk\pipe01", 0x40000000, 3, IntPtr.Zero, 3, 0x40000000, IntPtr.Zero);
        IntPtr hif = CreateFileW(@"\\?\USB#VID_238B&PID_0A11#6&279c56a4&0&1#{15005312-b672-4817-94bf-a507ad4e057e}", 0xC0000000, 3, IntPtr.Zero, 3, 0x40000000, IntPtr.Zero);
        Log("B1 pipes then interface, hif=" + hif);
        Wr(pout, "B2");
        CloseHandle(hif);
        Log("B3 closed interface");
        Wr(pout, "B4");
        CloseHandle(pin); CloseHandle(pout);
        Log("B5 done");
    }
}
