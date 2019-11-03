. .\scripts\build_helper.ps1

$ErrorActionPreference = "Stop"

Add-VisualStudio-Path

# This powershell script need SIGNTOOL CERTISSUER and EV_CERTISSUER env variable set
# SIGNTOOL - Signtool path
# CERTISSUER - Certificat issuer name
# EV_CERTISSUER - Certificat issuer name for EV sign

if ([string]::IsNullOrEmpty($env:SIGNTOOL)) {
	throw ("SIGNTOOL env variable is not set.");
}

set-alias st "$env:SIGNTOOL"

Write-Host Sign Dokan ...
New-Item -ItemType Directory -Force -Path Win32,x64,ARM,ARM64 | Out-Null
$files = Get-ChildItem -path Win32,x64,ARM,ARM64 -recurse -Include *.sys,*.cat,*.dll,*.exe
Exec-External { st sign /v /i "$env:CERTISSUER" /t http://timestamp.verisign.com/scripts/timstamp.dll $files }
Exec-External { st sign /as /fd SHA256 /v /i "$env:CERTISSUER" /tr http://timestamp.globalsign.com/?signature=sha2 /td SHA256 $files }
Write-Host Sign Dokan done !

# Need to sign with EV cert for Win10 before requesting Microsoft sign on dev hardware platform
if (-not ([string]::IsNullOrEmpty($env:EV_CERTISSUER)))
{
	Write-Host EV Sign Dokan ...
	Exec-External { st sign /as /fd sha256 /tr http://timestamp.digicert.com /td sha256 /i "$env:EV_CERTISSUER" $files }
	Write-Host EV Sign Dokan done !
}
