using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;

// Minimal DebugView: captures OutputDebugStringA from all processes via
// the DBWIN shared-memory protocol, writes to C:\hp780\dbwin.txt.
public class Dbwin {
    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    static extern IntPtr CreateFileMappingW(IntPtr h, IntPtr sa, uint prot, uint hi, uint lo, string name);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern IntPtr MapViewOfFile(IntPtr h, uint access, uint hi, uint lo, UIntPtr size);
    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    static extern IntPtr CreateEventW(IntPtr sa, bool manual, bool initial, string name);
    [DllImport("kernel32.dll")] static extern uint WaitForSingleObject(IntPtr h, uint ms);
    [DllImport("kernel32.dll")] static extern bool SetEvent(IntPtr h);
    [DllImport("kernel32.dll")] static extern bool ReleaseMutex(IntPtr h);
    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    static extern IntPtr OpenMutexW(uint access, bool inherit, string name);

    public static void Main() {
        var log = new StreamWriter(@"C:\hp780\dbwin.txt", false, System.Text.Encoding.UTF8);
        log.AutoFlush = true;
        IntPtr bufReady = CreateEventW(IntPtr.Zero, false, false, "DBWIN_BUFFER_READY");
        IntPtr dataReady = CreateEventW(IntPtr.Zero, false, false, "DBWIN_DATA_READY");
        IntPtr map = CreateFileMappingW(new IntPtr(-1), IntPtr.Zero, 0x04, 0, 4096, "DBWIN_BUFFER");
        if (map == IntPtr.Zero) {
            map = OpenFileMappingW(0x0006 /*FILE_MAP_WRITE|READ*/, false, "DBWIN_BUFFER");
        }
        if (map == IntPtr.Zero) { Console.WriteLine("map fail " + Marshal.GetLastWin32Error()); return; }
        IntPtr view = MapViewOfFile(map, 0x0004, 0, 0, (UIntPtr)4096);
        if (view == IntPtr.Zero) { Console.WriteLine("view fail " + Marshal.GetLastWin32Error()); return; }
        log.WriteLine("=== dbwin started " + DateTime.Now.ToString("HH:mm:ss") + " ===");
        SetEvent(bufReady);
        var t0 = DateTime.Now;
        while ((DateTime.Now - t0).TotalSeconds < 300) {
            if (WaitForSingleObject(dataReady, 500) != 0) continue;
            uint pid = (uint)Marshal.ReadInt32(view);
            string s = Marshal.PtrToStringAnsi(new IntPtr(view.ToInt64() + 4));
            log.WriteLine(DateTime.Now.ToString("HH:mm:ss.fff") + " [" + pid + "] " + s.TrimEnd('\r','\n'));
            SetEvent(bufReady);
        }
        log.WriteLine("=== done ===");
        log.Close();
    }
    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    static extern IntPtr OpenFileMappingW(uint access, bool inherit, string name);
    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    static extern IntPtr CreateMutexW(IntPtr sa, bool initial, string name);
}
