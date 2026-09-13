$log='C:\dbg\click68.txt'
"start $(Get-Date -Format 'HH:mm:ss')" | Out-File $log -Encoding utf8
try { Add-Type 'using System;using System.Runtime.InteropServices;using System.Text;using System.Collections.Generic;public class W{[DllImport("user32.dll")]public static extern bool EnumWindows(EnumProc cb,IntPtr l);public delegate bool EnumProc(IntPtr h,IntPtr l);[DllImport("user32.dll")]public static extern bool EnumChildWindows(IntPtr h,EnumProc cb,IntPtr l);[DllImport("user32.dll")]public static extern uint GetWindowThreadProcessId(IntPtr h,out uint pid);[DllImport("user32.dll",CharSet=CharSet.Unicode)]public static extern int GetWindowTextW(IntPtr h,StringBuilder s,int n);[DllImport("user32.dll",CharSet=CharSet.Unicode)]public static extern int GetClassNameW(IntPtr h,StringBuilder s,int n);[DllImport("user32.dll",CharSet=CharSet.Unicode)]public static extern IntPtr SendMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);[DllImport("user32.dll",CharSet=CharSet.Unicode)]public static extern IntPtr SendMessageW(IntPtr h,uint m,IntPtr w,StringBuilder l);[DllImport("user32.dll")]public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);[DllImport("user32.dll")]public static extern int GetDlgCtrlID(IntPtr h);public static string Txt(IntPtr h){var s=new StringBuilder(512);GetWindowTextW(h,s,512);return s.ToString();}public static string Cls(IntPtr h){var s=new StringBuilder(256);GetClassNameW(h,s,256);return s.ToString();}public static List<IntPtr> Find(uint pid,string cls,IntPtr parent){var o=new List<IntPtr>();EnumProc cb=(h,l)=>{uint p;GetWindowThreadProcessId(h,out p);if(p==pid){var c=Cls(h);if(cls==""||c==cls)o.Add(h);}return true;};if(parent==IntPtr.Zero)EnumWindows(cb,IntPtr.Zero);else EnumChildWindows(parent,cb,IntPtr.Zero);return o;}}' } catch {}
$cps=Get-Process CPS -EA SilentlyContinue | Select-Object -First 1
if(-not $cps){ "NO-CPS" | Out-File $log -Append -Encoding utf8; exit }
"pid $($cps.Id)" | Out-File $log -Append -Encoding utf8
try { (New-Object -ComObject WScript.Shell).AppActivate([int]$cps.Id) | Out-Null; "woke" | Out-File $log -Append -Encoding utf8 } catch {}
$ok=[string]([char]0x786E+[char]0x5B9A)
for($r=1;$r -le 6;$r++){
  Start-Sleep -s 2
  $dl=[W]::Find([uint32]$cps.Id,'#32770',[IntPtr]::Zero)
  if($dl.Count -eq 0){ "no-dlg r$r" | Out-File $log -Append -Encoding utf8; break }
  foreach($d in $dl){ $t=[W]::Txt($d); $st=@(); foreach($s in [W]::Find([uint32]$cps.Id,'Static',$d)){ $tx=[W]::Txt($s); if($tx.Length -gt 0){$st+=$tx} }
    "DLG $d [$t] [$($st -join ' | ')]" | Out-File $log -Append -Encoding utf8
    foreach($b in [W]::Find([uint32]$cps.Id,'Button',$d)){ if([W]::Txt($b) -eq $ok){ [W]::PostMessage($b,0x00F5,[IntPtr]::Zero,[IntPtr]::Zero)|Out-Null; "clicked OK on $($st -join ' ')" | Out-File $log -Append -Encoding utf8 } }
  }
  Start-Sleep -s 2
  $dl2=[W]::Find([uint32]$cps.Id,'#32770',[IntPtr]::Zero)
  if($dl2.Count -eq 0){ "clean after r$r" | Out-File $log -Append -Encoding utf8; break }
  try { $w=New-Object -ComObject WScript.Shell; $w.AppActivate([int]$cps.Id)|Out-Null; $w.SendKeys('{ENTER}') } catch {}
}
"D-DONE" | Out-File $log -Append -Encoding utf8
try { (New-Object Net.WebClient).UploadFile('http://10.211.55.2:8803/click68.txt','PUT',$log) } catch {}
