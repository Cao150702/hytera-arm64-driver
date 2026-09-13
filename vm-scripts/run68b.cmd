@echo off
powershell -NoProfile -Command "(New-Object Net.WebClient).DownloadFile('http://10.211.55.2:8800/click68.ps1','C:\dbg\click68.ps1')"
schtasks /create /tn click68 /tr "powershell -NoProfile -ExecutionPolicy Bypass -File C:\dbg\click68.ps1" /sc onlogon /ru ai-bot /rl highest /f
schtasks /run /tn click68
