param (
	[string[]]$BUILD_PART = @('lib', 'sys', 'cygwin')
)

. .\scripts\build_helper.ps1

$ErrorActionPreference = "Stop"

Add-VisualStudio-Path

if ($env:APPVEYOR -eq "True") { $CI_BUILD_ARG="/l:C:\Program Files\AppVeyor\BuildAgent\Appveyor.MSBuildLogger.dll" }
$msBuildPath=& Get-Command msbuild | Select-Object -ExpandProperty Definition
if (!([bool](Get-Command -Name buildWrapper -ErrorAction SilentlyContinue))) {
	set-alias buildWrapper "$msBuildPath"
}

if ($BUILD_PART -contains 'lib') {
	Write-Host Build dokan ...
	Exec-External { buildWrapper .\dokan.sln /p:Configuration=Release /p:Platform=Win32 /t:Build $CI_BUILD_ARG }
	Exec-External { buildWrapper .\dokan.sln /p:Configuration=Release /p:Platform=x64 /t:Build $CI_BUILD_ARG }
	Exec-External { buildWrapper .\dokan.sln /p:Configuration=Release /p:Platform=ARM /t:Build $CI_BUILD_ARG }
	Exec-External { buildWrapper .\dokan.sln /p:Configuration=Release /p:Platform=ARM64 /t:Build $CI_BUILD_ARG }

	Exec-External { buildWrapper .\dokan.sln /p:Configuration=Debug /p:Platform=Win32 /t:Build $CI_BUILD_ARG }
	Exec-External { buildWrapper .\dokan.sln /p:Configuration=Debug /p:Platform=x64 /t:Build $CI_BUILD_ARG }
	Exec-External { buildWrapper .\dokan.sln /p:Configuration=Debug /p:Platform=ARM /t:Build $CI_BUILD_ARG }
	Exec-External { buildWrapper .\dokan.sln /p:Configuration=Debug /p:Platform=ARM64 /t:Build $CI_BUILD_ARG }
	Write-Host Build dokan done !
}

if ($BUILD_PART -contains 'sys') {
	Write-Host Build dokan sys ...
	Exec-External { buildWrapper .\dokan.sln /p:Configuration="Win7 Release" /p:Platform=Win32 /t:Build $CI_BUILD_ARG }
	Exec-External { buildWrapper .\dokan.sln /p:Configuration="Win7 Release" /p:Platform=x64 /t:Build $CI_BUILD_ARG }
	Exec-External { buildWrapper .\dokan.sln /p:Configuration="Win8 Release" /p:Platform=Win32 /t:Build $CI_BUILD_ARG }
	Exec-External { buildWrapper .\dokan.sln /p:Configuration="Win8 Release" /p:Platform=x64 /t:Build $CI_BUILD_ARG }
	Exec-External { buildWrapper .\dokan.sln /p:Configuration="Win8 Release" /p:Platform=ARM /t:Build $CI_BUILD_ARG }
	Exec-External { buildWrapper .\dokan.sln /p:Configuration="Win8.1 Release" /p:Platform=Win32 /t:Build $CI_BUILD_ARG }
	Exec-External { buildWrapper .\dokan.sln /p:Configuration="Win8.1 Release" /p:Platform=x64 /t:Build $CI_BUILD_ARG }
	Exec-External { buildWrapper .\dokan.sln /p:Configuration="Win8.1 Release" /p:Platform=ARM /t:Build $CI_BUILD_ARG }
	Exec-External { buildWrapper .\dokan.sln /p:Configuration="Win10 Release" /p:Platform=Win32 /t:Build $CI_BUILD_ARG }
	Exec-External { buildWrapper .\dokan.sln /p:Configuration="Win10 Release" /p:Platform=x64 /t:Build $CI_BUILD_ARG }
	Exec-External { buildWrapper .\dokan.sln /p:Configuration="Win10 Release" /p:Platform=ARM /t:Build $CI_BUILD_ARG }
	Exec-External { buildWrapper .\dokan.sln /p:Configuration="Win10 Release" /p:Platform=ARM64 /t:Build $CI_BUILD_ARG }

	Exec-External { buildWrapper .\dokan.sln /p:Configuration="Win7 Debug" /p:Platform=Win32 /t:Build $CI_BUILD_ARG }
	Exec-External { buildWrapper .\dokan.sln /p:Configuration="Win7 Debug" /p:Platform=x64 /t:Build $CI_BUILD_ARG }
	Exec-External { buildWrapper .\dokan.sln /p:Configuration="Win8 Debug" /p:Platform=Win32 /t:Build $CI_BUILD_ARG }
	Exec-External { buildWrapper .\dokan.sln /p:Configuration="Win8 Debug" /p:Platform=x64 /t:Build $CI_BUILD_ARG }
	Exec-External { buildWrapper .\dokan.sln /p:Configuration="Win8 Debug" /p:Platform=ARM /t:Build $CI_BUILD_ARG }
	Exec-External { buildWrapper .\dokan.sln /p:Configuration="Win8.1 Debug" /p:Platform=Win32 /t:Build $CI_BUILD_ARG }
	Exec-External { buildWrapper .\dokan.sln /p:Configuration="Win8.1 Debug" /p:Platform=x64 /t:Build $CI_BUILD_ARG }
	Exec-External { buildWrapper .\dokan.sln /p:Configuration="Win8.1 Debug" /p:Platform=ARM /t:Build $CI_BUILD_ARG }
	Exec-External { buildWrapper .\dokan.sln /p:Configuration="Win10 Debug" /p:Platform=Win32 /t:Build $CI_BUILD_ARG }
	Exec-External { buildWrapper .\dokan.sln /p:Configuration="Win10 Debug" /p:Platform=x64 /t:Build $CI_BUILD_ARG }
	Exec-External { buildWrapper .\dokan.sln /p:Configuration="Win10 Debug" /p:Platform=ARM /t:Build $CI_BUILD_ARG }
	Exec-External { buildWrapper .\dokan.sln /p:Configuration="Win10 Debug" /p:Platform=ARM64 /t:Build $CI_BUILD_ARG }
	Write-Host Build dokan sys done !
}

if ($BUILD_PART -contains 'cygwin') {
	if (Test-Path -Path C:\cygwin64) {
		$ErrorActionPreference = "Continue" #cmake has normal stdout through stderr...
		Exec-External { ./dokan_fuse/build.ps1 }
		$ErrorActionPreference = "Stop"
	} else {
		Write-Host "Cygwin/Msys2 build disabled"
	}
}