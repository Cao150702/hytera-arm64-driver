using System;
using System.Runtime.InteropServices;
public class DbgClear {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern IntPtr CreateFileW(string f, uint a, uint s, IntPtr sa, uint c, uint fl, IntPtr t);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool DeviceIoControl(IntPtr h, uint code, IntPtr i, uint il, IntPtr o, uint ol, out uint r, IntPtr o2);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    public static void Main() {
        IntPtr h = CreateFileW(@"\\.\usbbulk", 0, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
        uint r = 0;
        bool ok = DeviceIoControl(h, 0x220001, IntPtr.Zero, 0, IntPtr.Zero, 0, out r, IntPtr.Zero);
        Console.WriteLine("clear ok=" + ok);
        CloseHandle(h);
    }
}
