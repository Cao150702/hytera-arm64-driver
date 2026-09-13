using System;
using System.Runtime.InteropServices;

class Test {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern IntPtr CreateFileW(string f, uint a, uint s, IntPtr sa, uint c, uint fl, IntPtr t);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool WriteFile(IntPtr h, byte[] b, uint n, out uint w, IntPtr o);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool ReadFile(IntPtr h, byte[] b, uint n, out uint r, IntPtr o);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool DeviceIoControl(IntPtr h, uint code, byte[] i, uint il, byte[] o, uint ol, out uint r, IntPtr ov);
    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr h);

    static void Main() {
        // synchronous, non-overlapped
        IntPtr pin = CreateFileW(@"\\.\usbbulk\pipe00", 0x80000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
        IntPtr pout = CreateFileW(@"\\.\usbbulk\pipe01", 0x40000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
        Console.WriteLine("open in=0x{0:X} out=0x{1:X} err={2}", pin, pout, Marshal.GetLastWin32Error());
        if (pin == new IntPtr(-1) || pout == new IntPtr(-1)) { Console.WriteLine("OPEN FAIL"); return; }

        byte[] frame = { 0x7e,0x00,0x00,0xfe,0x20,0x10,0x00,0x00,0x00,0x0c,0x60,0xe5 };
        uint w = 0;
        bool ok = WriteFile(pout, frame, 12, out w, IntPtr.Zero);
        Console.WriteLine("write ok={0} bytes={1} err={2}", ok, w, Marshal.GetLastWin32Error());

        System.Threading.Thread.Sleep(500);
        byte[] buf = new byte[512];
        uint r = 0;
        ok = ReadFile(pin, buf, 512, out r, IntPtr.Zero);
        Console.WriteLine("read ok={0} bytes={1} err={2}", ok, r, Marshal.GetLastWin32Error());
        if (r > 0) {
            string hex = BitConverter.ToString(buf, 0, (int)r);
            Console.WriteLine("data: {0}", hex);
        }

        CloseHandle(pin); CloseHandle(pout);

        // debug ioctl
        IntPtr ctl = CreateFileW(@"\\.\usbbulk", 0xC0000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
        byte[] dbg = new byte[256];
        r = 0;
        ok = DeviceIoControl(ctl, 0x220000, null, 0, dbg, 256, out r, IntPtr.Zero);
        Console.WriteLine("dbg ioctl ok={0} err={1} bytes={2}", ok, Marshal.GetLastWin32Error(), r);
        if (r > 0) {
            Console.WriteLine("DBG: {0}", System.Text.Encoding.ASCII.GetString(dbg, 0, (int)r));
        }
        CloseHandle(ctl);
    }
}
