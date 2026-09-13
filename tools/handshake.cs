using System;
using System.Runtime.InteropServices;
using System.Threading;

class Handshake {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern IntPtr CreateFileW(string f, uint a, uint s, IntPtr sa, uint c, uint fl, IntPtr t);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool WriteFile(IntPtr h, byte[] b, uint n, out uint w, IntPtr o);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool ReadFile(IntPtr h, byte[] b, uint n, out uint r, IntPtr o);
    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr h);

    static byte[] Hex(string s) {
        byte[] b = new byte[s.Length / 2];
        for (int i = 0; i < b.Length; i++) b[i] = Convert.ToByte(s.Substring(i * 2, 2), 16);
        return b;
    }

    static void Main() {
        IntPtr pin = CreateFileW(@"\\.\usbbulk\pipe00", 0x80000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
        IntPtr pout = CreateFileW(@"\\.\usbbulk\pipe01", 0x40000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
        Console.WriteLine("open: in={0} out={1}", pin, pout);

        string[] frames = {
            "7e0000fe20100000000c60e5",
            "7e01000020100000001433d10205020100002a03",
            "7e0000fe20100000000c60e5",
            "7e01000020100000001431d30203020100002c03",
            "7e01000020100000002402f102c501110000000000000000000000000000000000005b03",
            "7e01000020100000001441c30201020100121c03",
            "7e01000020100000001c08f302d30109000000000000000000005503",
        };

        for (int i = 0; i < frames.Length; i++) {
            byte[] f = Hex(frames[i]);
            uint w = 0;
            bool ok = WriteFile(pout, f, (uint)f.Length, out w, IntPtr.Zero);
            Console.WriteLine("frame {0}: write ok={1} bytes={2}", i + 1, ok, w);

            byte[] buf = new byte[1024];
            uint r = 0;
            ok = ReadFile(pin, buf, 1024, out r, IntPtr.Zero);
            Console.WriteLine("frame {0}: read ok={1} bytes={2}", i + 1, ok, r);
            if (r > 0) {
                Console.WriteLine("  resp: {0}", BitConverter.ToString(buf, 0, (int)Math.Min(r, 48)));
            }
            Thread.Sleep(300);
        }
        CloseHandle(pin); CloseHandle(pout);
    }
}
