$ErrorActionPreference = 'Continue'
$work = 'C:\catwork'
New-Item -ItemType Directory -Force -Path $work | Out-Null
Copy-Item '<path-to>\usbser-com\Hytera-DMR-COM.inf' $work -Force
Set-Location $work
# makecat (arm64 toolchain)
$makecat = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\arm64\makecat.exe'
if (Test-Path $makecat) {
    & $makecat Hytera-DMR-COM.cdf 2>&1 | Out-File makecat_out.txt
    "makecat exit: $LASTEXITCODE" | Out-File makecat_out.txt -Append
} else {
    "makecat NOT FOUND" | Out-File makecat_out.txt
}
Get-ChildItem $work | Select-Object Name, Length | Out-File makecat_out.txt -Append
