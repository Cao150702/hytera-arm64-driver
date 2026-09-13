$ErrorActionPreference='Continue'
$log='C:\dbg\p68.txt'
$M='http://10.211.55.2:8803/'
"start $(Get-Date -Format 'HH:mm:ss')" | Out-File $log -Encoding utf8
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
try { Add-Type 'using System;using System.Runtime.InteropServices;using System.Text;using System.Collections.Generic;public class W{[DllImport("user32.dll")]public static extern bool EnumWindows(EnumProc cb,IntPtr l);public delegate bool EnumProc(IntPtr h,IntPtr l);[DllImport("user32.dll")]public static extern bool EnumChildWindows(IntPtr h,EnumProc cb,IntPtr l);[DllImport("user32.dll")]public static extern uint GetWindowThreadProcessId(IntPtr h,out uint pid);[DllImport("user32.dll",CharSet=CharSet.Unicode)]public static extern int GetWindowTextW(IntPtr h,StringBuilder s,int n);[DllImport("user32.dll",CharSet=CharSet.Unicode)]public static extern int GetClassNameW(IntPtr h,StringBuilder s,int n);[DllImport("user32.dll",CharSet=CharSet.Unicode)]public static extern IntPtr SendMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);[DllImport("user32.dll",CharSet=CharSet.Unicode)]public static extern IntPtr SendMessageW(IntPtr h,uint m,IntPtr w,StringBuilder l);[DllImport("user32.dll")]public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);[DllImport("user32.dll")]public static extern int GetDlgCtrlID(IntPtr h);public static string Txt(IntPtr h){var s=new StringBuilder(512);GetWindowTextW(h,s,512);return s.ToString();}public static string Cls(IntPtr h){var s=new StringBuilder(256);GetClassNameW(h,s,256);return s.ToString();}public static List<IntPtr> Find(uint pid,string cls,IntPtr parent){var o=new List<IntPtr>();EnumProc cb=(h,l)=>{uint p;GetWindowThreadProcessId(h,out p);if(p==pid){var c=Cls(h);if(cls==""||c==cls)o.Add(h);}return true;};if(parent==IntPtr.Zero)EnumWindows(cb,IntPtr.Zero);else EnumChildWindows(parent,cb,IntPtr.Zero);return o;}}' } catch {}
try { Add-Type 'using System;using System.Runtime.InteropServices;public class DRV{[DllImport("user32.dll")]public static extern bool SetCursorPos(int x,int y);[DllImport("user32.dll")]public static extern void mouse_event(uint f,uint x,uint y,uint d,IntPtr e);}' } catch {}
function Shot($fn){$b=[System.Windows.Forms.SystemInformation]::VirtualScreen;$bmp=New-Object System.Drawing.Bitmap $b.Width,$b.Height;$g=[System.Drawing.Graphics]::FromImage($bmp);$g.CopyFromScreen($b.Left,$b.Top,0,0,$b.Size);$bmp.Save('C:\dbg\shot.png');(New-Object Net.WebClient).UploadFile($M+$fn,'PUT','C:\dbg\shot.png')}
try { Add-Type 'using System;using System.Runtime.InteropServices;public class IO{[DllImport("kernel32.dll",SetLastError=true,CharSet=CharSet.Ansi)]public static extern IntPtr CreateFileA(string n,uint acc,uint share,IntPtr sec,uint disp,uint fl,IntPtr tmpl);[DllImport("kernel32.dll",SetLastError=true)]public static extern bool DeviceIoControl(IntPtr h,uint code,byte[] ib,uint ibl,byte[] ob,uint obl,ref uint ret,IntPtr ov);[DllImport("kernel32.dll")]public static extern bool CloseHandle(IntPtr h);[DllImport("kernel32.dll",SetLastError=true)]public static extern uint GetLastError();}' } catch {}

function Dump2($tag){
  $g='{8ff89775-cb39-41c3-a170-a52e990fa331}'
  foreach($pair in @(@('1','6&279c56a4&0&1'),@('2','6&279c56a4&0&2'))){
    $p='\\?\usb#vid_238b&pid_0a11#'+$pair[1]+'#'+$g
    $h=[IO]::CreateFileA($p,0xC0000000,3,[IntPtr]::Zero,3,0,[IntPtr]::Zero)
    if($h -eq [IntPtr](-1)){ "pnp$($pair[0])$tag open-fail err=$([IO]::GetLastError())" | Out-File $log -Append -Encoding utf8; continue }
    $ob=New-Object byte[] 32768; $ret=[uint32]0
    $okc=[IO]::DeviceIoControl($h,0x220000,$null,0,$ob,32768,[ref]$ret,[IntPtr]::Zero)
    [IO]::CloseHandle($h)|Out-Null
    [System.IO.File]::WriteAllBytes("C:\dbg\pnp$($pair[0])$tag.bin",$ob)
    "pnp$($pair[0])$tag ok=$okc ret=$ret" | Out-File $log -Append -Encoding utf8
  }
}

# ============ phase 1: build v97 ============
Get-Process CPS -EA SilentlyContinue | Stop-Process -Force
Start-Sleep -s 2
try { (New-Object Net.WebClient).DownloadFile('http://10.211.55.2:8800/v97.c','C:\drvsrc\hyterabulk.c'); "src OK $((Get-Item C:\drvsrc\hyterabulk.c).Length) bytes" | Out-File $log -Append -Encoding utf8 } catch { "src FAIL $($_.Exception.Message)" | Out-File $log -Append -Encoding utf8 }
$txt=[IO.File]::ReadAllText('C:\drvsrc\hyterabulk.inf')
$txt=$txt.Replace('1.0.1.14','1.0.1.15')
[IO.File]::WriteAllText('C:\drvsrc\hyterabulk.inf',$txt,[Text.Encoding]::ASCII)
"inf: " + ((Select-String -Path C:\drvsrc\hyterabulk.inf -Pattern 'DriverVer').Line) | Out-File $log -Append -Encoding utf8
cmd /c "rmdir /s /q C:\drvsrc\ARM64\Release\hyterabulk.tlog" 2>$null; & 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe' C:\drvsrc\hyterabulk.vcxproj /t:Rebuild /p:Configuration=Release /p:Platform=ARM64 /p:WindowsTargetPlatformVersion=10.0.26100.0 /nologo /v:m *>> $log
$sys=Get-Item C:\drvsrc\ARM64\Release\hyterabulk.sys -EA SilentlyContinue
"build exit=$LASTEXITCODE sys=$($sys.LastWriteTime) $($sys.Length)B" | Out-File $log -Append -Encoding utf8
# ============ phase 2: deploy ============
cmd /c C:\hytera-deploy\v22deploy.cmd
Start-Sleep -s 12
"--- v22log tail ---" | Out-File $log -Append -Encoding utf8
Get-Content C:\hytera-deploy\v22log.txt -Tail 30 | Out-File $log -Append -Encoding utf8
$p1=(Get-PnpDevice -InstanceId 'USB\VID_238B&PID_0A11\6&279c56a4&0&1' -EA SilentlyContinue).Status
$p2=(Get-PnpDevice -InstanceId 'USB\VID_238B&PID_0A11\6&279c56a4&0&2' -EA SilentlyContinue).Status
"pnp1=$p1 pnp2=$p2" | Out-File $log -Append -Encoding utf8
Start-Sleep -s 3
& C:\dbg\dbglog.exe > C:\dbg\live68a.log 2>&1
Dump2 'a'

Get-ScheduledTask -EA SilentlyContinue | Where-Object {$_.TaskName -match 'daemon|pshell|cmd|ps1'} | ForEach-Object { Stop-ScheduledTask -TaskName $_.TaskName -EA SilentlyContinue; Disable-ScheduledTask -TaskName $_.TaskName -EA SilentlyContinue; "task $($_.TaskName) disabled" | Out-File $log -Append -Encoding utf8 }
Get-Process powershell -EA SilentlyContinue | Where-Object {$_.SessionId -eq 0} | Stop-Process -Force -EA SilentlyContinue
Get-Process CPS -EA SilentlyContinue | Stop-Process -Force
Start-Sleep -s 3
Get-Process conhost -EA SilentlyContinue | Where-Object {$_.MainWindowTitle -ne ''} | Stop-Process -Force -EA SilentlyContinue
Start-Sleep -s 1
Start-Process powershell -ArgumentList '-NoProfile -ExecutionPolicy Bypass -File C:\dbg\cps-read-fix.ps1' -WindowStyle Hidden
Start-Sleep -s 2
Start-Process 'C:\cpsim\CPS.exe'
Start-Sleep -s 40
$cps=Get-Process CPS -EA SilentlyContinue | Select-Object -First 1
if(-not $cps){ "NO-CPS" | Out-File $log -Append -Encoding utf8; try { (New-Object Net.WebClient).UploadFile($M+'readfix.log','PUT','C:\dbg\readfix.log') } catch {}
(New-Object Net.WebClient).UploadFile($M+'p65.txt','PUT',$log); exit }
"pid $($cps.Id)" | Out-File $log -Append -Encoding utf8
# ============ phase 3: read test on the 20-class path ============
$cps=Get-Process CPS -EA SilentlyContinue | Select-Object -First 1
if(-not $cps){ "NO-CPS" | Out-File $log -Append -Encoding utf8; try { (New-Object Net.WebClient).UploadFile($M+'readfix.log','PUT','C:\dbg\readfix.log') } catch {}
(New-Object Net.WebClient).UploadFile($M+'p65.txt','PUT',$log); exit }
"pid $($cps.Id)" | Out-File $log -Append -Encoding utf8
Remove-Item C:\dbg\args.log,C:\dbg\patch.log,C:\dbg\hook.log,C:\dbg\read-dump.bin,C:\dbg\write-dump.bin -EA SilentlyContinue
"clean run (no debug tools)" | Out-File $log -Append -Encoding utf8
Start-Sleep -s 8
$root=[System.Windows.Automation.AutomationElement]::RootElement
$cond=New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ProcessIdProperty,[int]$cps.Id)
$main=$null
foreach($w in $root.FindAll([System.Windows.Automation.TreeScope]::Children,$cond)){ $r=$w.Current.BoundingRectangle; if($r.Width -gt 500){ $main=$w; break } }
if($main){ $mr=$main.Current.BoundingRectangle; $cx=[int]($mr.X + $mr.Width/2); $cy=[int]($mr.Y + 20)
  [DRV]::SetCursorPos($cx,$cy)|Out-Null; Start-Sleep -m 300; [DRV]::mouse_event(2,0,0,0,[IntPtr]::Zero); Start-Sleep -m 90; [DRV]::mouse_event(4,0,0,0,[IntPtr]::Zero) }
Start-Sleep -m 800
[System.Windows.Forms.SendKeys]::SendWait('^r')
$ok=[string]([char]0x786E+[char]0x5B9A)
$dlg=[IntPtr]::Zero
for($w=1;$w -le 20;$w++){
  Start-Sleep -s 3
  $dlgs=[W]::Find([uint32]$cps.Id,'#32770',[IntPtr]::Zero)
  foreach($d in $dlgs){ $cs=[W]::Find([uint32]$cps.Id,'ComboBox',$d); if($cs.Count -gt 0){ $dlg=$d; break } }
  if($dlg -ne [IntPtr]::Zero){ $combo0=([W]::Find([uint32]$cps.Id,'ComboBox',$dlg))[0]
    $n0=[W]::SendMessageW($combo0,0x146,[IntPtr]::Zero,[IntPtr]::Zero).ToInt32()
    for($i0=0;$i0 -lt $n0;$i0++){ $sb0=New-Object System.Text.StringBuilder 512; [W]::SendMessageW($combo0,0x148,[IntPtr]$i0,$sb0)|Out-Null; if($sb0.ToString() -match 'USB'){ "wait$w comboOK [$($sb0.ToString())]" | Out-File $log -Append -Encoding utf8; break } }
    if($n0 -gt 0){ break } }
  "wait$w dlg=$dlg" | Out-File $log -Append -Encoding utf8
}
if($dlg -ne [IntPtr]::Zero){
  $combo=([W]::Find([uint32]$cps.Id,'ComboBox',$dlg))[0]
  $n=[W]::SendMessageW($combo,0x146,[IntPtr]::Zero,[IntPtr]::Zero).ToInt32()
  for($i=0;$i -lt $n;$i++){ $sb=New-Object System.Text.StringBuilder 512; [W]::SendMessageW($combo,0x148,[IntPtr]$i,$sb)|Out-Null; "item $i [$($sb.ToString())]" | Out-File $log -Append -Encoding utf8 }
  for($i=0;$i -lt $n;$i++){ $sb=New-Object System.Text.StringBuilder 512; [W]::SendMessageW($combo,0x148,[IntPtr]$i,$sb)|Out-Null; if($i -eq 0){ [W]::SendMessageW($combo,0x14E,[IntPtr]$i,[IntPtr]::Zero)|Out-Null
    $cid=[W]::GetDlgCtrlID($combo); $wp=[IntPtr]((1 -shl 16) -bor ($cid -band 0xFFFF))
    [W]::PostMessage($dlg,0x111,$wp,$combo)|Out-Null; Start-Sleep -m 300
    "picked item0 USB1 + notify id=$cid" | Out-File $log -Append -Encoding utf8; break } }
  Start-Sleep -m 500
  foreach($b in [W]::Find([uint32]$cps.Id,'Button',$dlg)){ if([W]::Txt($b) -eq $ok){ [W]::PostMessage($b,0x00F5,[IntPtr]::Zero,[IntPtr]::Zero)|Out-Null; "port OK posted" | Out-File $log -Append -Encoding utf8; break } }
} else { "NO-PORT-DIALOG" | Out-File $log -Append -Encoding utf8 }
Start-Sleep -s 8
$dlgs=[W]::Find([uint32]$cps.Id,'#32770',[IntPtr]::Zero)
$rdlg=[IntPtr]::Zero
foreach($d in $dlgs){ $t=[W]::Txt($d); "dlg [$t] $d" | Out-File $log -Append -Encoding utf8; if($t -ne 'CPS' -and $t.Length -gt 0 -and $rdlg -eq [IntPtr]::Zero){ $rdlg=$d } }
# Port-stack compatibility is applied by the cps-read-fix.ps1 watcher during this run.
"natural run (compat handled by watcher)" | Out-File $log -Append -Encoding utf8

if($rdlg -ne [IntPtr]::Zero){
  Start-Sleep -s 2
  foreach($b in [W]::Find([uint32]$cps.Id,'Button',$rdlg)){ if([W]::Txt($b) -eq $ok){ [W]::PostMessage($b,0x00F5,[IntPtr]::Zero,[IntPtr]::Zero)|Out-Null; "read OK posted in $rdlg" | Out-File $log -Append -Encoding utf8; break } }
}
Start-Sleep -s 3
$shots=0
$zero=0
$succ_i=0
$clicked=0
Start-Sleep -s 20
& C:\dbg\dbglog.exe > C:\dbg\live68b.log 2>&1
Dump2 'b'
for($i=1;$i -le 120;$i++){
  Start-Sleep -s 4
  $dl=[W]::Find([uint32]$cps.Id,'#32770',[IntPtr]::Zero)
  $txts=@()
  foreach($d in $dl){ foreach($st in [W]::Find([uint32]$cps.Id,'Static',$d)){ $tx=[W]::Txt($st); if($tx.Length -gt 0){ $txts+=$tx } } }
  $ts=$txts -join ' | '
  if($i % 5 -eq 0 -or $dl.Count -eq 0 -or $ts -match '读频成功'){ "t+$($i*4)s dlg=$($dl.Count) txt=[$ts]" | Out-File $log -Append -Encoding utf8 }
  if($dl.Count -eq 0){ $zero++ } else { $zero=0 }
  if($ts -match '读频成功' -and $succ_i -eq 0){ $succ_i=$i; "SUCCESS at t+$($i*4)s" | Out-File $log -Append -Encoding utf8; & C:\dbg\dbglog.exe > C:\dbg\live68c.log 2>&1; Dump2 'c' }
  if($succ_i -gt 0 -and $i -gt $succ_i+1 -and $clicked -lt 10 -and $dl.Count -gt 0){
    $clicked++
    try { (New-Object -ComObject WScript.Shell).AppActivate([int]$cps.Id) | Out-Null } catch {}
    $c2=0
    foreach($d in $dl){ foreach($bb in [W]::Find([uint32]$cps.Id,'Button',$d)){ if([W]::Txt($bb) -eq $ok){ [W]::PostMessage($bb,0x00F5,[IntPtr]::Zero,[IntPtr]::Zero)|Out-Null; $c2++ } } }
    "click#$clicked posted=$c2 at t+$($i*4)s" | Out-File $log -Append -Encoding utf8
    Start-Sleep -m 500
  }
  if($i -eq 8 -or $i -eq 20 -or $i -eq 40 -or $i -eq 65){ $shots++; Shot "p68-s$shots.png" }
  if($zero -ge 3 -and $i -gt 8){ "ALL-DIALOGS-GONE x3 at t+$($i*4)s" | Out-File $log -Append -Encoding utf8; break }
}
Shot 'p68-tfinal.png'
Start-Sleep -s 15
Shot 'p68-tfinal2.png'
$dl=[W]::Find([uint32]$cps.Id,'#32770',[IntPtr]::Zero)
foreach($d in $dl){ $txts=@(); foreach($st in [W]::Find([uint32]$cps.Id,'Static',$d)){ $tx=[W]::Txt($st); if($tx.Length -gt 0){$txts+=$tx} }; "POST dlg=[$([W]::Txt($d))] [$($txts -join ' | ')]" | Out-File $log -Append -Encoding utf8 }
Get-ChildItem C:\cpsim -Recurse -EA SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 10 | ForEach-Object { "FILE $($_.LastWriteTime) $($_.Length) $($_.FullName)" } | Out-File $log -Append -Encoding utf8
& C:\dbg\dbglog.exe > C:\dbg\live68d.log 2>&1
Dump2 'd'
Start-Sleep -s 2
foreach($f in @('pnp1a.bin','pnp2a.bin','pnp1b.bin','pnp2b.bin','pnp1c.bin','pnp2c.bin','pnp1d.bin','pnp2d.bin','live68a.log','live68b.log','live68c.log','live68d.log')){ try { (New-Object Net.WebClient).UploadFile($M+$f,'PUT',('C:\dbg\'+$f)) } catch { "upfail $f" | Out-File $log -Append -Encoding utf8 } }
try { (New-Object Net.WebClient).UploadFile($M+'readfix.log','PUT','C:\dbg\readfix.log') } catch {}
"D-DONE" | Out-File $log -Append -Encoding utf8
(New-Object Net.WebClient).UploadFile($M+'p68.txt','PUT',$log)
