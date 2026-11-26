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
