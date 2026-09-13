@echo off
set KITS=C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0
certutil -addstore -f Root C:\catwork\dmr-root.cer >nul 2>&1
certutil -addstore -f TrustedPublisher C:\catwork\dmr-root.cer >nul 2>&1
certutil -addstore -f TrustedPublisher C:\catwork\dmr-leaf.cer >nul 2>&1
mkdir C:\catwork\h4 2>nul
copy /y C:\drvsrc\ARM64\Release\hyterabulk.sys C:\catwork\h4\ >nul
copy /y C:\drvsrc\hyterabulk.inf C:\catwork\h4\ >nul
cd /d C:\catwork\h4
del hyterabulk.cat 2>nul
"%KITS%\x86\Inf2Cat.exe" /driver:. /os:10_GE_ARM64
echo [INF2CAT] %ERRORLEVEL%
"%KITS%\arm64\signtool.exe" sign /sm /sha1 <YOUR-LEAF-CERT-SHA1> /fd sha256 hyterabulk.cat
echo [SIGN] %ERRORLEVEL%
pnputil /remove-device "USB\VID_238B&PID_0A11\6&279c56a4&0&2"
pnputil /remove-device "USB\VID_238B&PID_0A11\6&279c56a4&0&1" >nul 2>&1
pnputil /delete-driver oem10.inf /uninstall /force >nul 2>&1
pnputil /add-driver C:\catwork\h4\hyterabulk.inf /install
echo [ADD] %ERRORLEVEL%
pnputil /scan-devices
echo [SCAN] %ERRORLEVEL%
