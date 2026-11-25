param (
	[string[]]$BuildPart = @('win', 'cygwin', 'msys2'),
	[string[]]$Platforms = @('Win32', 'x64', 'ARM64'),
	[string[]]$Configurations = @('Release', 'Debug')
)

. .\scripts\build_helper.ps1

$ErrorActionPreference = "Stop"

Add-VisualStudio-Path

$msBuildPath=& Get-Command msbuild | Select-Object -ExpandProperty Definition
if (!([bool](Get-Command -Name buildWrapper -ErrorAction SilentlyContinue))) {
	set-alias buildWrapper "$msBuildPath"
}

if ($BuildPart -contains 'win') {
	if (!(Test-Path -Path '.\packages\Microsoft.Windows.WDK.x64.*') -and (Get-Command -Name nuget -ErrorAction SilentlyContinue)) {
		Write-Host "Restoring WDK NuGet packages for sys..."
		Exec-External { nuget restore .\sys\packages.config -PackagesDirectory .\packages }
	}
	foreach ($Configuration in $Configurations) {
		foreach ($Platform in $Platforms) {
			Write-Host Build dokan $Configuration $Platform ...
			Exec-External { buildWrapper .\dokan.sln /p:Configuration=$Configuration /p:Platform=$Platform /t:Build }
			Write-Host Build dokan $Configuration $Platform done !
		}
	}
}

$fuseTargets = @()
if ($BuildPart -contains 'cygwin') {
	$cygwinDir = if (-not [string]::IsNullOrWhiteSpace($env:CYGWIN_INST_DIR)) { $env:CYGWIN_INST_DIR } else { 'C:\cygwin64' }
	$cygwinBash = Join-Path $cygwinDir 'bin\bash.exe'
	if (Test-Path -LiteralPath $cygwinBash -PathType Leaf) {
		$fuseTargets += 'cygwin'
	} else {
		Write-Output "Cygwin build disabled ($cygwinBash not found)"
	}
}

if ($BuildPart -contains 'msys2') {
	$msys2Dir = if (-not [string]::IsNullOrWhiteSpace($env:MSYS2_INST_DIR)) { $env:MSYS2_INST_DIR } else { 'C:\msys64' }
	$msys2Bash = Join-Path $msys2Dir 'usr\bin\bash.exe'
	if (Test-Path -LiteralPath $msys2Bash -PathType Leaf) {
		$fuseTargets += 'msys2'
	} else {
		Write-Output "MSYS2 build disabled ($msys2Bash not found)"
	}
}

if ($fuseTargets.Count -gt 0) {
	$ErrorActionPreference = "Continue" #cmake writes some status output to stderr
	try {
		Exec-External { ./dokan_fuse/build.ps1 -Targets $fuseTargets }
	} finally {
		$ErrorActionPreference = "Stop"
	}
}