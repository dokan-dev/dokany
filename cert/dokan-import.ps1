Import-Certificate cert\DokanCA.cer -CertStoreLocation "Cert:\LocalMachine\Root"
Import-Certificate cert\DokanCA.cer -CertStoreLocation "Cert:\LocalMachine\TrustedPublisher"
Import-PfxCertificate .\cert\DokanSign.pfx  -CertStoreLocation "Cert:\CurrentUser\my"

& Bcdedit.exe -set TESTSIGNING ON

Write-Host You need to reboot to enable changes