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

function Get-ExcludedUnixPrefixes {
    $cygwinDir = if (-not [string]::IsNullOrWhiteSpace($env:CYGWIN_INST_DIR)) { $env:CYGWIN_INST_DIR } else { 'C:\cygwin64' }
    $msys2Dir = if (-not [string]::IsNullOrWhiteSpace($env:MSYS2_INST_DIR)) { $env:MSYS2_INST_DIR } else { 'C:\msys64' }
    return @(
        ([System.IO.Path]::GetFullPath($cygwinDir).TrimEnd('\') + '\'),
        ([System.IO.Path]::GetFullPath($msys2Dir).TrimEnd('\') + '\')
    )
}

function Test-ExcludedPath {
    param(
        [Parameter(Mandatory = $true)][string]$FullPath,
        [Parameter(Mandatory = $true)][string[]]$ExcludedPrefixes
    )
    foreach ($prefix in $ExcludedPrefixes) {
        if ($FullPath.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
            return $true
        }
    }
    return $false
}

function Resolve-VisualStudioInfo {
    $vs = Find-DokanVisualStudio -Requires 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64' -Latest -IncludeVersion -RequireVsWhere | Select-Object -First 1
    if (-not $vs) {
        $vs = Find-DokanVisualStudio -Requires 'Microsoft.Component.MSBuild' -Latest -IncludeVersion -RequireVsWhere | Select-Object -First 1
    }
    if (-not $vs) {
        throw 'Visual Studio installation with C++ tools not found via vswhere.exe.'
    }
    return $vs
}

function Resolve-WindowsCMake {
    param([string]$VsPath = '')
    $excludedPrefixes = Get-ExcludedUnixPrefixes

    foreach ($cmd in @(Get-Command cmake.exe -All -CommandType Application -ErrorAction SilentlyContinue)) {
        if (-not $cmd.Source) { continue }
        $fullSource = [System.IO.Path]::GetFullPath($cmd.Source)
        if (-not (Test-ExcludedPath -FullPath $fullSource -ExcludedPrefixes $excludedPrefixes)) {
            return $fullSource
        }
    }

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

    foreach ($candidate in ($candidates | Select-Object -Unique)) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return [System.IO.Path]::GetFullPath($candidate)
        }
    }
    throw 'Native Windows cmake.exe not found (Cygwin and MSYS2 CMake are excluded).'
}

function Resolve-WindowsNinja {
    param([string]$VsPath = '')
    $excludedPrefixes = Get-ExcludedUnixPrefixes

    foreach ($cmd in @(Get-Command ninja.exe -All -CommandType Application -ErrorAction SilentlyContinue)) {
        if (-not $cmd.Source) { continue }
        $fullSource = [System.IO.Path]::GetFullPath($cmd.Source)
        if (-not (Test-ExcludedPath -FullPath $fullSource -ExcludedPrefixes $excludedPrefixes)) {
            return $fullSource
        }
    }

    $candidates = @()
    if (-not [string]::IsNullOrWhiteSpace($VsPath)) {
        $candidates += Join-Path $VsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
    }
    $candidates += @(
        "${env:ProgramFiles}\Ninja\ninja.exe",
        'C:\ProgramData\chocolatey\bin\ninja.exe'
    )
    foreach ($candidate in ($candidates | Select-Object -Unique)) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return [System.IO.Path]::GetFullPath($candidate)
        }
    }
    return $null
}

function Resolve-ClangCl {
    param([string]$VsPath = '')
    $excludedPrefixes = Get-ExcludedUnixPrefixes

    # Prefer the Visual Studio bundled LLVM toolchain so clang-cl matches the
    # active MSVC STL headers (avoiding STL1000 compiler version mismatches).
    $candidates = @()
    if (-not [string]::IsNullOrWhiteSpace($VsPath)) {
        $candidates += Join-Path $VsPath 'VC\Tools\Llvm\x64\bin\clang-cl.exe'
    }
    foreach ($cmd in @(Get-Command clang-cl.exe -All -CommandType Application -ErrorAction SilentlyContinue)) {
        if (-not $cmd.Source) { continue }
        $fullSource = [System.IO.Path]::GetFullPath($cmd.Source)
        if (-not (Test-ExcludedPath -FullPath $fullSource -ExcludedPrefixes $excludedPrefixes)) {
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

function Import-VsDevEnvironment {
    param([Parameter(Mandatory = $true)][string]$VsPath)

    $vsDevCmd = Join-Path $VsPath 'Common7\Tools\VsDevCmd.bat'
    if (-not (Test-Path -LiteralPath $vsDevCmd -PathType Leaf)) {
        throw "VsDevCmd.bat not found at $vsDevCmd"
    }

    $sentinel = '===FSTEST_VSDEV_ENV_START==='
    $tempCmd = Join-Path ([System.IO.Path]::GetTempPath()) ("fstest_vsdev_" + [guid]::NewGuid().ToString('N') + '.cmd')
    try {
        $cmdContent = @(
            '@echo off'
            'set VSCMD_SKIP_SENDTELEMETRY=1'
            "call `"$vsDevCmd`" -arch=amd64 -host_arch=amd64 -no_logo"
            'if errorlevel 1 exit /b %errorlevel%'
            "echo $sentinel"
            'set'
        ) -join "`r`n"
        $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
        [System.IO.File]::WriteAllText($tempCmd, $cmdContent + "`r`n", $utf8NoBom)

        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = if ($env:ComSpec) { $env:ComSpec } else { 'cmd.exe' }
        $psi.Arguments = "/d /c `"$tempCmd`""
        $psi.UseShellExecute = $false
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true
        $psi.CreateNoWindow = $true

        $proc = New-Object System.Diagnostics.Process
        $proc.StartInfo = $psi
        try {
            [void]$proc.Start()
            $stderrTask = $proc.StandardError.ReadToEndAsync()
            $stdout = $proc.StandardOutput.ReadToEnd()
            $proc.WaitForExit()
            $stderr = $stderrTask.GetAwaiter().GetResult()
            $exitCode = $proc.ExitCode
        } finally {
            $proc.Dispose()
        }

        if ($exitCode -ne 0) {
            throw "VsDevCmd.bat failed (exit $exitCode). Stdout:`n$stdout`nStderr:`n$stderr"
        }

        $inEnv = $false
        $parsed = @{}
        foreach ($rawLine in ($stdout -split "`r?`n")) {
            if (-not $inEnv) {
                if ($rawLine -eq $sentinel) {
                    $inEnv = $true
                }
                continue
            }
            $eqIndex = $rawLine.IndexOf('=')
            if ($eqIndex -gt 0) {
                $name = $rawLine.Substring(0, $eqIndex)
                $value = $rawLine.Substring($eqIndex + 1)
                $parsed[$name] = $value
            }
        }

        if (-not $inEnv -or $parsed.Count -eq 0) {
            throw "Failed to capture environment variables from $vsDevCmd"
        }

        foreach ($kv in $parsed.GetEnumerator()) {
            [System.Environment]::SetEnvironmentVariable($kv.Key, $kv.Value, 'Process')
        }
    } finally {
        Remove-Item -LiteralPath $tempCmd -Force -ErrorAction SilentlyContinue
    }
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

function Resolve-VisualStudioGenerator {
    param(
        [Parameter(Mandatory = $true)][string]$CMakeExe,
        [string]$VsVersion = ''
    )

    $majorVersion = 0
    if (-not [string]::IsNullOrWhiteSpace($VsVersion) -and ($VsVersion -match '^(\d+)')) {
        $majorVersion = [int]$Matches[1]
    }
    $knownGenerators = @{
        18 = 'Visual Studio 18 2026'
        17 = 'Visual Studio 17 2022'
        16 = 'Visual Studio 16 2019'
    }

    $helpText = (@(& $CMakeExe --help 2>&1) -join "`n")
    if ($knownGenerators.ContainsKey($majorVersion)) {
        $preferred = $knownGenerators[$majorVersion]
        if ($helpText -match [regex]::Escape($preferred)) {
            return $preferred
        }
        throw "CMake at $CMakeExe does not support generator '$preferred' and ninja.exe was not found. Install Ninja or upgrade CMake."
    }

    foreach ($ver in @(18, 17, 16)) {
        $candidate = $knownGenerators[$ver]
        if ($helpText -match [regex]::Escape($candidate)) {
            return $candidate
        }
    }
    throw "No supported Visual Studio generator found in $CMakeExe --help and ninja.exe was not found."
}

function Test-StaticCrtImports {
    param([Parameter(Mandatory = $true)][string]$BinaryPath)

    $dumpbinCmd = Get-Command dumpbin.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $dumpbinCmd) {
        throw 'dumpbin.exe not found on PATH; cannot verify static CRT imports.'
    }

    $prevPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = @(& $dumpbinCmd.Source /nologo /imports $BinaryPath 2>&1)
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $prevPreference
    }
    if ($exitCode -ne 0) {
        throw "dumpbin /imports failed with exit code ${exitCode}:`n$($output -join "`n")"
    }

    $importedDlls = New-Object System.Collections.Generic.List[string]
    $seen = @{}
    foreach ($line in $output) {
        $text = [string]$line
        if ($text -match '^\s*Summary\s*$') {
            break
        }
        if ($text -match '^\s+([A-Za-z0-9._-]+\.dll)\s*$') {
            $dll = $Matches[1]
            $key = $dll.ToLowerInvariant()
            if (-not $seen.ContainsKey($key)) {
                $seen[$key] = $true
                $importedDlls.Add($dll)
            }
        }
    }

    if ($importedDlls.Count -eq 0) {
        throw "Failed to parse any imported DLLs from dumpbin /imports output for $BinaryPath."
    }

    $denylistPatterns = @(
        'VCRUNTIME*',
        'MSVCP*',
        'ucrtbase*',
        'CONCRT*',
        'api-ms-win-crt-*',
        'dokan*'
    )
    $server2016Allowlist = @(
        'KERNEL32.dll',
        'ADVAPI32.dll',
        'ntdll.dll',
        'bcrypt.dll',
        'bcryptprimitives.dll'
    )

    $deniedHits = @()
    $disallowedHits = @()
    foreach ($dll in $importedDlls) {
        foreach ($pattern in $denylistPatterns) {
            if ($dll -like $pattern) {
                $deniedHits += $dll
                break
            }
        }
        if ($server2016Allowlist -notcontains $dll) {
            $disallowedHits += $dll
        }
    }

    if ($deniedHits.Count -gt 0 -or $disallowedHits.Count -gt 0) {
        $details = @("Import portability gate failed for $BinaryPath.")
        $details += "Imported DLLs: $($importedDlls -join ', ')"
        if ($deniedHits.Count -gt 0) {
            $details += "Denied CRT/Dokan imports: $($deniedHits -join ', ')"
        }
        if ($disallowedHits.Count -gt 0) {
            $details += "Imports outside Server 2016 base-OS allowlist ($($server2016Allowlist -join ', ')): $($disallowedHits -join ', ')"
        }
        throw ($details -join "`n")
    }

    return @($importedDlls)
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

$vsInfo = Resolve-VisualStudioInfo
$cmakeExe = Resolve-WindowsCMake -VsPath $vsInfo.Path
Write-Output "Using Visual Studio: $($vsInfo.Path) (version $($vsInfo.Version))"
Write-Output "Using CMake: $cmakeExe"

$savedEnv = @{}
foreach ($entry in [System.Environment]::GetEnvironmentVariables('Process').GetEnumerator()) {
    $savedEnv[$entry.Key] = $entry.Value
}

try {
    Import-VsDevEnvironment -VsPath $vsInfo.Path

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
        $vsGen = Resolve-VisualStudioGenerator -CMakeExe $cmakeExe -VsVersion $vsInfo.Version
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
            $verifiedDlls = Test-StaticCrtImports -BinaryPath $fsTestExe
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
} finally {
    $currentKeys = @([System.Environment]::GetEnvironmentVariables('Process').Keys)
    foreach ($key in $currentKeys) {
        if (-not $savedEnv.ContainsKey($key)) {
            [System.Environment]::SetEnvironmentVariable($key, $null, 'Process')
        }
    }
    foreach ($kv in $savedEnv.GetEnumerator()) {
        [System.Environment]::SetEnvironmentVariable($kv.Key, $kv.Value, 'Process')
    }
}
