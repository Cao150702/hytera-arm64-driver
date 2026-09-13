using System;
using System.IO;
using System.Runtime.InteropServices;

// Ground-truth probe for the real MCCI driver (QEMU): sends CPS's 12-byte
// session command, then reads the driver's response stream and saves it.
public class Probe {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern IntPtr CreateFileW(string f, uint a, uint s, IntPtr sa, uint c, uint fl, IntPtr t);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool ReadFile(IntPtr h, byte[] b, uint n, out uint r, IntPtr o);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool WriteFile(IntPtr h, byte[] b, uint n, out uint r, IntPtr o);

    static string LogPath = @"D:\probe-log.txt";
    static void Log(string m) {
        try { File.AppendAllText(LogPath, m + "\r\n"); } catch {
            try { File.AppendAllText(@"C:\probe-log.txt", m + "\r\n"); LogPath = @"C:\probe-log.txt"; } catch {}
        }
    }

    public static void Main() {
        Log("probe start " + DateTime.Now);
        var pin = CreateFileW(@"\\.\usbbulk\pipe00", 0x80000000, 3, IntPtr.Zero, 3, 0x40000000, IntPtr.Zero);
        var pout = CreateFileW(@"\\.\usbbulk\pipe01", 0x40000000, 3, IntPtr.Zero, 3, 0x40000000, IntPtr.Zero);
        Log("pin=" + pin + " pout=" + pout);
        if (pin == (IntPtr)(-1) || pout == (IntPtr)(-1)) { Log("OPEN FAIL"); return; }

        // CPS's session command: [pipe_sel][flags][opcode F8BF][HRCP 0x0203]
        byte[] cmd = new byte[]{0x0c,0x00,0xbf,0xf8,0x02,0x03,0x02,0x01,0x00,0x00,0x2c,0x03};
        uint w = 0;
        WriteFile(pout, cmd, 12, out w, IntPtr.Zero);
        Log("wrote=" + w);

        using (var ms = new MemoryStream()) {
            var buf = new byte[8192];
            var last = DateTime.Now;
            while (ms.Length < 110000 && (DateTime.Now - last).TotalSeconds < 5) {
                uint r = 0;
                if (ReadFile(pin, buf, 8192, out r, IntPtr.Zero)) {
                    ms.Write(buf, 0, (int)r);
                    if (r > 0) last = DateTime.Now;
                }
            }
            Log("read total " + ms.Length);
            // hex dump first 64 bytes of the stream
            var all = ms.ToArray();
            string hex = "";
            for (int i = 0; i < Math.Min(64, all.Length); i++) hex += all[i].ToString("x2") + " ";
            Log("head: " + hex);
            string outp = null;
            foreach (var drive in new[]{"D","E","F","G"}) {
                try {
                    File.WriteAllBytes(drive + @":\probe-out.bin", all);
                    outp = drive + @":\probe-out.bin";
                    break;
                } catch {}
            }
            if (outp == null) {
                File.WriteAllBytes(@"C:\probe-out.bin", all);
                outp = @"C:\probe-out.bin";
            }
            Log("saved " + all.Length + " to " + outp);
        }
        Log("probe done");
    }
}
