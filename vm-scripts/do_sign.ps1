$work = 'C:\catwork'
Set-Location $work
$signtool = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x86\signtool.exe'
& $signtool sign /a /v /fd SHA256 $work\Hytera-DMR-COM.cat 2>&1 | Out-File sign_out.txt
"exit: $LASTEXITCODE" | Out-File sign_out.txt -Append
