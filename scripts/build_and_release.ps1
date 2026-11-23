. .\scripts\build_helper.ps1

$ErrorActionPreference = "Stop"

Add-VisualStudio-Path

# This powershell script need SIGNTOOL, EV_CERTTHUMBPRINT and CHOCO_API_KEY env variable set
# SIGNTOOL - Signtool path
# EV_CERTTHUMBPRINT - EV Sign certificat thumb print
# CHOCO_API_KEY - Chocolatey API key to publish new installers

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

$cygwinDir = if (-not [string]::IsNullOrWhiteSpace($env:CYGWIN_INST_DIR)) { $env:CYGWIN_INST_DIR } else { 'C:\cygwin64' }
if (Test-Path -LiteralPath (Join-Path $cygwinDir 'bin\bash.exe') -PathType Leaf) {
	Write-Output "Include Cygwin binaries in installer"
	(Get-Content .\dokan_wix\version.xml) -replace 'BuildCygwin="false"', 'BuildCygwin="true"' | Set-Content .\dokan_wix\version.xml
} else {
	Write-Output "Exclude Cygwin binaries in installer"
	(Get-Content .\dokan_wix\version.xml) -replace 'BuildCygwin="true"', 'BuildCygwin="false"' | Set-Content .\dokan_wix\version.xml
}

$msys2Dir = if (-not [string]::IsNullOrWhiteSpace($env:MSYS2_INST_DIR)) { $env:MSYS2_INST_DIR } else { 'C:\msys64' }
if (Test-Path -LiteralPath (Join-Path $msys2Dir 'usr\bin\bash.exe') -PathType Leaf) {
	Write-Output "Include MSYS2 binaries in installer"
	(Get-Content .\dokan_wix\version.xml) -replace 'BuildMsys2="false"', 'BuildMsys2="true"' | Set-Content .\dokan_wix\version.xml
} else {
	Write-Output "Exclude MSYS2 binaries from installer"
	(Get-Content .\dokan_wix\version.xml) -replace 'BuildMsys2="true"', 'BuildMsys2="false"' | Set-Content .\dokan_wix\version.xml
}

Write-Output "Build installer ..."
# Nuget restore sln fails to restore all the project of the solution for some reason so lets do it manually
Get-ChildItem .\dokan_wix -Include ('*.wixproj', '*.vcxproj') -Recurse | ForEach-Object { nuget restore $_.FullName -SolutionDirectory .\dokan_wix }
Exec-External { msbuild .\dokan_wix\Dokan_WiX.sln /p:Configuration=Release /p:Platform="Mixed Platforms" /t:rebuild /fileLogger }
copy .\dokan_wix\Bootstrapper\bin\x86\Release\DokanSetup.exe .\dokan_wix\
copy .\dokan_wix\bin\x64\Release\Dokan_x64.msi .\dokan_wix\
copy .\dokan_wix\bin\ARM64\Release\Dokan_ARM64.msi .\dokan_wix\
Exec-External { msbuild .\dokan_wix\Dokan_WiX.sln /p:Configuration=Debug /p:Platform="Mixed Platforms" /t:rebuild /fileLogger }
copy .\dokan_wix\Bootstrapper\bin\x86\Debug\DokanSetup.exe .\dokan_wix\DokanSetupDbg.exe
Write-Host Build installer done !

Write-Host Build archive ...
set-alias sz "$env:ProgramFiles\7-Zip\7z.exe"  
Exec-External { sz a -tzip .\dokan_wix\dokan.zip Win32 x64 ARM64 }
Write-Host Build archive done !

if ([string]::IsNullOrEmpty($env:CHOCO_API_KEY)) {
	Write-Host Skip Chocolatey publication due to missing CHOCO_API_KEY env variable
	exit
}
$publishChocoConfirmation = Read-Host "Do you want to publish to Chocolatey ? [y/n]"
if ($publishChocoConfirmation -ne 'y') { exit }
$baseVersion = Select-String -Path .\dokan_wix\version.xml -Pattern 'BaseVersion="(.*)"' | % { $_.Matches.groups[1].Value }
$buildVersion = Select-String -Path .\dokan_wix\version.xml -Pattern 'BuildVersion="(.*)"' | % { $_.Matches.groups[1].Value }
Exec-External { .\chocolatey\release.ps1 "$baseVersion.$buildVersion" }
