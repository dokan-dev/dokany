<#
.SYNOPSIS
    Builds the file system and network provider test tools for CI: dokan_fstest.exe (tests/fstest), dokan_np_unit_test.exe / dokan_np_test.exe (tests/np), and fsx.exe (Liryna/fstools).

.DESCRIPTION
    Independent of the Dokan build, so CI runs it in its own job in parallel.
    Runs Tier 1 dokan_np unit tests (x64 + Win32) with JUnit XML output as a fast pre-VM gate.
    Stages:
        * $OutputDir\fstest: dokan_fstest.exe, baselines\ and license files
        * $OutputDir\np\x64\dokan_np_test.exe and $OutputDir\np\x86\dokan_np_test.exe
        * $OutputDir\fsx.exe
        * $NpUnitResultsDir: dokan_np_unit_test_{x64,x86}.xml
        * $ReleaseDir\dokan-fstest-<version>.zip (contents of $OutputDir\fstest)
#>
param(
    [Parameter(Mandatory = $true)]
    [int]$BuildNumber,
    [string]$OutputDir = 'ci-test-tools',
    [string]$ReleaseDir = 'ci-release-fstest',
    [string]$NpUnitResultsDir = 'ci-np-unit-results',
    [string]$FsTestDepsCacheDir = '',
    # Optional filename prefix for the staged release archive (e.g. 'pr-42-' on pull requests).
    [string]$ArtifactPrefix = ''
)

$ErrorActionPreference = 'Stop'

if ($ArtifactPrefix -notmatch '^[A-Za-z0-9_.-]*$') {
    throw "Invalid ArtifactPrefix '$ArtifactPrefix': only ASCII letters, digits, '_', '.', and '-' are allowed."
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

# 1b. Build tests/np (x64 and Win32), verify static CRT + Server 2016 imports, and run Tier 1 unit tests.
$npSourceDir = Join-Path $repoRoot 'tests\np'
if (-not (Test-Path -LiteralPath (Join-Path $npSourceDir 'CMakeLists.txt') -PathType Leaf)) {
    throw "tests/np/CMakeLists.txt not found at $npSourceDir"
}
$vsInfo = Resolve-DokanVisualStudioInfo
$cmakeExe = Resolve-DokanWindowsTool -ExeName 'cmake.exe' -Required -Candidates @(
    "${env:ProgramFiles}\CMake\bin\cmake.exe",
    "${env:ProgramFiles(x86)}\CMake\bin\cmake.exe",
    (Join-Path $vsInfo.Path 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe')
)
$ninjaExe = Resolve-DokanWindowsTool -ExeName 'ninja.exe' -Candidates @(
    (Join-Path $vsInfo.Path 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'),
    "${env:ProgramFiles}\Ninja\ninja.exe",
    'C:\ProgramData\chocolatey\bin\ninja.exe'
)

$npUnitAllowlist = @('KERNEL32.dll', 'ADVAPI32.dll', 'ntdll.dll', 'bcrypt.dll', 'bcryptprimitives.dll')
$npTestAllowlist = @('KERNEL32.dll', 'ADVAPI32.dll', 'MPR.dll', 'ntdll.dll', 'bcrypt.dll', 'bcryptprimitives.dll')
$npArchSpecs = @(
    @{ VsArch = 'amd64'; CMakeArch = 'x64'; Stage = 'x64' },
    @{ VsArch = 'x86'; CMakeArch = 'Win32'; Stage = 'x86' }
)
$npUnitBinaries = @{}
$npTestBinaries = @{}

foreach ($spec in $npArchSpecs) {
    $npBuildDir = Join-Path $npSourceDir "build-$($spec.Stage)"
    Invoke-DokanWithVsDevEnv -VsPath $vsInfo.Path -Arch $spec.VsArch -Action {
        $cmArgs = @('-S', $npSourceDir, '-B', $npBuildDir)
        if ($ninjaExe) {
            $cmArgs += @(
                '-G', 'Ninja',
                ("-DCMAKE_MAKE_PROGRAM=" + ($ninjaExe -replace '\\', '/')),
                '-DCMAKE_BUILD_TYPE=Release',
                '-DCMAKE_C_COMPILER=cl.exe',
                '-DCMAKE_CXX_COMPILER=cl.exe'
            )
        } else {
            $vsGen = Resolve-DokanVisualStudioGenerator -CMakeExe $cmakeExe -VsVersion $vsInfo.Version
            $cmArgs += @('-G', $vsGen, '-A', $spec.CMakeArch)
        }
        $cmArgs += @(
            '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>',
            '-Dgtest_force_shared_crt=OFF',
            '-DBUILD_GMOCK=OFF',
            '-DINSTALL_GTEST=OFF',
            '-DNP_TEST_WERROR=ON'
        )
        if (-not [string]::IsNullOrWhiteSpace($FsTestDepsCacheDir)) {
            $resolvedNpCache = Resolve-DokanTargetPath -BasePath $repoRoot -Path $FsTestDepsCacheDir
            $cmArgs += ("-DDOKAN_DEPS_CACHE_DIR=" + ($resolvedNpCache -replace '\\', '/'))
        }
        Write-Host "Configuring tests/np ($($spec.Stage))..."
        Exec-External { & $cmakeExe @cmArgs }
        Write-Host "Building tests/np ($($spec.Stage))..."
        Exec-External { & $cmakeExe --build $npBuildDir --config Release --parallel }

        $binDir = Join-Path $npBuildDir 'bin\Release'
        $unitExe = Join-Path $binDir 'dokan_np_unit_test.exe'
        $testExe = Join-Path $binDir 'dokan_np_test.exe'
        foreach ($bin in @($unitExe, $testExe)) {
            if (-not (Test-Path -LiteralPath $bin -PathType Leaf)) {
                throw "Expected tests/np binary missing: $bin"
            }
        }
        $unitDlls = Test-DokanStaticCrtImports -BinaryPath $unitExe -Allowlist $npUnitAllowlist
        Write-Host "Import gate passed ($unitExe): $($unitDlls -join ', ')"
        $testDlls = Test-DokanStaticCrtImports -BinaryPath $testExe -Allowlist $npTestAllowlist
        Write-Host "Import gate passed ($testExe): $($testDlls -join ', ')"
        $npUnitBinaries[$spec.Stage] = $unitExe
        $npTestBinaries[$spec.Stage] = $testExe
    }
}

$npResultsAbs = Resolve-DokanTargetPath -BasePath $repoRoot -Path $NpUnitResultsDir
Remove-Item -LiteralPath $npResultsAbs -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $npResultsAbs | Out-Null

$gflagsExe = Resolve-DokanWindowsTool -ExeName 'gflags.exe' -Candidates @(
    'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\gflags.exe',
    'C:\Program Files (x86)\Windows Kits\10\Debuggers\x86\gflags.exe'
)
$pageHeapEnabled = $false
if ($gflagsExe) {
    try {
        Write-Host "Enabling PageHeap for dokan_np_unit_test.exe via $gflagsExe..."
        & $gflagsExe /p /enable dokan_np_unit_test.exe /full | Out-Host
        if ($LASTEXITCODE -eq 0) {
            $pageHeapEnabled = $true
        } else {
            Write-Warning "gflags /p /enable returned exit code $LASTEXITCODE; continuing without PageHeap."
        }
    } catch {
        Write-Warning "Skipping PageHeap setup: $($_.Exception.Message)"
    }
}
try {
    foreach ($stageArch in @('x64', 'x86')) {
        $unitExe = $npUnitBinaries[$stageArch]
        $xmlOut = Join-Path $npResultsAbs "dokan_np_unit_test_${stageArch}.xml"
        Write-Host "Running dokan_np_unit_test ($stageArch)..."
        Exec-External { & $unitExe "--gtest_output=xml:$xmlOut" }
    }
} finally {
    if ($pageHeapEnabled -and $gflagsExe) {
        & $gflagsExe /p /disable dokan_np_unit_test.exe | Out-Null
    }
}

# 2. fsx.exe, so the test VMs do not need Visual Studio.
$fstoolsDir = Join-Path $repoRoot 'build\fstools'
New-Item -ItemType Directory -Force -Path $fstoolsDir | Out-Null
Push-Location -LiteralPath $fstoolsDir
try {
    if (-not (Test-Path -LiteralPath '.git')) {
        Exec-External { & git init -q }
    }
    Invoke-DokanWithRetry -Description "git fetch fstools $fstoolsRevision" -Action {
        Exec-External { & git fetch -q --depth 1 https://github.com/Liryna/fstools.git $fstoolsRevision }
    }
    Exec-External { & git checkout -q --detach FETCH_HEAD }
    Exec-External { & $msbuild '.\winfstest.sln' '/m' '/p:Configuration=Release' '/p:Platform=x64' }
} finally {
    Pop-Location
}

# 3. Stage the tools and the dokan-fstest release zip (same layout as the former AppVeyor artifact).
$outputAbs = Resolve-DokanTargetPath -BasePath $repoRoot -Path $OutputDir
$releaseAbs = Resolve-DokanTargetPath -BasePath $repoRoot -Path $ReleaseDir
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
$npStageX64 = Join-Path $outputAbs 'np\x64'
$npStageX86 = Join-Path $outputAbs 'np\x86'
New-Item -ItemType Directory -Force -Path $npStageX64, $npStageX86 | Out-Null
Copy-Item -LiteralPath $npTestBinaries['x64'] -Destination "$npStageX64\dokan_np_test.exe"
Copy-Item -LiteralPath $npTestBinaries['x86'] -Destination "$npStageX86\dokan_np_test.exe"
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
