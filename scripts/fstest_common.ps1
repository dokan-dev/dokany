# Shared PowerShell 5.1+ helpers for running dokan_fstest against Dokan mounts.

. (Join-Path -Path $PSScriptRoot -ChildPath 'build_helper.ps1')

# Single source of truth for the pinned Liryna/fstools commit used by CI and
# sample test scripts (modern MSBuild Current).
$script:DokanFsToolsRevision = '9e378a7745a7dcb6fbfc1f649975e7a47c8f952e'

function Get-DokanFsToolsRevision {
    [CmdletBinding()]
    [OutputType([string])]
    param()

    return $script:DokanFsToolsRevision
}

function Initialize-DokanFsxTool {
    [CmdletBinding()]
    [OutputType([string])]
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot,
        [AllowEmptyString()]
        [string]$FsToolsDir = ''
    )

    if ([string]::IsNullOrWhiteSpace($FsToolsDir)) {
        # Match scripts/ci/vm/run_vm_tests.sh and legacy samples/ CWD layout.
        $FsToolsDir = Join-Path -Path $RepoRoot -ChildPath 'samples\fstools'
    }
    $fsxExe = Join-Path -Path $FsToolsDir -ChildPath 'src\fsx\fsx.exe'

    if (-not (Test-Path -LiteralPath $fsxExe -PathType Leaf)) {
        $fstoolsRevision = 'f010a68f96e004e8ef76436c0582e64216f400a0'
        $buildCmd = 'C:\Program Files (x86)\MSBuild\14.0\Bin\MSBuild.exe'
        $vs = Find-DokanVisualStudio -Requires 'Microsoft.Component.MSBuild' -Latest | Select-Object -First 1
        if ($vs) {
            $tmpBuildCmd = Join-Path -Path $vs.Path -ChildPath 'MSBuild\Current\Bin\MSBuild.exe'
            if (Test-Path -LiteralPath $tmpBuildCmd -PathType Leaf) {
                $buildCmd = $tmpBuildCmd
                $fstoolsRevision = Get-DokanFsToolsRevision
            }
        }

        if (-not (Test-Path -LiteralPath $FsToolsDir -PathType Container)) {
            Invoke-DokanExternal { & git clone -q https://github.com/Liryna/fstools.git $FsToolsDir } | Out-Host
        }
        Push-Location -LiteralPath $FsToolsDir
        try {
            Invoke-DokanExternal { & git reset --hard $fstoolsRevision } | Out-Host
        } finally {
            Pop-Location
        }

        $slnPath = Join-Path -Path $FsToolsDir -ChildPath 'winfstest.sln'
        $buildArgs = @(
            $slnPath,
            '/m',
            '/p:Configuration=Release',
            '/p:Platform=x64'
        )
        Invoke-DokanExternal { & $buildCmd @buildArgs } | Out-Host
    }

    if (-not (Test-Path -LiteralPath $fsxExe -PathType Leaf)) {
        throw "fsx.exe not found at $fsxExe"
    }

    return $fsxExe
}

function Resolve-DokanFsTestPaths {
    [CmdletBinding()]
    [OutputType([hashtable])]
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot,
        [Parameter(Mandatory = $true)]
        [string]$BaselineFileName,
        [AllowEmptyString()]
        [string]$FsTestExe = '',
        [AllowEmptyString()]
        [string]$BaselinesDir = ''
    )

    if ([string]::IsNullOrWhiteSpace($FsTestExe)) {
        $candidates = @(
            (Join-Path -Path $RepoRoot -ChildPath 'tests\fstest\ci-bin\dokan_fstest.exe'),
            (Join-Path -Path $RepoRoot -ChildPath 'tests\fstest\build\bin\Release\dokan_fstest.exe'),
            (Join-Path -Path $RepoRoot -ChildPath 'tests\fstest\build\Release\dokan_fstest.exe'),
            (Join-Path -Path $RepoRoot -ChildPath 'tests\fstest\build\dokan_fstest.exe')
        )
        foreach ($candidate in $candidates) {
            if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                $FsTestExe = $candidate
                break
            }
        }
        if ([string]::IsNullOrWhiteSpace($FsTestExe)) {
            if ($env:CI) {
                throw 'dokan_fstest.exe not found in CI; pass -FsTestExe (or stage it at tests\fstest\ci-bin\dokan_fstest.exe)'
            }
            Write-Information -MessageData 'Building dokan_fstest.exe via scripts\build_fstest.ps1...' -InformationAction Continue
            $buildScript = Join-Path -Path $RepoRoot -ChildPath 'scripts\build_fstest.ps1'
            Push-Location -LiteralPath $RepoRoot
            try {
                Invoke-DokanExternal { & $buildScript -Configuration Release } | Out-Host
            } finally {
                Pop-Location
            }
            $FsTestExe = Join-Path -Path $RepoRoot -ChildPath 'tests\fstest\build\bin\Release\dokan_fstest.exe'
        }
    }
    if (-not (Test-Path -LiteralPath $FsTestExe -PathType Leaf)) {
        throw "dokan_fstest.exe not found at $FsTestExe"
    }

    if ([string]::IsNullOrWhiteSpace($BaselinesDir)) {
        $exeDir = Split-Path -Path $FsTestExe -Parent
        $adjacentBaselines = if ($exeDir) { Join-Path -Path $exeDir -ChildPath 'baselines' } else { '' }
        if ($adjacentBaselines -and (Test-Path -LiteralPath $adjacentBaselines -PathType Container)) {
            $BaselinesDir = $adjacentBaselines
        } else {
            $BaselinesDir = Join-Path -Path $RepoRoot -ChildPath 'tests\fstest\baselines'
        }
    }

    $baselineFile = Join-Path -Path $BaselinesDir -ChildPath $BaselineFileName
    if (-not (Test-Path -LiteralPath $baselineFile -PathType Leaf)) {
        throw "Baseline manifest not found at $baselineFile"
    }

    return @{
        FsTestExe    = $FsTestExe
        BaselinesDir = $BaselinesDir
        BaselineFile = $baselineFile
    }
}

function Stop-DokanMountProcess {
    [CmdletBinding(SupportsShouldProcess = $true)]
    param(
        [AllowNull()]
        [System.Diagnostics.Process]$Process,
        [Parameter(Mandatory = $true)]
        [string]$Destination,
        [Parameter(Mandatory = $true)]
        [string]$DokanDriveLetter,
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot
    )

    if (-not $Process -or $Process.HasExited) {
        return
    }
    if (-not $PSCmdlet.ShouldProcess("PID $($Process.Id) ($Destination)", 'Stop Dokan mount process')) {
        return
    }

    $dokanCtl = if ($env:DokanLibrary3) {
        Join-Path -Path $env:DokanLibrary3 -ChildPath 'dokanctl.exe'
    } else {
        Join-Path -Path $RepoRoot -ChildPath 'x64\Release\dokanctl.exe'
    }
    $unmounted = $false
    if ($dokanCtl -and (Test-Path -LiteralPath $dokanCtl -PathType Leaf)) {
        $unmountTarget = if ($Destination -like '\\*') { "$($DokanDriveLetter):" } else { $Destination }
        $ctlProc = Start-Process -FilePath $dokanCtl -ArgumentList @('/u', $unmountTarget) -WindowStyle Hidden -PassThru
        if ($ctlProc -and -not $ctlProc.WaitForExit(10000)) {
            Stop-Process -Id $ctlProc.Id -Force -ErrorAction SilentlyContinue
        }
        $unmounted = $Process.WaitForExit(10000)
    } else {
        Write-Warning "dokanctl.exe not found at '$dokanCtl'; falling back to direct termination of mount process PID $($Process.Id)."
    }

    if (-not $unmounted -and -not $Process.HasExited) {
        if ($Process.CloseMainWindow()) {
            $unmounted = $Process.WaitForExit(4000)
        }
    }

    if (-not $unmounted -and -not $Process.HasExited) {
        Write-Warning "Force-stopping mount process $($Process.Id) after unmount timeout."
        Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
        $Process.WaitForExit(5000) | Out-Null
    }
}

function Add-DokanDefenderExclusion {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if (-not (Get-Command -Name Add-MpPreference -ErrorAction SilentlyContinue)) {
        return
    }
    try {
        Add-MpPreference -ExclusionPath $Path -ErrorAction Stop
    } catch {
        Write-Warning "Windows Defender exclusion skipped for ${Path}: $($_.Exception.Message)"
    }
}

function Invoke-DokanFsTestVhdxVolumeRoot {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$FsTestExe,
        [Parameter(Mandatory = $true)]
        [string]$JsonOut,
        [AllowEmptyString()]
        [string]$TempDir = ''
    )

    # Volume-root-only tests skip under a subdirectory root, so run them against
    # the root of a throwaway NTFS VHDX: 6 MountRoot*, DotDotTraversalClamps-
    # AtVolumeRoot, and 3 CrossVolumeMove* (temp dir lives on another volume).
    $gtestFilter = '*MountRoot*:*DotDotTraversalClampsAtVolumeRoot*:*CrossVolumeMove*'
    $minPassed = 10

    if ([string]::IsNullOrWhiteSpace($TempDir)) {
        $TempDir = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [System.IO.Path]::GetTempPath() }
    }
    $usedLetters = @([System.IO.DriveInfo]::GetDrives() | ForEach-Object { $_.Name.Substring(0, 1).ToUpperInvariant() })
    $letter = @('V', 'W', 'X', 'Y', 'Z', 'T', 'U') |
        Where-Object { $usedLetters -notcontains $_ } |
        Select-Object -First 1
    if (-not $letter) {
        throw 'No free drive letter available for the volume-root VHDX.'
    }
    $volRoot = "${letter}:\"

    $vhdxPath = Join-Path -Path $TempDir -ChildPath 'fstest_root.vhdx'
    $createScript = Join-Path -Path $TempDir -ChildPath 'fstest_vhdx_create.txt'
    $detachScript = Join-Path -Path $TempDir -ChildPath 'fstest_vhdx_detach.txt'
    if (Test-Path -LiteralPath $vhdxPath) {
        Remove-Item -LiteralPath $vhdxPath -Force
    }
    @(
        "create vdisk file=`"$vhdxPath`" maximum=256 type=expandable",
        "select vdisk file=`"$vhdxPath`"",
        'attach vdisk',
        'create partition primary',
        'format fs=ntfs quick label=FSTESTROOT',
        "assign letter=$letter"
    ) | Set-Content -LiteralPath $createScript -Encoding ASCII
    @(
        "select vdisk file=`"$vhdxPath`"",
        'detach vdisk'
    ) | Set-Content -LiteralPath $detachScript -Encoding ASCII

    try {
        Invoke-DokanExternal { & diskpart.exe /s $createScript } | Out-Host

        # Mount manager letter assignment can lag diskpart's return.
        $deadline = (Get-Date).AddSeconds(5)
        while (-not [System.IO.Directory]::Exists($volRoot) -and (Get-Date) -lt $deadline) {
            Start-Sleep -Milliseconds 250
        }
        if (-not [System.IO.Directory]::Exists($volRoot)) {
            throw "VHDX volume $volRoot is not accessible after diskpart."
        }
        Add-DokanDefenderExclusion -Path $volRoot

        & $FsTestExe "--root=$volRoot" '--strict' "--gtest_filter=$gtestFilter" "--json-out=$JsonOut" | Out-Host
        if ($LASTEXITCODE -ne 0) {
            throw "dokan_fstest --strict (volume root $volRoot) failed with exit code $LASTEXITCODE"
        }

        # Guard against a mis-detected volume root silently skipping every
        # volume-root test while the run still exits 0.
        $card = Get-Content -LiteralPath $JsonOut -Raw | ConvertFrom-Json
        if (-not $card.capabilities.is_volume_root -or $card.summary.skip -ne 0 -or $card.summary.pass -lt $minPassed) {
            throw "Volume-root VHDX run did not exercise volume-root tests (is_volume_root=$($card.capabilities.is_volume_root), pass=$($card.summary.pass), skip=$($card.summary.skip); expected is_volume_root=True, pass>=$minPassed, skip=0)"
        }
    } finally {
        if (Test-Path -LiteralPath $vhdxPath) {
            & diskpart.exe /s $detachScript | Out-Host
            if ($LASTEXITCODE -ne 0) {
                Write-Warning "diskpart VHDX detach failed with exit code $LASTEXITCODE"
            }
            # A best-effort detach failure must not fail the caller's step.
            $global:LASTEXITCODE = 0
            Remove-Item -LiteralPath $vhdxPath -Force -ErrorAction SilentlyContinue
        }
        Remove-Item -LiteralPath $createScript, $detachScript -Force -ErrorAction SilentlyContinue
    }
}
