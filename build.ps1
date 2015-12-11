param(
	[Parameter(Mandatory = $false)] [switch] $d,
	[Parameter(Mandatory = $false)] [string] $vsver = '14.0'
)

if($d -eq $true)
{
	$configuration = 'Debug'
}
else
{
	$configuration = 'Release'
}

# we need to use the 32-bit version of msbuild

$hasMSBuild = Test-Path "HKLM:\SOFTWARE\Wow6432Node\Microsoft\MSBuild\ToolsVersions\$vsver"

if($hasMSBuild -eq $false)
{
	$hasMSBuild = Test-Path "HKLM:\SOFTWARE\Microsoft\MSBuild\ToolsVersions\$vsver"

	if($hasMSBuild -eq $false)
	{
		Write-Host "Could not locate MSBuild $vsver"
		exit 1
	}
	else
	{
		$msbuild = Get-ItemProperty -Path "HKLM:\SOFTWARE\Microsoft\MSBuild\ToolsVersions\$vsver" -Name MSBuildToolsPath
	}
}
else
{
	$msbuild = Get-ItemProperty -Path "HKLM:\SOFTWARE\Wow6432Node\Microsoft\MSBuild\ToolsVersions\$vsver" -Name MSBuildToolsPath
}

$hasVS = Test-Path "HKLM:\SOFTWARE\Wow6432Node\Microsoft\VisualStudio\$vsver"

if($hasVS -eq $false)
{
	$hasVS = Test-Path "HKLM:\SOFTWARE\Microsoft\VisualStudio\$vsver"

	if($hasVS -eq $false)
	{
		Write-Host "Could not locate Visual Studio $vsver"
		exit 1
	}
	else
	{
		$vspath = Get-ItemProperty -Path "HKLM:\SOFTWARE\Microsoft\VisualStudio\$vsver" -Name ShellFolder
	}
}
else
{
	$vspath = Get-ItemProperty -Path "HKLM:\SOFTWARE\Wow6432Node\Microsoft\VisualStudio\$vsver" -Name ShellFolder
}

# & "$($vspath.ShellFolder)VC\vcvarsall.bat"
# http://stackoverflow.com/questions/2124753/how-i-can-use-powershell-with-the-visual-studio-command-prompt

pushd "$($vspath.ShellFolder)VC"

cmd /c "vcvarsall.bat&set" |
foreach {
	if ($_ -match "(.*?)=(.*)") {
		Set-Item -force -path "ENV:\$($matches[1])" -value "$($matches[2])"
	}
}

popd

Write-Host "Environment for Visual C++ $vsver has been setup." -ForegroundColor Yellow

& "$($msbuild.MSBuildToolsPath)MSBuild.exe" BuildAll.target /p:Configuration=$configuration