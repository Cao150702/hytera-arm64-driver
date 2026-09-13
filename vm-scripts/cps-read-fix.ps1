# cps-read-fix.ps1 - compatibility helper. Watches for the CPS read dialog
# and selects the newer USB port stack (class 0x14) for the running CPS
# process, so the cable + radio combination is recognized automatically.
$ErrorActionPreference='Continue'
Add-Type 'using System;using System.Runtime.InteropServices;using System.Text;using System.Collections.Generic;public class W{[DllImport("user32.dll")]public static extern bool EnumWindows(EnumProc cb,IntPtr l);public delegate bool EnumProc(IntPtr h,IntPtr l);[DllImport("user32.dll")]public static extern bool EnumChildWindows(IntPtr h,EnumProc cb,IntPtr l);[DllImport("user32.dll")]public static extern uint GetWindowThreadProcessId(IntPtr h,out uint pid);[DllImport("user32.dll",CharSet=CharSet.Unicode)]public static extern int GetWindowTextW(IntPtr h,StringBuilder s,int n);[DllImport("user32.dll",CharSet=CharSet.Unicode)]public static extern int GetClassNameW(IntPtr h,StringBuilder s,int n);public static string Txt(IntPtr h){var s=new StringBuilder(512);GetWindowTextW(h,s,512);return s.ToString();}public static string Cls(IntPtr h){var s=new StringBuilder(256);GetClassNameW(h,s,256);return s.ToString();}public static List<IntPtr> Find(uint pid,string cls,IntPtr parent){var o=new List<IntPtr>();EnumProc cb=(h,l)=>{uint p;GetWindowThreadProcessId(h,out p);if(p==pid){var c=Cls(h);if(cls==""||c==cls)o.Add(h);}return true;};if(parent==IntPtr.Zero)EnumWindows(cb,IntPtr.Zero);else EnumChildWindows(parent,cb,IntPtr.Zero);return o;}}' 2>$null
Add-Type 'using System;using System.Runtime.InteropServices;public class K32{[DllImport("kernel32.dll")]public static extern IntPtr OpenProcess(uint a,bool i,int p);[DllImport("kernel32.dll")]public static extern bool WriteProcessMemory(IntPtr h,IntPtr a,byte[] b,int n,IntPtr o);[DllImport("kernel32.dll")]public static extern bool CloseHandle(IntPtr h);}' 2>$null
$dlgTitle=[string]([char]0x8BFB+[char]0x9891)   # "读频"
$log='C:\dbg\readfix.log'
"watcher started $(Get-Date -Format 'HH:mm:ss')" | Out-File $log -Append -Encoding utf8
while($true){
  Start-Sleep -m 400
  $cps=Get-Process CPS -EA SilentlyContinue | Select-Object -First 1
  if(-not $cps){ continue }
  $applied=$false
  foreach($d in [W]::Find([uint32]$cps.Id,'#32770',[IntPtr]::Zero)){
    if([W]::Txt($d) -eq $dlgTitle){
      $h=[K32]::OpenProcess(0x1F0FFF,$false,[int]$cps.Id)
      if($h -ne [IntPtr]::Zero){
        $val=[BitConverter]::GetBytes([uint32]0x14)
        $ok=[K32]::WriteProcessMemory($h,[IntPtr]0xc1931c,$val,4,[IntPtr]::Zero)
        [K32]::CloseHandle($h)|Out-Null
        if($ok){ "applied pid=$($cps.Id) $(Get-Date -Format 'HH:mm:ss')" | Out-File $log -Append -Encoding utf8 }
      }
      $applied=$true; break
    }
  }
  if($applied){ Start-Sleep -m 1500 }
}
