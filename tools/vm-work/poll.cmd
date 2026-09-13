@echo off
for /l %%i in (1,1,90) do (
  C:\hp780\windump.exe >nul 2>&1
  ping -n 1 -w 90 127.0.0.1 >nul
)
