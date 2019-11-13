. .\scripts\build_helper.ps1

$ErrorActionPreference = "Stop"

Add-VisualStudio-Path

Write-Host Set Dokan version ...
if (!(Test-Path -Path .\dokan_wix\SetAssemblyVersion\bin\Release\SetAssemblyVersion.exe)) {
	Exec-External { msbuild .\dokan_wix\SetAssemblyVersion.sln /p:Configuration=Release /p:Platform="Any CPU" /t:rebuild }
}
Exec-External { .\dokan_wix\SetAssemblyVersion\bin\Release\SetAssemblyVersion CHANGELOG.md .\dokan_wix\version.xml . }
Write-Host Build dokan done !

Exec-External { .\scripts\build.ps1 }

Exec-External { .\scripts\sign.ps1 }

Exec-External { MakeCab /f .\dokan_wix\dokanx64.ddf }
Exec-External { MakeCab /f .\dokan_wix\dokanx86.ddf }

Read-Host -Prompt "Please submit drivers to developer hardware dashboard. Hit ENTER when it is done..." 

if (Test-Path -Path C:\cygwin64) {
	Write-Host Include Cygwin binaries in installer
	(gc .\dokan_wix\version.xml) -replace 'BuildCygwin="false"', 'BuildCygwin="true"' | sc .\dokan_wix\version.xml
} else {
	Write-Host Exclude Cygwin binaries from installer
	(gc .\dokan_wix\version.xml) -replace 'BuildCygwin="true"', 'BuildCygwin="false"' | sc .\dokan_wix\version.xml
}

Write-Host Build light installer ...
(gc .\dokan_wix\version.xml) -replace 'Compressed="yes"', 'Compressed="no"' | sc .\dokan_wix\version.xml
Exec-External { msbuild .\dokan_wix\Dokan_WiX.sln /p:Configuration=Release /p:Platform="Mixed Platforms" /t:rebuild /fileLogger }
copy .\dokan_wix\Bootstrapper\bin\Release\DokanSetup.exe .\dokan_wix\
copy .\dokan_wix\bin\x64\Release\Dokan_x64.msi .\dokan_wix\
copy .\dokan_wix\bin\x86\Release\Dokan_x86.msi .\dokan_wix\
Exec-External { msbuild .\dokan_wix\Dokan_WiX.sln /p:Configuration=Debug /p:Platform="Mixed Platforms" /t:rebuild /fileLogger }
copy .\dokan_wix\Bootstrapper\bin\Debug\DokanSetup.exe .\dokan_wix\DokanSetupDbg.exe
Write-Host Build light installer done !

Write-Host Build full installer ...
(gc .\dokan_wix\version.xml) -replace 'Compressed="no"', 'Compressed="yes"' | sc .\dokan_wix\version.xml
Exec-External { msbuild .\dokan_wix\Dokan_WiX.sln /p:Configuration=Release /p:Platform="Mixed Platforms" /t:rebuild /fileLogger }
copy .\dokan_wix\Bootstrapper\bin\Release\DokanSetup.exe .\dokan_wix\DokanSetup_redist.exe
Exec-External { msbuild .\dokan_wix\Dokan_WiX.sln /p:Configuration=Debug /p:Platform="Mixed Platforms" /t:rebuild /fileLogger }
copy .\dokan_wix\Bootstrapper\bin\Debug\DokanSetup.exe .\dokan_wix\DokanSetupDbg_redist.exe
Write-Host Build full installer done !

Write-Host Build archive ...
set-alias sz "$env:ProgramFiles\7-Zip\7z.exe"  
Exec-External { sz a -tzip .\dokan_wix\dokan.zip Win32 x64 ARM ARM64 }
Write-Host Build archive done !