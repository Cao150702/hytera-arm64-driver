@echo off
powershell -NoProfile -Command "(New-Object Net.WebClient).DownloadFile('http://10.211.55.2:8800/p68.ps1','C:\dbg\p68.ps1')"
schtasks /create /tn p68run /tr "powershell -NoProfile -ExecutionPolicy Bypass -File C:\dbg\p68.ps1" /sc onlogon /ru ai-bot /rl highest /f
schtasks /run /tn p68run
