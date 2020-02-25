param (
	[string[]]$BuildPart = @('win', 'cygwin'),
	[string[]]$Platforms = @('Win32', 'x64', 'ARM', 'ARM64'),
	[string[]]$Configurations = @('Release', 'Debug')
)

. .\scripts\build_helper.ps1

$ErrorActionPreference = "Stop"

Add-VisualStudio-Path

if ($env:APPVEYOR -eq "True") { $CI_BUILD_ARG="/l:C:\Program Files\AppVeyor\BuildAgent\Appveyor.MSBuildLogger.dll" }
$msBuildPath=& Get-Command msbuild | Select-Object -ExpandProperty Definition
if (!([bool](Get-Command -Name buildWrapper -ErrorAction SilentlyContinue))) {
	set-alias buildWrapper "$msBuildPath"
}

if ($BuildPart -contains 'win') {
	foreach ($Configuration in $Configurations) {
		foreach ($Platform in $Platforms) {
			Write-Host Build dokan $Configuration $Platform ...
			Exec-External { buildWrapper .\dokan.sln /p:Configuration=$Configuration /p:Platform=$Platform /t:Build $CI_BUILD_ARG }
			Write-Host Build dokan $Configuration $Platform done !
		}
	}
}

if ($BuildPart -contains 'cygwin') {
	if ((Test-Path -Path env:CYGWIN_INST_DIR) -or (Test-Path -Path 'C:\cygwin64')) {
		$ErrorActionPreference = "Continue" #cmake has normal stdout through stderr...
		Exec-External { ./dokan_fuse/build.ps1 }
		$ErrorActionPreference = "Stop"
	} else {
		Write-Host "Cygwin/Msys2 build disabled"
	}
}