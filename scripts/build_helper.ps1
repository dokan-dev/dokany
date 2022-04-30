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
	$vsPath = (& "${Env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -property installationPath).Split([Environment]::NewLine) | Select -First 1
	$msBuild_VSPath = "$vsPath\MSBuild\Current\Bin"
	
	if (!(Test-Path -Path $msBuild_VSPath)) {
		throw ("Visual C++ 2019 NOT Installed.")
	}
	
	$env:Path += ";$msBuild_VSPath";
}