using System;
using System.Runtime.InteropServices;
using System.IO;
public class DbgLog {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern IntPtr CreateFileW(string f, uint a, uint s, IntPtr sa, uint c, uint fl, IntPtr t);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool DeviceIoControl(IntPtr h, uint code, IntPtr i, uint il, byte[] o, uint ol, out uint r, IntPtr o2);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    public static void Main() {
        IntPtr h = CreateFileW(@"\\.\usbbulk", 0, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
        Console.WriteLine("open=" + h);
        byte[] buf = new byte[32768]; uint r = 0;
        bool ok = DeviceIoControl(h, 0x220000, IntPtr.Zero, 0, buf, 32768, out r, IntPtr.Zero);
        Console.WriteLine("ioctl ok=" + ok + " bytes=" + r);
        if (ok && r > 0) {
            int n = (int)r;
            string s = System.Text.Encoding.ASCII.GetString(buf, 0, n);
            Console.WriteLine("magic=" + (n >= 16 ? s.Substring(0, 16) : "short"));
            Console.WriteLine("--- log ---");
            Console.WriteLine(s.Substring(Math.Min(16, n)));
        }
        CloseHandle(h);
    }
}
