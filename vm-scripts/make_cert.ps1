# 生成代码签名证书链 (testsigning 环境用)
# 结构: 根 CA (CA=TRUE) -> 签名证书 (CA=FALSE, CodeSigning EKU)
# 驱动包验证要求签名证书为 end-entity
$ErrorActionPreference = 'Stop'
$pw = ConvertTo-SecureString 'dmrcable' -AsPlainText -Force

Get-ChildItem Cert:\LocalMachine\My | Where-Object { $_.Subject -like '*DMR Cable*' } | Remove-Item -Force -ErrorAction SilentlyContinue
Get-ChildItem Cert:\LocalMachine\Root | Where-Object { $_.Subject -like '*DMR Cable*' } | Remove-Item -Force -ErrorAction SilentlyContinue
Get-ChildItem Cert:\LocalMachine\TrustedPublisher | Where-Object { $_.Subject -like '*DMR Cable*' } | Remove-Item -Force -ErrorAction SilentlyContinue

$root = New-SelfSignedCertificate -Type Custom `
  -Subject 'CN=DMR Cable Test Root CA' `
  -KeyUsage CertSign, CRLSign `
  -KeyAlgorithm RSA -KeyLength 3072 `
  -TextExtension '2.5.29.19={critical}{text}ca=1' `
  -KeyExportPolicy Exportable `
  -CertStoreLocation Cert:\LocalMachine\My `
  -NotAfter (Get-Date).AddYears(10)
Write-Host "Root: $($root.Thumbprint)"

$leaf = New-SelfSignedCertificate -Type Custom `
  -Subject 'CN=DMR Cable Test Signing' `
  -Signer $root `
  -KeyUsage DigitalSignature `
  -KeyAlgorithm RSA -KeyLength 3072 `
  -TextExtension '2.5.29.37={text}1.3.6.1.5.5.7.3.3' `
  -KeyExportPolicy Exportable `
  -CertStoreLocation Cert:\LocalMachine\My `
  -NotAfter (Get-Date).AddYears(10)
Write-Host "Leaf: $($leaf.Thumbprint)"

Export-PfxCertificate -Cert $leaf -FilePath C:\catwork\dmr-signing.pfx -Password $pw | Out-Null
Export-Certificate -Cert $root -FilePath C:\catwork\dmr-root.cer | Out-Null
Write-Host "Exported pfx + cer"

Import-Certificate -FilePath C:\catwork\dmr-root.cer -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
Import-Certificate -FilePath C:\catwork\dmr-root.cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
Write-Host "Imported to Root + TrustedPublisher"
