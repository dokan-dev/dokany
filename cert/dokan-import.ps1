if (!(Get-Command "CertMgr" -errorAction SilentlyContinue))
{
	Write-Host "CertMgr (Certificate Manager Tool)does not seem to be installed on your system." -ForegroundColor Red
	Write-Host "The Certificate Manager is automatically installed with Visual Studio." -ForegroundColor Red
	return;
}

Import-Certificate cert\DokanCA.cer -CertStoreLocation "Cert:\LocalMachine\Root"
Import-Certificate cert\DokanCA.cer -CertStoreLocation "Cert:\LocalMachine\TrustedPublisher"
Import-PfxCertificate .\cert\DokanSign.pfx  -CertStoreLocation "Cert:\CurrentUser\my"

& Bcdedit.exe -set TESTSIGNING ON

Write-Host You need to reboot to enable changes