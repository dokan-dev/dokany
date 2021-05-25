. .\scripts\build_helper.ps1

$ErrorActionPreference = "Stop"

Add-VisualStudio-Path

# This powershell script need SIGNTOOL and EV_CERTTHUMBPRINT env variable set
# SIGNTOOL - Signtool path
# EV_CERTTHUMBPRINT - EV Sign certificat thumb print

Write-Host Set Dokan version ...
if (!(Test-Path -Path .\dokan_wix\SetAssemblyVersion\bin\Release\SetAssemblyVersion.exe)) {
	Exec-External { msbuild .\dokan_wix\SetAssemblyVersion.sln /p:Configuration=Release /p:Platform="Any CPU" /t:rebuild }
}
Exec-External { .\dokan_wix\SetAssemblyVersion\bin\Release\SetAssemblyVersion CHANGELOG.md .\dokan_wix\version.xml . }
Write-Host Build dokan version done !

Exec-External { .\scripts\build.ps1 }

Exec-External { .\scripts\sign.ps1 }

Exec-External { MakeCab /f .\dokan_wix\dokan.ddf }
if (-not ([string]::IsNullOrEmpty($env:EV_CERTTHUMBPRINT)))
{
	Write-Host EV Sign cab ...
	set-alias st "$env:SIGNTOOL"
	Exec-External { st sign /v /tr http://timestamp.digicert.com /td sha256 /fd sha256 /as /sha1 "$env:EV_CERTTHUMBPRINT" .\dokan_wix\Dokan.cab }
	Write-Host EV Sign cab done
	Read-Host -Prompt "Please submit driver cab to developer hardware dashboard. Hit ENTER when it is done..." 
}
else { Write-Host EV_CERTTHUMBPRINT env variable is missing. EV Signature cab is needed for developer hardware dashboard submission. }

if (Test-Path -Path C:\cygwin64) {
	Write-Host Include Cygwin binaries in installer
	(gc .\dokan_wix\version.xml) -replace 'BuildCygwin="false"', 'BuildCygwin="true"' | sc .\dokan_wix\version.xml
} else {
	Write-Host Exclude Cygwin binaries from installer
	(gc .\dokan_wix\version.xml) -replace 'BuildCygwin="true"', 'BuildCygwin="false"' | sc .\dokan_wix\version.xml
}

Write-Host Build installer ...
Exec-External { msbuild .\dokan_wix\Dokan_WiX.sln /p:Configuration=Release /p:Platform="Mixed Platforms" /t:rebuild /fileLogger }
copy .\dokan_wix\Bootstrapper\bin\Release\DokanSetup.exe .\dokan_wix\
copy .\dokan_wix\bin\x64\Release\Dokan_x64.msi .\dokan_wix\
copy .\dokan_wix\bin\x86\Release\Dokan_x86.msi .\dokan_wix\
Exec-External { msbuild .\dokan_wix\Dokan_WiX.sln /p:Configuration=Debug /p:Platform="Mixed Platforms" /t:rebuild /fileLogger }
copy .\dokan_wix\Bootstrapper\bin\Debug\DokanSetup.exe .\dokan_wix\DokanSetupDbg.exe
Write-Host Build installer done !

Write-Host Build archive ...
set-alias sz "$env:ProgramFiles\7-Zip\7z.exe"  
Exec-External { sz a -tzip .\dokan_wix\dokan.zip Win32 x64 ARM ARM64 }
Write-Host Build archive done !