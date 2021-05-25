. .\scripts\build_helper.ps1

$ErrorActionPreference = "Stop"

Add-VisualStudio-Path

# This powershell script need SIGNTOOL, SHA1_CERTTHUMBPRINT, SHA2_CERTTHUMBPRINT and EV_CERTTHUMBPRINT env variable set
# SIGNTOOL - Signtool path
# SHA1_CERTTHUMBPRINT - SHA1 Sign certificat thumb print
# SHA2_CERTTHUMBPRINT - SHA2 Sign certificat thumb print
# EV_CERTTHUMBPRINT - EV Sign certificat thumb print

if ([string]::IsNullOrEmpty($env:SIGNTOOL)) {
	throw ("SIGNTOOL env variable is not set.");
}

if ([string]::IsNullOrEmpty($env:SHA1_CERTTHUMBPRINT) -or [string]::IsNullOrEmpty($env:SHA2_CERTTHUMBPRINT)) {
	throw ("SHA1_CERTTHUMBPRINT or SHA2_CERTTHUMBPRINT env variable are not set.");
}


set-alias st "$env:SIGNTOOL"

Write-Host Sign Dokan ...
New-Item -ItemType Directory -Force -Path Win32,x64,ARM,ARM64 | Out-Null
$files = Get-ChildItem -path Win32,x64,ARM,ARM64 -recurse -Include *.dll,*.exe
Exec-External { st sign /v /sha1 "$env:SHA1_CERTTHUMBPRINT" /t http://timestamp.digicert.com $files }
Exec-External { st sign /v /tr http://timestamp.digicert.com /td sha256 /fd sha256 /as /sha1 "$env:SHA2_CERTTHUMBPRINT" $files }
if (-not ([string]::IsNullOrEmpty($env:EV_CERTTHUMBPRINT)))
{
	# Remove test signatures before submiting to Microsoft hardware dashboard.
	$drive_files = Get-ChildItem -path Win32,x64,ARM,ARM64 -recurse -Include *.sys
	Exec-External { st remove /s $drive_files }
}
Write-Host Sign Dokan done !