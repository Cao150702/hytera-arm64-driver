using System;
using System.Text;
using System.Runtime.InteropServices;

// Enumerate all top-level windows of CPS.exe and dump their text
// (message box content included) so we can see the exact error.
public class WinDump {
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumWindowsProc cb, IntPtr lp);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr h, EnumWindowsProc cb, IntPtr lp);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    delegate bool EnumWindowsProc(IntPtr h, IntPtr lp);

    static uint targetPid = 0;
    static System.IO.StreamWriter log;

    static bool OnWindow(IntPtr h, IntPtr lp) {
        uint pid; GetWindowThreadProcessId(h, out pid);
        bool vis = IsWindowVisible(h);
        var cls = new StringBuilder(256); GetClassNameW(h, cls, 256);
        var txt = new StringBuilder(1024); GetWindowTextW(h, txt, 1024);
        if (pid == targetPid || (txt.Length > 0 && cls.ToString().Contains("#32770"))) {
            log.WriteLine((pid==targetPid ? "OWN " : "DLG ") + "cls=" + cls + " vis=" + vis + " text=[" + txt + "]");
            EnumChildWindows(h, OnChild, lp);
        }
        return true;
    }
    static bool OnChild(IntPtr h, IntPtr lp) {
        var cls = new StringBuilder(256); GetClassNameW(h, cls, 256);
        var txt = new StringBuilder(2048); GetWindowTextW(h, txt, 2048);
        if (txt.Length > 0)
            log.WriteLine("  CH cls=" + cls + " text=[" + txt + "]");
        return true;
    }

    public static void Main(string[] a) {
        if (a.Length > 0) uint.TryParse(a[0], out targetPid);
        else {
            foreach (var p in System.Diagnostics.Process.GetProcessesByName("CPS"))
                if (targetPid == 0) targetPid = (uint)p.Id;
        }
        log = new System.IO.StreamWriter(@"C:\hp780\windump.txt", true, Encoding.UTF8);
        log.WriteLine("=== " + DateTime.Now.ToString("HH:mm:ss.fff") + " pid=" + targetPid);
        EnumWindows(OnWindow, IntPtr.Zero);
        log.Close();
        Console.WriteLine("done pid=" + targetPid);
    }
}
