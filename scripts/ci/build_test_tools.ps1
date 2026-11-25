<#
.SYNOPSIS
    Builds the file system test tools for CI: dokan_fstest.exe (tests/fstest) and fsx.exe (Liryna/fstools).

.DESCRIPTION
    Independent of the Dokan build, so CI runs it in its own job in parallel.
    Stages:
        * $OutputDir\fstest: dokan_fstest.exe, baselines\ and license files
        * $OutputDir\fsx.exe
        * $ReleaseDir\dokan-fstest-<version>.zip (contents of $OutputDir\fstest)
#>
param(
    [Parameter(Mandatory = $true)]
    [int]$BuildNumber,
    [string]$OutputDir = 'ci-test-tools',
    [string]$ReleaseDir = 'ci-release-fstest',
    [string]$FsTestDepsCacheDir = '',
    # Optional filename prefix for the staged release archive (e.g. 'pr-42-' on pull requests).
    [string]$ArtifactPrefix = ''
)

$ErrorActionPreference = 'Stop'

if ($ArtifactPrefix -notmatch '^[A-Za-z0-9_.-]*$') {
    throw "Invalid ArtifactPrefix '$ArtifactPrefix': only ASCII letters, digits, '_', '.', and '-' are allowed."
}

function Resolve-TargetPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$BasePath,
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return $Path
    }
    return (Join-Path -Path $BasePath -ChildPath $Path)
}

function Invoke-WithRetry {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Description,
        [Parameter(Mandatory = $true)]
        [scriptblock]$Action,
        [int]$MaxAttempts = 3,
        [int]$DelaySeconds = 10
    )

    for ($attempt = 1; $attempt -le $MaxAttempts; $attempt++) {
        try {
            & $Action
            return
        } catch {
            if ($attempt -ge $MaxAttempts) {
                throw
            }
            Write-Warning "$Description failed (attempt $attempt/$MaxAttempts): $($_.Exception.Message). Retrying in ${DelaySeconds}s..."
            Start-Sleep -Seconds $DelaySeconds
        }
    }
}

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $repoRoot

. .\scripts\build_helper.ps1
. .\scripts\fstest_common.ps1
Add-VisualStudio-Path
$msbuild = (Get-Command msbuild.exe -CommandType Application | Select-Object -First 1).Source

$sevenZip = (Get-Command 7z.exe -ErrorAction SilentlyContinue | Select-Object -First 1).Source
if (-not $sevenZip) {
    $sevenZip = 'C:\Program Files\7-Zip\7z.exe'
}
if (-not (Test-Path -LiteralPath $sevenZip -PathType Leaf)) {
    throw "7z.exe not found at $sevenZip"
}

$fstoolsRevision = Get-DokanFsToolsRevision
$version = (& "$PSScriptRoot\get_version.ps1" -BuildNumber $BuildNumber | Select-Object -First 1).ToString().Trim()
if ($version -notmatch '^\d+\.\d+\.\d+\.\d+$') {
    throw "Invalid version '$version' returned by get_version.ps1"
}

# 1. dokan_fstest.exe: static CRT, Server 2016 import gate, self-tests.
$fsTestArgs = @{ Configuration = 'Release'; VerifyStaticCrt = $true }
if (-not [string]::IsNullOrWhiteSpace($FsTestDepsCacheDir)) {
    $fsTestArgs['CacheDir'] = $FsTestDepsCacheDir
}
& .\scripts\build_fstest.ps1 @fsTestArgs
if ($LASTEXITCODE -and $LASTEXITCODE -ne 0) {
    throw "scripts\build_fstest.ps1 failed with exit code $LASTEXITCODE"
}

# 2. fsx.exe, so the test VMs do not need Visual Studio.
$fstoolsDir = Join-Path $repoRoot 'build\fstools'
New-Item -ItemType Directory -Force -Path $fstoolsDir | Out-Null
Push-Location -LiteralPath $fstoolsDir
try {
    if (-not (Test-Path -LiteralPath '.git')) {
        Exec-External { & git init -q }
    }
    Invoke-WithRetry -Description "git fetch fstools $fstoolsRevision" -Action {
        Exec-External { & git fetch -q --depth 1 https://github.com/Liryna/fstools.git $fstoolsRevision }
    }
    Exec-External { & git checkout -q --detach FETCH_HEAD }
    Exec-External { & $msbuild '.\winfstest.sln' '/m' '/p:Configuration=Release' '/p:Platform=x64' }
} finally {
    Pop-Location
}

# 3. Stage the tools and the dokan-fstest release zip (same layout as the former AppVeyor artifact).
$outputAbs = Resolve-TargetPath -BasePath $repoRoot -Path $OutputDir
$releaseAbs = Resolve-TargetPath -BasePath $repoRoot -Path $ReleaseDir
foreach ($dir in @($outputAbs, $releaseAbs)) {
    Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
}
$fsTestStage = Join-Path $outputAbs 'fstest'
New-Item -ItemType Directory -Force -Path $fsTestStage | Out-Null
Copy-Item -LiteralPath "$repoRoot\tests\fstest\build\bin\Release\dokan_fstest.exe" -Destination $fsTestStage
Copy-Item -LiteralPath "$repoRoot\tests\fstest\baselines" -Destination $fsTestStage -Recurse
Copy-Item -LiteralPath "$repoRoot\tests\fstest\LICENSE" -Destination "$fsTestStage\LICENSE"
Copy-Item -LiteralPath "$repoRoot\tests\fstest\build\_deps\googletest-src\LICENSE" -Destination "$fsTestStage\LICENSE-googletest.txt"
Copy-Item -LiteralPath "$repoRoot\tests\fstest\LICENSE-nlohmann-json.txt" -Destination "$fsTestStage\LICENSE-nlohmann-json.txt"
Copy-Item -LiteralPath "$fstoolsDir\src\fsx\fsx.exe" -Destination "$outputAbs\fsx.exe"

$releaseZip = Join-Path $releaseAbs "${ArtifactPrefix}dokan-fstest-$version.zip"
Exec-External { & $sevenZip a -tzip $releaseZip "$fsTestStage\*" | Out-Null }

# 4. Round-trip verify the release archive contents.
$tempRoot = if (-not [string]::IsNullOrWhiteSpace($env:RUNNER_TEMP)) {
    $env:RUNNER_TEMP
} else {
    [System.IO.Path]::GetTempPath()
}
$verifyDir = Join-Path $tempRoot "verify-dokan-fstest-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Force -Path $verifyDir | Out-Null
try {
    Exec-External { & $sevenZip x -y "-o$verifyDir" -- $releaseZip | Out-Null }
    $expectedFiles = @(
        'dokan_fstest.exe',
        'baselines\dokan_memfs.json',
        'baselines\dokan_mirror.json',
        'LICENSE',
        'LICENSE-googletest.txt',
        'LICENSE-nlohmann-json.txt'
    )
    foreach ($relPath in $expectedFiles) {
        $extractedPath = Join-Path $verifyDir $relPath
        if (-not (Test-Path -LiteralPath $extractedPath -PathType Leaf)) {
            throw "Archive verification failed: '$relPath' missing from $releaseZip"
        }
    }
} finally {
    Remove-Item -LiteralPath $verifyDir -Recurse -Force -ErrorAction SilentlyContinue
}

if ($env:GITHUB_OUTPUT) {
    Add-Content -Path $env:GITHUB_OUTPUT -Value "version=$version"
}
Get-ChildItem -LiteralPath $outputAbs, $releaseAbs -Recurse -File | Format-Table FullName, Length
