param(
    [Parameter(Mandatory=$false)][Array] $Mirrors = @("..\x64\Release\mirror.exe", "..\Win32\Release\mirror.exe")
)
#TODO: enable specifying own mirror-commands and own test-commands
#TODO: move compilation of test tools to separate scripts in scripts
#TODO: allow running from any directory by using the path this script is located in as reference

function Exec-External {
  param(
	[Parameter(Position=0,Mandatory=1)][scriptblock] $command
  )
  & $command
  if ($LASTEXITCODE -ne 0) {
	throw ("Command returned non-zero error-code ${LASTEXITCODE}: $command")
  }
}

$ifstest_user = "dokan_ifstest"
$ifstest_pass = "D0kan_1fstest"
# TODO: read password from command-line or file to keep dev-machines secure

$fsTestPath = "FSTMP"
$fsTestPath2 = "FSTMP2"

$DokanDriverLetter = "M"
$Commands = @{
	"/l $DokanDriverLetter" = "$($DokanDriverLetter):"
	"/l C:\DokanMount" = "C:\DokanMount"
	"/l $DokanDriverLetter /m" = "$($DokanDriverLetter):"
	"/l $DokanDriverLetter /o" = "$($DokanDriverLetter):"
	"/l $DokanDriverLetter /n" = "$($DokanDriverLetter):"
	"/l $DokanDriverLetter /n /u \myfs\dokan" = "\\myfs\dokan"
}

$ifstestParameters = @(
	"-t", "FileNameLengthTest",            # reason: buffer overflow in mirror. Issue #511
	"-t", "EndOfFileInformationTest",      # reason: IFSTest crashes 😲. Issue #546
	"-t", "NotificationSecurityTest",      # reason: IFSTest hangs 😞. Issue #547
	"-t", "NotificationCleanupAttribTest"  # reason: bothersome to wait for timeout. Issue #548
	"-t", "SimpleRenameInformationTest"    # reason: Issue #566
	"-t", "AVChangeLogTest"                # reason: Part of ChangeJournal
	"-t", "MountedDirtyTest"               # reason: Need a reboot to see the result
	"-t", "SetCompressionTest"             # reason: Compression is not enable on Mirror
	#Disable not supported features
	"-g", "ChangeJournal"
	"-g", "Virus"
	"-g", "DefragEnhancements"
	"-g", "Quotas"
	"-g", "Encryption"
	"-g", "ObjectId"
	"-g", "MountPoints"
	"-g", "ReparsePoints"
	"-g", "SparseFiles"
	"-p",                                  # Enable pagefile testing in CreatePagingFileTest
	"/v",                                  # verbose output
	"/d", "\Device\Dokan_1"                # Dokan device named need for FileSystemDeviceOpenTest
	"/r", "C:\$fsTestPath2"                # SimpleRenameInformationTest need an extra volum
	"/u", $ifstest_user,
	"/U", $ifstest_pass
)

$buildCmd = "C:\Program Files (x86)\MSBuild\14.0\Bin\MSBuild.exe"
$env:Path = $env:Path + ";C:\Program Files (x86)\Windows Kits\8.1\bin\x64\"
$env:CI_BUILD_ARG = ""
if ($env:APPVEYOR) { $env:CI_BUILD_ARG="/l:C:\Program Files\AppVeyor\BuildAgent\Appveyor.MSBuildLogger.dll" }
 
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
if (!(Test-Path "C:\$fsTestPath2")) { New-Item "C:\$fsTestPath2" -type directory | Out-Null }

add-type -AssemblyName System.Windows.Forms

foreach ($mirror in $Mirrors){
	$Commands.Keys | % {
		$command = $_
		$destination = $Commands.Item($_)
	
		Write-Host Test mirror $mirror with args $command with $destination as mount -ForegroundColor Green
		if ($destination.StartsWith("C:\")) {
			#Cleanup mount folder - Tag source folder to wait a not empty folder at mount
			New-Item -Force "C:\$fsTestPath\tmp" | Out-Null
			if (!(Test-Path $destination)) { New-Item $destination -type directory | Out-Null }
			Remove-Item -Recurse -Force "$($destination)\*"
		}
		
		$app = Start-Process -passthru $mirror -ArgumentList "/r C:\$fsTestPath $command"
		
		$count = 20;
		if ($destination.StartsWith("C:\")) {
			while (!(Test-Path "$($destination)\*") -and ($count -ne 0)) { Start-Sleep -m 250; $count -= 1 }
		} else {
			while (!(Test-Path $destination) -and ($count -ne 0)) { Start-Sleep -m 250 ; $count -= 1 }
		}
		
		if ($count -eq 0) {
			throw ("Impossible to mount for command $command")
		}

		Write-Host "Start FSX Test" -ForegroundColor Green
		Exec-External {& .\fstools\src\fsx\fsx.exe -N 5000 "$($destination)\fsxtest"}
		Write-Host "FSX Test finished" -ForegroundColor Green

		Write-Host "Start WinFSTest" -ForegroundColor Green
		Exec-External {& .\winfstest\TestSuite\run-winfstest.bat . "$($destination)\"}
		Write-Host "WinFSTest finished" -ForegroundColor Green

		if ($destination -match "[a-zA-Z]:") {
			Write-Host "Start IFSTest" -ForegroundColor Green
			Exec-External {& "..\scripts\run_ifstest.ps1" @ifstestParameters "$($destination)\"}
			Write-Host "IFSTest finished" -ForegroundColor Green
		} else {
			Write-Host "Skipping IFSTest, because it cannot be run against UNC-Paths" -ForegroundColor Green
		}

		[System.Windows.Forms.SendKeys]::SendWait("^{c}") 
		$app.WaitForExit()
		Write-Host Test mirror $mirror ended. -ForegroundColor Green
	}
}

