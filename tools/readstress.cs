using System;
using System.Runtime.InteropServices;
using System.Threading;

class ReadStress {
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
        string blockFrame = "7e01000020100000001f53a402c7010c0000000001000000000000dc057c03";

        for (int i = 0; i < frames.Length; i++) {
            byte[] f = Hex(frames[i]);
            uint w = 0, r = 0;
            byte[] buf = new byte[1024];
            WriteFile(pout, f, (uint)f.Length, out w, IntPtr.Zero);
            ReadFile(pin, buf, 1024, out r, IntPtr.Zero);
            Console.WriteLine("handshake {0}: resp {1}B", i + 1, r);
        }

        int okBlocks = 0;
        for (int k = 0; k < 64; k++) {
            byte[] f = Hex(blockFrame);
            uint w = 0, r = 0;
            byte[] buf = new byte[2048];
            bool ok1 = WriteFile(pout, f, (uint)f.Length, out w, IntPtr.Zero);
            bool ok2 = ReadFile(pin, buf, 2048, out r, IntPtr.Zero);
            if (ok1 && ok2 && r > 1500) okBlocks++;
            if (k < 3 || k % 8 == 0) Console.WriteLine("block {0}: write={1} read={2}B", k, ok1, r);
        }
        Console.WriteLine("RESULT: {0}/64 blocks OK", okBlocks);
        CloseHandle(pin); CloseHandle(pout);
    }
}
