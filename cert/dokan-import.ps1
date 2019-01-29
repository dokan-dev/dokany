$currentUser = New-Object Security.Principal.WindowsPrincipal $([Security.Principal.WindowsIdentity]::GetCurrent())
$testadmin = $currentUser.IsInRole([Security.Principal.WindowsBuiltinRole]::Administrator)
if ($testadmin -eq $false) 
{
    write-host "Restarting in an Admin prompt"
	Start-Process powershell.exe -Verb RunAs -ArgumentList ('-noprofile -noexit -file "{0}" -elevated' -f ($myinvocation.MyCommand.Definition))
	exit $LASTEXITCODE
}


if (confirm-securebootUEFI) 
{
	write-Host "Secureboot is enabled. This needs to be disabled so that the driver signed with a self signed certificate can be loaded." -ForegroundColor Red
	write-host "See https://docs.microsoft.com/en-us/windows-hardware/manufacture/desktop/disabling-secure-boot#span-iddisablesecurebootspandisable-secure-boot for instructions to disable it" -ForegroundColor Red
	return;
}
else
{
	Write-Host "Looking for CertMgr"
	if (!(Get-Command "CertMgr" -errorAction SilentlyContinue))	
	{	
			Write-Host "CertMgr (Certificate Manager Tool)does not seem to be installed on your system." -ForegroundColor Red
			Write-Host "The Certificate Manager is automatically installed with Visual Studio." -ForegroundColor Red
			return;
	}

	Write-Host "Import Dokan certificate"
	Import-Certificate cert\DokanCA.cer -CertStoreLocation "Cert:\LocalMachine\Root"
	Import-Certificate cert\DokanCA.cer -CertStoreLocation "Cert:\LocalMachine\TrustedPublisher"
	Import-PfxCertificate .\cert\DokanSign.pfx  -CertStoreLocation "Cert:\CurrentUser\my"

	& Bcdedit.exe -set TESTSIGNING ON

	Write-Host "You need to reboot to enable changes"	
}
