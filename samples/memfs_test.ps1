param(
    [Parameter(Mandatory=$false)][Array] $Memfs = @(),
    [Parameter(Mandatory=$false)][string] $FsTestExe = "",
    [Parameter(Mandatory=$false)][string] $BaselinesDir = "",
    [Parameter(Mandatory=$false)][string] $ResultsDir = "",
    [Parameter(Mandatory=$false)][ValidateSet("fail", "warn")][string] $XpassPolicy = "fail"
)
#TODO: enable specifying own mirror-commands and own test-commands

$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path -Path $repoRoot -ChildPath 'scripts\fstest_common.ps1')

if (-not $PSBoundParameters.ContainsKey('Memfs')) {
    $Memfs = @(
        (Join-Path -Path $repoRoot -ChildPath 'x64\Release\memfs.exe'),
        (Join-Path -Path $repoRoot -ChildPath 'Win32\Release\memfs.exe')
    )
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
		"Target" = "memfs_drive";
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
	"/d", "\Device\Dokan_3",               # Dokan device named need for FileSystemDeviceOpenTest
	"/r", "$fsTestPath2",                  # SimpleRenameInformationTest need an extra volum
	"/u", $ifstest_user,
	"/U", $ifstest_pass
)

Write-Output 'Build test tools'
$fsxExe = Initialize-DokanFsxTool -RepoRoot $repoRoot
$fsTestPaths = Resolve-DokanFsTestPaths `
	-RepoRoot $repoRoot `
	-BaselineFileName 'dokan_memfs.json' `
	-FsTestExe $FsTestExe `
	-BaselinesDir $BaselinesDir
$FsTestExe = $fsTestPaths.FsTestExe
$BaselinesDir = $fsTestPaths.BaselinesDir
$baselineFile = $fsTestPaths.BaselineFile
$runIfstestScript = Join-Path -Path $repoRoot -ChildPath 'scripts\run_ifstest.ps1'
Write-Output 'Build test tools done.'

if (!(Test-Path $fsTestPath2)) { New-Item $fsTestPath2 -type directory | Out-Null }

foreach ($mfs in $Memfs) {
	$archTag = if ($mfs -match '(?i)(^|[\\/])(x86|Win32)[\\/]') { 'x86' } elseif ($mfs -match '(?i)(^|[\\/])arm64[\\/]') { 'arm64' } else { 'x64' }
	foreach ($Config in $Configs) {
		$command = $Config.Item("MemFSArguments")
		$destination = $Config.Item("Destination")
		$targetName = $Config.Item("Target")
		$rootArg = if ($destination -match '^[a-zA-Z]:$') { "$destination\" } else { $destination }

		# Cleanup second volume folder
		Remove-Item -Recurse -Force "$fsTestPath2\*" | Out-Null

		Write-Output "Test memfs $mfs with args $command with $destination as mount"

		$app = Start-Process -passthru $mfs -ArgumentList "$command"
		try {
			# When memfs finished mounting, Test-Path will return success.
			$count = 20;
			while (!(Test-Path "$($destination)\") -and ($count -ne 0)) { Start-Sleep -m 250; $count -= 1 }

			if ($count -eq 0) {
				throw ("Impossible to mount for command $command")
			}

			Write-Output "Start FSX Test"
			Invoke-DokanExternal { & $fsxExe -N 5000 "$($destination)\fsxtest" }
			Write-Output "FSX Test finished"

			$fsTestArgs = @(
				"--root=$rootArg",
				"--baseline=$baselineFile",
				"--target=$targetName",
				"--target-arch=$archTag",
				"--xpass-policy=$XpassPolicy"
			)
			if (-not [string]::IsNullOrWhiteSpace($ResultsDir)) {
				New-Item -ItemType Directory -Force -Path $ResultsDir | Out-Null
				$jsonOut = Join-Path $ResultsDir "memfs_${archTag}_$($Config.Item('Name')).json"
				$fsTestArgs += "--json-out=$jsonOut"
			}

			Write-Output "Start Dokan FsTest ($targetName / $archTag)"
			Invoke-DokanExternal { & $FsTestExe @fsTestArgs }
			Write-Output "Dokan FsTest finished"

			if ($destination -match "[a-zA-Z]:") {
				Write-Output "Start IFSTest"
				Invoke-DokanExternal { & $runIfstestScript @ifstestParameters "$($destination)\" }
				Write-Output "IFSTest finished"
			} else {
				Write-Output "Skipping IFSTest, because it cannot be run against UNC-Paths"
			}
		} finally {
			Stop-DokanMountProcess `
				-Process $app `
				-Destination $destination `
				-DokanDriveLetter $DokanDriverLetter `
				-RepoRoot $repoRoot
		}
		Write-Output "Test Memfs $mfs ended."
	}
}

