
using System;
using System.Runtime.InteropServices;
using System.IO;

public class IfOpen {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern IntPtr CreateFileW(string f, uint a, uint s, IntPtr sa, uint c, uint fl, IntPtr t);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool WriteFile(IntPtr h, byte[] b, uint n, out uint w, IntPtr o);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool ReadFile(IntPtr h, byte[] b, uint n, out uint r, IntPtr o);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool DeviceIoControl(IntPtr h, uint code, IntPtr inb, uint ins, IntPtr outb, uint outs, out uint ret, IntPtr o);
    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr h);

    static void Log(string m) {
        try { File.AppendAllText(@"C:\hp780\ifopen-prog.txt", m + "\r\n"); } catch {}
        Console.WriteLine(m);
    }

    public static void Main() {
        try { File.Delete(@"C:\hp780\ifopen-prog.txt"); } catch {}
        Log("I1 opening interface path");
        IntPtr h = CreateFileW(@"\\?\USB#VID_238B&PID_0A11#6&279c56a4&0&1#{15005312-b672-4817-94bf-a507ad4e057e}", 0xC0000000, 3, IntPtr.Zero, 3, 0x40000000, IntPtr.Zero);
        Log("I2 open result=" + h + " err=" + Marshal.GetLastWin32Error());
        if (h == (IntPtr)(-1)) { Log("I9 OPEN FAILED"); return; }
        Log("I3 closing");
        CloseHandle(h);
        Log("I4 SURVIVED CLOSE");
        IntPtr h2 = CreateFileW(@"\\?\USB#VID_238B&PID_0A11#6&279c56a4&0&1#{87e5a6ea-d48b-4883-8440-81d8a22508d7}", 0xC0000000, 3, IntPtr.Zero, 3, 0x40000000, IntPtr.Zero);
        Log("I5 open2=" + h2 + " err=" + Marshal.GetLastWin32Error());
        if (h2 != (IntPtr)(-1)) CloseHandle(h2);
        Log("I6 DONE");
    }
}
