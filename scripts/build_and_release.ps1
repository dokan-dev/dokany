. .\scripts\build_helper.ps1

$ErrorActionPreference = "Stop"

Add-VisualStudio-Path

# This powershell script need SIGNTOOL, EV_CERTTHUMBPRINT and CHOCO_API_KEY env variable set
# SIGNTOOL - Signtool path
# EV_CERTTHUMBPRINT - EV Sign certificat thumb print
# CHOCO_API_KEY - Chocolatey API key to publish new installers


Write-Host Build installer ...
# Nuget restore sln fails to restore all the project of the solution for some reason so lets do it manually
dir .\dokan_wix -include ('*.wixproj', '*.vcxproj') -recurse | ForEach-Object { .\nuget restore $_.FullName -SolutionDirectory .\dokan_wix }
Exec-External { msbuild .\dokan_wix\Dokan_WiX.sln /p:Configuration=Release /p:Platform="Mixed Platforms" /t:rebuild /fileLogger }
copy .\dokan_wix\Bootstrapper\bin\x86\Release\DokanSetup.exe .\dokan_wix\
copy .\dokan_wix\bin\x64\Release\Dokan_x64.msi .\dokan_wix\
copy .\dokan_wix\bin\x86\Release\Dokan_x86.msi .\dokan_wix\
copy .\dokan_wix\bin\ARM64\Release\Dokan_ARM64.msi .\dokan_wix\
Exec-External { msbuild .\dokan_wix\Dokan_WiX.sln /p:Configuration=Debug /p:Platform="Mixed Platforms" /t:rebuild /fileLogger }
copy .\dokan_wix\Bootstrapper\bin\x86\Debug\DokanSetup.exe .\dokan_wix\DokanSetupDbg.exe
Write-Host Build installer done !

Write-Host Build archive ...
set-alias sz "$env:ProgramFiles\7-Zip\7z.exe"  
Exec-External { sz a -tzip .\dokan_wix\dokan.zip Win32 x64 ARM ARM64 }
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
