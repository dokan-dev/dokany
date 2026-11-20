function Exec-External {
  param(
	[Parameter(Position=0,Mandatory=1)][scriptblock] $command
  )
  & $command
  if ($LASTEXITCODE -ne 0) {
	throw ("Command returned non-zero error-code ${LASTEXITCODE}: $command")
  }
}

function Add-VisualStudio-Path {
	$vswhere = "${Env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
	if (!(Test-Path -Path $vswhere)) {
		throw ("Visual Studio Installer (vswhere.exe) NOT found.")
	}
	$vsPath = (& $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath) | Select-Object -First 1
	$msBuild_VSPath = "$vsPath\MSBuild\Current\Bin\amd64"
	if (!(Test-Path -Path $msBuild_VSPath)) {
		$msBuild_VSPath = "$vsPath\MSBuild\Current\Bin"
	}
	
	if ([string]::IsNullOrEmpty($vsPath) -or !(Test-Path -Path $msBuild_VSPath)) {
		throw ("Visual Studio with MSBuild NOT Installed.")
	}
	
	$env:Path = "$msBuild_VSPath;$env:Path"
}