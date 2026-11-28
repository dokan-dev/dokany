[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [string]$BuildDir = 'tests\fstest\build',
    [string]$CacheDir = '',
    [string]$GTestSourceDir = '',
    [string]$JsonSourceDir = '',
    [ValidateSet('msvc', 'clang-cl')]
    [string]$Compiler = 'msvc',
    [switch]$VerifyStaticCrt,
    [switch]$SkipSelfTest,
    [switch]$SkipCli,
    [switch]$WarningsAsErrors,
    [string[]]$CMakeExtraArgs = @()
)

$ErrorActionPreference = 'Stop'

if ($VerifyStaticCrt -and $SkipCli) {
    throw '-VerifyStaticCrt cannot be combined with -SkipCli because static libraries (.lib) do not have a PE import table to verify via dumpbin /imports.'
}

$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path -Path $PSScriptRoot -ChildPath 'build_helper.ps1')
$sourceDir = Join-Path $repoRoot 'tests\fstest'
$cmakeListsPath = Join-Path $sourceDir 'CMakeLists.txt'
if (-not (Test-Path -LiteralPath $cmakeListsPath -PathType Leaf)) {
    throw "tests/fstest/CMakeLists.txt not found at $sourceDir"
}

$depsJsonPath = Join-Path $sourceDir 'deps.json'
if (-not (Test-Path -LiteralPath $depsJsonPath -PathType Leaf)) {
    throw "tests/fstest/deps.json not found at $sourceDir"
}

function Get-DependencyPins {
    param([Parameter(Mandatory = $true)][string]$DepsJsonPath)

    $pins = [System.IO.File]::ReadAllText($DepsJsonPath) | ConvertFrom-Json
    foreach ($depName in @('googletest', 'nlohmann_json')) {
        $dep = $pins.$depName
        if ($null -eq $dep) {
            throw "Missing '$depName' entry in $DepsJsonPath"
        }
        foreach ($field in @('version', 'url', 'filename', 'sha256')) {
            if ([string]::IsNullOrWhiteSpace([string]$dep.$field)) {
                throw "Missing '$depName.$field' in $DepsJsonPath"
            }
        }
    }

    return @{
        GTestVersion  = [string]$pins.googletest.version
        GTestUrl      = [string]$pins.googletest.url
        GTestFileName = [string]$pins.googletest.filename
        GTestSha256   = ([string]$pins.googletest.sha256).ToLowerInvariant()
        JsonVersion   = [string]$pins.nlohmann_json.version
        JsonUrl       = [string]$pins.nlohmann_json.url
        JsonFileName  = [string]$pins.nlohmann_json.filename
        JsonSha256    = ([string]$pins.nlohmann_json.sha256).ToLowerInvariant()
    }
}

$depPins = Get-DependencyPins -DepsJsonPath $depsJsonPath
$script:GTestVersion = $depPins.GTestVersion
$script:GTestUrl = $depPins.GTestUrl
$script:GTestFileName = $depPins.GTestFileName
$script:GTestSha256 = $depPins.GTestSha256

$script:JsonVersion = $depPins.JsonVersion
$script:JsonUrl = $depPins.JsonUrl
$script:JsonFileName = $depPins.JsonFileName
$script:JsonSha256 = $depPins.JsonSha256

function Resolve-CallerPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    return $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
}

function Resolve-WindowsCMake {
    param([string]$VsPath = '')
    $candidates = @(
        "${env:ProgramFiles}\CMake\bin\cmake.exe",
        "${env:ProgramFiles(x86)}\CMake\bin\cmake.exe"
    )
    if (-not [string]::IsNullOrWhiteSpace($VsPath)) {
        $candidates += Join-Path $VsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    }
    foreach ($vs in @(Find-DokanVisualStudio)) {
        $candidates += Join-Path $vs.Path 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    }
    return (Resolve-DokanWindowsTool -ExeName 'cmake.exe' -Candidates $candidates -Required)
}

function Resolve-WindowsNinja {
    param([string]$VsPath = '')
    $candidates = @()
    if (-not [string]::IsNullOrWhiteSpace($VsPath)) {
        $candidates += Join-Path $VsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
    }
    $candidates += @(
        "${env:ProgramFiles}\Ninja\ninja.exe",
        'C:\ProgramData\chocolatey\bin\ninja.exe'
    )
    return (Resolve-DokanWindowsTool -ExeName 'ninja.exe' -Candidates $candidates)
}

function Resolve-ClangCl {
    param([string]$VsPath = '')
    $excludedPrefixes = Get-DokanExcludedUnixPrefixes

    # Prefer the Visual Studio bundled LLVM toolchain so clang-cl matches the
    # active MSVC STL headers (avoiding STL1000 compiler version mismatches).
    $candidates = @()
    if (-not [string]::IsNullOrWhiteSpace($VsPath)) {
        $candidates += Join-Path $VsPath 'VC\Tools\Llvm\x64\bin\clang-cl.exe'
    }
    foreach ($cmd in @(Get-Command clang-cl.exe -All -CommandType Application -ErrorAction SilentlyContinue)) {
        if (-not $cmd.Source) { continue }
        $fullSource = [System.IO.Path]::GetFullPath($cmd.Source)
        if (-not (Test-DokanExcludedPath -FullPath $fullSource -ExcludedPrefixes $excludedPrefixes)) {
            $candidates += $fullSource
        }
    }
    $candidates += @(
        "${env:ProgramFiles}\LLVM\bin\clang-cl.exe",
        "${env:ProgramFiles(x86)}\LLVM\bin\clang-cl.exe"
    )
    foreach ($candidate in ($candidates | Select-Object -Unique)) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return [System.IO.Path]::GetFullPath($candidate)
        }
    }
    throw 'clang-cl.exe not found on PATH, under Visual Studio LLVM tools, or under Program Files\LLVM.'
}

function Get-CachedDependency {
    param(
        [Parameter(Mandatory = $true)][string]$CacheDirectory,
        [Parameter(Mandatory = $true)][string]$Url,
        [Parameter(Mandatory = $true)][string]$FileName,
        [Parameter(Mandatory = $true)][string]$ExpectedSha256
    )

    New-Item -ItemType Directory -Force -Path $CacheDirectory | Out-Null
    $destPath = Join-Path $CacheDirectory $FileName
    $expectedLower = $ExpectedSha256.ToLowerInvariant()

    if (Test-Path -LiteralPath $destPath -PathType Leaf) {
        $existingHash = (Get-FileHash -LiteralPath $destPath -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($existingHash -eq $expectedLower) {
            return @{
                Path     = $destPath
                CMakeUrl = ($destPath -replace '\\', '/')
                CacheHit = $true
            }
        }
        Write-Warning "Cached $FileName failed SHA256 check (got $existingHash, expected $expectedLower); re-downloading."
        Remove-Item -LiteralPath $destPath -Force -ErrorAction SilentlyContinue
    }

    if ($PSVersionTable.PSVersion.Major -lt 6) {
        [System.Net.ServicePointManager]::SecurityProtocol = [System.Net.ServicePointManager]::SecurityProtocol -bor [System.Net.SecurityProtocolType]::Tls12
    }

    $tempPath = "$destPath.tmp.$([guid]::NewGuid().ToString('N'))"
    $prevProgress = $ProgressPreference
    $ProgressPreference = 'SilentlyContinue'
    try {
        $maxAttempts = 3
        for ($attempt = 1; $attempt -le $maxAttempts; $attempt++) {
            try {
                Remove-Item -LiteralPath $tempPath -Force -ErrorAction SilentlyContinue
                Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $tempPath
                break
            } catch {
                Write-Warning "Download attempt $attempt/$maxAttempts failed for ${Url}: $($_.Exception.Message)"
                if ($attempt -eq $maxAttempts) {
                    throw
                }
                Start-Sleep -Seconds (2 * [Math]::Pow(2, $attempt - 1))
            }
        }

        $actualHash = (Get-FileHash -LiteralPath $tempPath -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actualHash -ne $expectedLower) {
            throw "SHA256 mismatch for $FileName downloaded from ${Url}: got $actualHash, expected $expectedLower"
        }

        Move-Item -LiteralPath $tempPath -Destination $destPath -Force
    } finally {
        $ProgressPreference = $prevProgress
        Remove-Item -LiteralPath $tempPath -Force -ErrorAction SilentlyContinue
    }

    return @{
        Path     = $destPath
        CMakeUrl = ($destPath -replace '\\', '/')
        CacheHit = $false
    }
}

$resolvedBuildDir = if ($PSBoundParameters.ContainsKey('BuildDir')) {
    Resolve-CallerPath -Path $BuildDir
} else {
    Join-Path $repoRoot $BuildDir
}

$resolvedCacheDir = if (-not [string]::IsNullOrWhiteSpace($CacheDir)) {
    Resolve-CallerPath -Path $CacheDir
} else {
    ''
}

$resolvedGTestSourceDir = if (-not [string]::IsNullOrWhiteSpace($GTestSourceDir)) {
    Resolve-CallerPath -Path $GTestSourceDir
} else {
    ''
}

$resolvedJsonSourceDir = if (-not [string]::IsNullOrWhiteSpace($JsonSourceDir)) {
    Resolve-CallerPath -Path $JsonSourceDir
} else {
    ''
}

$gtestArchiveUrl = ''
$jsonArchiveUrl = ''
if (-not [string]::IsNullOrWhiteSpace($resolvedCacheDir)) {
    if ([string]::IsNullOrWhiteSpace($resolvedGTestSourceDir)) {
        $gtestCache = Get-CachedDependency `
            -CacheDirectory $resolvedCacheDir `
            -Url $script:GTestUrl `
            -FileName $script:GTestFileName `
            -ExpectedSha256 $script:GTestSha256
        $gtestArchiveUrl = $gtestCache.CMakeUrl
        $hitState = if ($gtestCache.CacheHit) { 'cache hit' } else { 'downloaded' }
        Write-Output "GoogleTest v$($script:GTestVersion) ($hitState): $($gtestCache.Path)"
    }
    if (-not $SkipCli -and [string]::IsNullOrWhiteSpace($resolvedJsonSourceDir)) {
        $jsonCache = Get-CachedDependency `
            -CacheDirectory $resolvedCacheDir `
            -Url $script:JsonUrl `
            -FileName $script:JsonFileName `
            -ExpectedSha256 $script:JsonSha256
        $jsonArchiveUrl = $jsonCache.CMakeUrl
        $hitState = if ($jsonCache.CacheHit) { 'cache hit' } else { 'downloaded' }
        Write-Output "nlohmann/json v$($script:JsonVersion) ($hitState): $($jsonCache.Path)"
    }
}

$vsInfo = Resolve-DokanVisualStudioInfo
$cmakeExe = Resolve-WindowsCMake -VsPath $vsInfo.Path
Write-Output "Using Visual Studio: $($vsInfo.Path) (version $($vsInfo.Version))"
Write-Output "Using CMake: $cmakeExe"

Invoke-DokanWithVsDevEnv -VsPath $vsInfo.Path -Arch 'amd64' -Action {
    $ninjaExe = Resolve-WindowsNinja -VsPath $vsInfo.Path
    $cmakeArgs = @(
        '-S', $sourceDir,
        '-B', $resolvedBuildDir
    )

    if ($ninjaExe) {
        Write-Output "Using Ninja generator: $ninjaExe"
        $cmakeArgs += @(
            '-G', 'Ninja',
            ("-DCMAKE_MAKE_PROGRAM=" + ($ninjaExe -replace '\\', '/')),
            "-DCMAKE_BUILD_TYPE=$Configuration"
        )
        if ($Compiler -eq 'clang-cl') {
            $clangClExe = Resolve-ClangCl -VsPath $vsInfo.Path
            Write-Output "Using clang-cl compiler: $clangClExe"
            $clangClForward = $clangClExe -replace '\\', '/'
            $cmakeArgs += @(
                "-DCMAKE_C_COMPILER=$clangClForward",
                "-DCMAKE_CXX_COMPILER=$clangClForward"
            )
        } else {
            $cmakeArgs += @(
                '-DCMAKE_C_COMPILER=cl.exe',
                '-DCMAKE_CXX_COMPILER=cl.exe'
            )
        }
    } else {
        $vsGen = Resolve-DokanVisualStudioGenerator -CMakeExe $cmakeExe -VsVersion $vsInfo.Version
        Write-Output "Ninja not found; falling back to generator: $vsGen (x64)"
        $cmakeArgs += @(
            '-G', $vsGen,
            '-A', 'x64'
        )
        if ($Compiler -eq 'clang-cl') {
            $cmakeArgs += @('-T', 'ClangCL')
        }
    }

    $werrorValue = if ($WarningsAsErrors) { 'ON' } else { 'OFF' }
    $buildCliValue = if ($SkipCli) { 'OFF' } else { 'ON' }
    $cmakeArgs += @(
        '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>',
        '-Dgtest_force_shared_crt=OFF',
        '-DBUILD_GMOCK=OFF',
        '-DINSTALL_GTEST=OFF',
        "-DFSTEST_WERROR=$werrorValue",
        "-DFSTEST_BUILD_CLI=$buildCliValue"
    )

    if (-not [string]::IsNullOrWhiteSpace($resolvedGTestSourceDir)) {
        $cmakeArgs += ("-DFSTEST_GTEST_SOURCE_DIR=" + ($resolvedGTestSourceDir -replace '\\', '/'))
    } elseif (-not [string]::IsNullOrWhiteSpace($gtestArchiveUrl)) {
        $cmakeArgs += "-DFSTEST_GTEST_URL=$gtestArchiveUrl"
    }

    if (-not $SkipCli) {
        if (-not [string]::IsNullOrWhiteSpace($resolvedJsonSourceDir)) {
            $cmakeArgs += ("-DFSTEST_JSON_SOURCE_DIR=" + ($resolvedJsonSourceDir -replace '\\', '/'))
        } elseif (-not [string]::IsNullOrWhiteSpace($jsonArchiveUrl)) {
            $cmakeArgs += "-DFSTEST_JSON_URL=$jsonArchiveUrl"
        }
    }

    if ($CMakeExtraArgs -and $CMakeExtraArgs.Count -gt 0) {
        $cmakeArgs += $CMakeExtraArgs
    }

    Write-Output "Configuring tests/fstest ($Configuration, compiler=$Compiler)..."
    Invoke-DokanExternal { & $cmakeExe @cmakeArgs }

    Write-Output "Building tests/fstest ($Configuration)..."
    $buildArgs = @('--build', $resolvedBuildDir, '--config', $Configuration, '--parallel')
    if ($SkipCli) {
        $buildArgs += @('--target', 'ntfs_helpers', 'ntfs_suite')
    }
    Invoke-DokanExternal { & $cmakeExe @buildArgs }

    if ($SkipCli) {
        $libDir = Join-Path -Path $resolvedBuildDir -ChildPath "lib\$Configuration"
        foreach ($libName in @('ntfs_helpers.lib', 'ntfs_suite_base.lib', 'ntfs_suite.lib')) {
            $libPath = Join-Path -Path $libDir -ChildPath $libName
            if (-not (Test-Path -LiteralPath $libPath -PathType Leaf)) {
                throw "Expected static library not found after build: $libPath"
            }
        }
        Write-Output "Library-only build succeeded (-SkipCli): $libDir"
    } else {
        $binDir = Join-Path $resolvedBuildDir "bin\$Configuration"
        $fsTestExe = Join-Path $binDir 'dokan_fstest.exe'
        $selfTestExe = Join-Path $binDir 'fstest_self_test.exe'

        if (-not (Test-Path -LiteralPath $fsTestExe -PathType Leaf)) {
            throw "Expected binary not found after build: $fsTestExe"
        }

        if (-not $SkipSelfTest) {
            if (-not (Test-Path -LiteralPath $selfTestExe -PathType Leaf)) {
                throw "Expected self-test binary not found after build: $selfTestExe"
            }
            Write-Output "Running fstest_self_test ($selfTestExe)..."
            Invoke-DokanExternal { & $selfTestExe }
        }

        if ($VerifyStaticCrt) {
            Write-Output "Verifying static CRT & Server 2016 base-OS imports for $fsTestExe..."
            $verifiedDlls = Test-DokanStaticCrtImports -BinaryPath $fsTestExe
            Write-Output "Import gate passed. Imported DLLs: $($verifiedDlls -join ', ')"
        }

        Write-Output "Running smoke check ($fsTestExe --gtest_list_tests)..."
        $listOutput = @(Invoke-DokanExternal { & $fsTestExe --gtest_list_tests })
        $testCount = @($listOutput | Where-Object { $_ -match '^\s+\S' }).Count
        if ($testCount -le 0) {
            throw "Smoke check '$fsTestExe --gtest_list_tests' listed 0 tests."
        }
        Write-Output "Smoke check passed: $fsTestExe lists $testCount tests."
    }
}
