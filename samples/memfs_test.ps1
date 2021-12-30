param(
    [Parameter(Mandatory=$false)][Array] $Memfs = @("..\x64\Release\memfs.exe", "..\Win32\Release\memfs.exe")
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

$fsTestPath2 = "C:\FSTMP2"

$DokanDriverLetter = "M"
$Configs = @(
	@{
		"MemFSArguments" = "/l $DokanDriverLetter";
		"Destination" = "$($DokanDriverLetter):";
		"Name" = "drive";
	}
)

$ifstestParameters = @(
	"-t", "EndOfFileInformationTest",      # reason: IFSTest crashes 😲. Issue #546
	"-t", "SimpleRenameInformationTest",   # reason: Issue #566
	"-t", "AVChangeLogTest",               # reason: Part of ChangeJournal
	"-t", "MountedDirtyTest",              # reason: Need a reboot to see the result
	"-t", "SetCompressionTest",            # reason: Compression is not enable
	"-t", "FileOpenByIDTest",              # reason: FILE_OPEN_BY_FILE_ID not supported
	"-t", "OpenVolumeTest",                # reason: We do not have FCB for \ to count open
	"-t", "CaseSensitiveTest",             # reason: NTFS and CreateFile is not case sensitive by default
	"-t", "ShortFileNameTest",             # reason: shortname not supported
	"-t", "TunnelingTest",                 # reason: shortname not supported
	"-t", "CompressionInformationTest",    # reason: compression not supported
	"-t", "LinkInformationTest",           # reason: file link not supported
	"-t", "StreamStandardInformationTest", # reason: file link not supported
	"-t", "AlternateNameInformationTest",  # reason: alternate name not supported
	"-t", "HardLinkInformationTest",       # reason: hard link not supported
	"-t", "EaInformationTest",             # reason: extended file attributes not supported
	"-t", "FullDirectoryInformationTest",  # reason: Fail because extended attributes is incorrect
	"-t", "CreatePagingFileTest",          # reason: Paging files are not supported
	# Memfs not supported features
	"-t", "ArchiveAttributeTest"           # reason: We do not set Archive attr after close.
	"-g", "Security",                      # reason: ACL are not really supporting.
	"-g", "CloseCleanupDelete",            # reason: We do not update info during cleanup.
	# Features should work but have some issues needed to be debug
	"-t", "NotificationCleanupAttribTest",
	"-t", "NotificationSecurityTest",
	# Disable not supported features
	"-g", "ChangeJournal",
	"-g", "Virus",
	"-g", "DefragEnhancements",
	"-g", "Quotas",
	"-g", "Encryption",
	"-g", "ObjectId",
	"-g", "MountPoints",
	"-g", "ReparsePoints",
	"-g", "SparseFiles",
	"-g", "EaInformation",
	"-g", "FileSystemControlGeneral",      # reason: Retrieval Pointers fsctl not supported
	"/v",                                  # verbose output
	"/d", "\Device\Dokan_2",               # Dokan device named need for FileSystemDeviceOpenTest
	"/r", "$fsTestPath2",                  # SimpleRenameInformationTest need an extra volum
	"/u", $ifstest_user,
	"/U", $ifstest_pass
)

$fstoolsRevision="f010a68f96e004e8ef76436c0582e64216f400a0"
$winfstestRevision="98faa827b31d6e9144ff7c2c235f0b07b0ccf5d0"
$buildCmd = "C:\Program Files (x86)\MSBuild\14.0\Bin\MSBuild.exe"
$vswhere = "${Env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vswhere) {
	$path = & "${Env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
	if ($path) {
		$tmpBuildCmd = join-path $path 'MSBuild\Current\Bin\MSBuild.exe'
		if (test-path $tmpBuildCmd) {
			# Visual studio 2019
			$buildCmd = $tmpBuildCmd
			$fstoolsRevision="9e378a7745a7dcb6fbfc1f649975e7a47c8f952e"
			$winfstestRevision="d8b70ad91fae6681edeef15c35344ae280467600"
		}
	}
}

$env:Path = $env:Path + ";C:\Program Files (x86)\Windows Kits\8.1\bin\x64\"
$env:CI_BUILD_ARG = ""
if ($env:APPVEYOR) { $env:CI_BUILD_ARG="/l:C:\Program Files\AppVeyor\BuildAgent\Appveyor.MSBuildLogger.dll" }
 
Write-Host Build test tools -ForegroundColor Green
if (!(Test-Path .\fstools\src\fsx\fsx.exe))
{
	Exec-External {if (!(Test-Path fstools)) { git clone -q https://github.com/Liryna/fstools.git } }
	Exec-External {cd fstools; git reset --hard $fstoolsRevision; cd .. }
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
	Exec-External {cd winfstest; git reset --hard $winfstestRevision; cd .. }
	$buildArgs = @(
	".\winfstest\winfstest.sln",
	"/m",
	"/p:Configuration=Release",
	"/p:Platform=x64",
	"$env:CI_BUILD_ARG")
	Exec-External {& $buildCmd $buildArgs}
}
Write-Host Build test tools done. -ForegroundColor Green

if (!(Test-Path $fsTestPath2)) { New-Item $fsTestPath2 -type directory | Out-Null }

add-type -AssemblyName System.Windows.Forms

foreach ($mfs in $Memfs) {
	foreach ($Config in $Configs) {
		$command = $Config.Item("MemFSArguments")
		$destination = $Config.Item("Destination")

		# Cleanup second volume folder
		Remove-Item -Recurse -Force "$fsTestPath2\*" | Out-Null

		Write-Host Test memfs $mfs with args $command with $destination as mount -ForegroundColor Green
		
		$app = Start-Process -passthru $mfs -ArgumentList "$command"
		
		# When memfs finished mounting, Test-Path will return success.
		$count = 20;
		while (!(Test-Path "$($destination)\") -and ($count -ne 0)) { Start-Sleep -m 250; $count -= 1 }
		
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

		#TODO: can we use dokanctl to unmount?
		[System.Windows.Forms.SendKeys]::SendWait("^{c}") 
		$app.WaitForExit()
		Write-Host Test Memfs $mfs ended. -ForegroundColor Green
	}
}

