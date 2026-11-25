# Shared PowerShell 5.1+ helpers for running dokan_fstest against Dokan mounts.

# Single source of truth for the pinned Liryna/fstools commit used in CI.
# Keep in sync with $fstoolsRevision in samples/memfs_test.ps1 and samples/mirror_test.ps1.
$script:DokanFsToolsRevision = '9e378a7745a7dcb6fbfc1f649975e7a47c8f952e'

function Get-DokanFsToolsRevision {
    [CmdletBinding()]
    [OutputType([string])]
    param()

    return $script:DokanFsToolsRevision
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
                & $buildScript -Configuration Release | Out-Host
                if ($LASTEXITCODE -and $LASTEXITCODE -ne 0) {
                    throw "Command returned non-zero exit code ${LASTEXITCODE}: $buildScript -Configuration Release"
                }
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
