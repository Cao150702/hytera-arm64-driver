$cert = Get-ChildItem Cert:\LocalMachine\My | Where-Object { $_.Subject -match 'DMR Cable' -and $_.HasPrivateKey } | Select-Object -First 1
if (-not $cert) { "NO CERT WITH KEY" | Out-File C:\catwork\cert_import.txt; exit 1 }
$pwd = ConvertTo-SecureString -String 'dmrcable2026' -Force -AsPlainText
Export-PfxCertificate -Cert $cert -FilePath C:\catwork\dmr-sign.pfx -Password $pwd | Out-Null
Import-PfxCertificate -FilePath C:\catwork\dmr-sign.pfx -CertStoreLocation Cert:\CurrentUser\My -Password $pwd | Out-Null
"IMPORTED: " + $cert.Thumbprint | Out-File C:\catwork\cert_import.txt
