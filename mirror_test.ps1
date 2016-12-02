function Exec-External {
  param(
	[Parameter(Position=0,Mandatory=1)][scriptblock] $command
  )
  & $command
  if ($LASTEXITCODE -ne 0) {
	throw ("Command returned non-zero error-code ${LASTEXITCODE}: $command")
  }
}

$fsTestPath = "FSTMP"
$Platforms = @("Win32", "x64")
$DokanDriverLetter = "M"
$Commands = @("/l $DokanDriverLetter", "/l $DokanDriverLetter /n", "/l $DokanDriverLetter /o")
$buildCmd = "C:\Program Files (x86)\MSBuild\14.0\Bin\MSBuild.exe"
$env:Path = $env:Path + ";C:\Program Files (x86)\Windows Kits\8.1\bin\x64\"
$env:CI_BUILD_ARG = ""
if ($env:APPVEYOR) { $env:CI_BUILD_ARG="/l:C:\Program Files\AppVeyor\BuildAgent\Appveyor.MSBuildLogger.dll" }

Write-Host Build mirrors -ForegroundColor Green
foreach ($Platform in $Platforms){
	$buildArgs = @(
	"/m",
	"/p:Configuration=Release",
	"/p:Platform=$Platform"
	"$env:CI_BUILD_ARG")

	Exec-External { & $buildCmd $buildArgs }
}
Write-Host Build mirrors done. -ForegroundColor Green
 
Write-Host Build test tools -ForegroundColor Green
if (!(Test-Path .\fstools\src\fsx\fsx.exe))
{
	Exec-External {if (!(Test-Path fstools)) { git clone -q https://github.com/Liryna/fstools.git } }
	Exec-External {cd fstools; git reset --hard f010a68f96e004e8ef76436c0582e64216f400a0; cd .. }
	$buildArgs = @(
	".\fstools\winfstest.sln",
	"/m",
	"/p:Configuration=Release",
	"/p:Platform=x64",
	"$env:CI_BUILD_ARG")
	Exec-External {& $buildCmd $buildArgs}
}

if (!(Test-Path .\winfstest\TestSuite\winfstest.exe))
{
	Exec-External {if (!(Test-Path winfstest)) { git clone -q https://github.com/Liryna/winfstest.git } }
	Exec-External {cd winfstest; git reset --hard 41ea2b555d9d9abc9ebbd58e6472e0ff703997bd; cd .. }
	$buildArgs = @(
	".\winfstest\winfstest.sln",
	"/m",
	"/p:Configuration=Release",
	"/p:Platform=x64",
	"$env:CI_BUILD_ARG")
	Exec-External {& $buildCmd $buildArgs}
}
Write-Host Build test tools done. -ForegroundColor Green

if (!(Test-Path "C:\$fsTestPath")) { New-Item "C:\$fsTestPath" -type directory | Out-Null }

add-type -AssemblyName System.Windows.Forms

foreach ($Platform in $Platforms){
	foreach ($Command in $Commands){
		Write-Host Test mirror $Platform with command $Command -ForegroundColor Green
		$app = Start-Process -passthru .\$Platform\Release\mirror.exe -ArgumentList "/r C:\$fsTestPath $Command"
		while (!(Test-Path "$DokanDriverLetter`:\")) { Start-Sleep -m 250 }

		Write-Host "Start FSX Test" -ForegroundColor Green
		Exec-External {& .\fstools\src\fsx\fsx.exe -N 5000 "$DokanDriverLetter`:\fsxtest"}
		Write-Host "FSX Test finished" -ForegroundColor Green

		Write-Host "Start WinFSTest" -ForegroundColor Green
		Exec-External {& .\winfstest\TestSuite\run-winfstest.bat . "$DokanDriverLetter`:\"}
		Write-Host "WinFSTest finished" -ForegroundColor Green

		[System.Windows.Forms.SendKeys]::SendWait("^{c}") 
		$app.WaitForExit()
		Write-Host Test mirror $Platform ended. -ForegroundColor Green
	}
}

