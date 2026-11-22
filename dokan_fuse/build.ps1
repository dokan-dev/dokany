param (
    [ValidateSet('cygwin', 'msys2')]
    [string[]]$Targets = @('cygwin', 'msys2'),
    [ValidateSet('MINGW32', 'MINGW64', 'UCRT64')]
    [string[]]$Msys2Envs = @('MINGW32', 'UCRT64')
)

$currentPath = Get-Location
$cygwinDir = if (-not [string]::IsNullOrWhiteSpace($env:CYGWIN_INST_DIR)) { $env:CYGWIN_INST_DIR } else { 'C:\cygwin64' }
$msys2Dir = if (-not [string]::IsNullOrWhiteSpace($env:MSYS2_INST_DIR)) { $env:MSYS2_INST_DIR } else { 'C:\msys64' }
$cygwinBash = Join-Path $cygwinDir 'bin\bash.exe'
$msys2Bash = Join-Path $msys2Dir 'usr\bin\bash.exe'
$script:failed = 0

if (-not $PSBoundParameters.ContainsKey('Targets')) {
    $detectedTargets = @()
    if (Test-Path -LiteralPath $cygwinBash -PathType Leaf) { $detectedTargets += 'cygwin' }
    if (Test-Path -LiteralPath $msys2Bash -PathType Leaf) { $detectedTargets += 'msys2' }
    if ($detectedTargets.Count -eq 0) {
        Write-Error "Neither Cygwin ($cygwinBash) nor MSYS2 ($msys2Bash) was found."
        Exit 1
    }
    $Targets = $detectedTargets
}

$Targets = @($Targets | ForEach-Object { $_.ToLowerInvariant() } | Select-Object -Unique)
$Msys2Envs = @($Msys2Envs | ForEach-Object { $_.ToUpperInvariant() } | Select-Object -Unique)

if (($Msys2Envs -contains 'UCRT64') -and ($Msys2Envs -contains 'MINGW64')) {
    Write-Error "Cannot build both UCRT64 and MINGW64 in the same run because both install to x64/Msys2/."
    Exit 1
}

function Invoke-CygwinFuseBuild {
    param(
        [Parameter(Mandatory = $true)][string]$BuildDir,
        [Parameter(Mandatory = $true)][string]$InstallDir
    )
    if (-not (Test-Path -LiteralPath $cygwinBash -PathType Leaf)) {
        Write-Error "Cygwin bash not found at $cygwinBash"
        $script:failed = 1
        return
    }

    New-Item -Force -Type Directory $BuildDir | Out-Null

    $envBackup = @{
        DOKAN_REPO_ROOT   = $env:DOKAN_REPO_ROOT
        DOKAN_BUILD_DIR   = $env:DOKAN_BUILD_DIR
        DOKAN_INSTALL_DIR = $env:DOKAN_INSTALL_DIR
    }
    $tempScript = Join-Path ([System.IO.Path]::GetTempPath()) ("dokan_fuse_cygwin_" + [guid]::NewGuid().ToString('N') + ".sh")
    try {
        $env:DOKAN_REPO_ROOT = $currentPath.Path
        $env:DOKAN_BUILD_DIR = $BuildDir
        $env:DOKAN_INSTALL_DIR = $InstallDir

        $bashScript = @'
set -e
repo_root="$(cygpath -u "$DOKAN_REPO_ROOT")"
cd "$repo_root/$DOKAN_BUILD_DIR"

for tool in cmake gcc g++ make pkg-config; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "ERROR: required Cygwin build tool not found: $tool" >&2
        exit 1
    fi
done

echo "Building dokan_fuse (Cygwin x64):"
echo "  cmake:      $(command -v cmake) ($(cmake --version | sed -n '1p'))"
echo "  gcc:        $(command -v gcc) ($(gcc --version | sed -n '1p'))"
echo "  make:       $(command -v make) ($(make --version | sed -n '1p'))"
echo "  pkg-config: $(command -v pkg-config) ($(pkg-config --version))"

cmake ../../../ \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="../../../../$DOKAN_INSTALL_DIR" \
    -DCMAKE_INSTALL_BINDIR=.
make -j "$(getconf _NPROCESSORS_ONLN)" install

cd "$repo_root"
fuse_flags="$(PKG_CONFIG_PATH="$DOKAN_INSTALL_DIR/lib/pkgconfig" pkg-config fuse --cflags --libs)"
gcc -o "$DOKAN_INSTALL_DIR/mirror" samples/fuse_mirror/fusexmp.c $fuse_flags
'@
        $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
        [System.IO.File]::WriteAllText($tempScript, ($bashScript -replace "`r`n", "`n") + "`n", $utf8NoBom)
        & $cygwinBash -l $tempScript
        if ($LASTEXITCODE -ne 0) {
            $script:failed = $LASTEXITCODE
        }
    } finally {
        Remove-Item -LiteralPath $tempScript -Force -ErrorAction SilentlyContinue
        foreach ($key in $envBackup.Keys) {
            if ($null -eq $envBackup[$key]) {
                Remove-Item "Env:\$key" -ErrorAction SilentlyContinue
            } else {
                Set-Item "Env:\$key" $envBackup[$key]
            }
        }
    }
}

function Resolve-WindowsCMake {
    $excludedPrefixes = @(
        ([System.IO.Path]::GetFullPath($cygwinDir).TrimEnd('\') + '\'),
        ([System.IO.Path]::GetFullPath($msys2Dir).TrimEnd('\') + '\')
    )
    foreach ($cmd in (Get-Command cmake.exe -All -CommandType Application -ErrorAction SilentlyContinue)) {
        if (-not $cmd.Source) { continue }
        $fullSource = [System.IO.Path]::GetFullPath($cmd.Source)
        $isExcluded = $false
        foreach ($prefix in $excludedPrefixes) {
            if ($fullSource.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
                $isExcluded = $true
                break
            }
        }
        if (-not $isExcluded) {
            return $cmd.Source
        }
    }

    $candidates = @(
        "${env:ProgramFiles}\CMake\bin\cmake.exe",
        "${env:ProgramFiles(x86)}\CMake\bin\cmake.exe"
    )
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere) {
        $vsPaths = & $vswhere -products * -sort -property installationPath 2>$null
        foreach ($vsPath in $vsPaths) {
            if (-not [string]::IsNullOrEmpty($vsPath)) {
                $candidates += Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
            }
        }
    }
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
    }
    return $null
}

function Invoke-Msys2FuseBuild {
    param(
        [Parameter(Mandatory = $true)][string]$Msystem,
        [Parameter(Mandatory = $true)][string]$BuildDir,
        [Parameter(Mandatory = $true)][string]$InstallDir,
        [string]$WinCmake
    )
    if (-not (Test-Path -LiteralPath $msys2Bash -PathType Leaf)) {
        Write-Error "MSYS2 bash not found at $msys2Bash"
        $script:failed = 1
        return
    }

    New-Item -Force -Type Directory $BuildDir | Out-Null

    $envBackup = @{
        MSYSTEM           = $env:MSYSTEM
        DOKAN_REPO_ROOT   = $env:DOKAN_REPO_ROOT
        DOKAN_BUILD_DIR   = $env:DOKAN_BUILD_DIR
        DOKAN_INSTALL_DIR = $env:DOKAN_INSTALL_DIR
        DOKAN_WIN_CMAKE   = $env:DOKAN_WIN_CMAKE
    }
    $tempScript = Join-Path ([System.IO.Path]::GetTempPath()) ("dokan_fuse_" + [guid]::NewGuid().ToString('N') + ".sh")
    try {
        $env:MSYSTEM = $Msystem
        $env:DOKAN_REPO_ROOT = $currentPath.Path
        $env:DOKAN_BUILD_DIR = $BuildDir
        $env:DOKAN_INSTALL_DIR = $InstallDir
        if ($WinCmake) {
            $env:DOKAN_WIN_CMAKE = $WinCmake
        } else {
            Remove-Item Env:\DOKAN_WIN_CMAKE -ErrorAction SilentlyContinue
        }

        $bashScript = @'
set -e
repo_root="$(cygpath -u "$DOKAN_REPO_ROOT")"
cd "$repo_root/$DOKAN_BUILD_DIR"

cmake_bin=""
if [ -x "$MINGW_PREFIX/bin/cmake.exe" ]; then
    cmake_bin="$MINGW_PREFIX/bin/cmake.exe"
elif [ -n "${DOKAN_WIN_CMAKE:-}" ]; then
    cmake_bin="$(cygpath -u "$DOKAN_WIN_CMAKE")"
else
    cmake_bin="$(command -v cmake || true)"
fi
if [ -z "$cmake_bin" ] || [ ! -x "$cmake_bin" ]; then
    echo "ERROR: cmake not found in MSYS2 ($MSYSTEM) or Windows PATH" >&2
    exit 1
fi

cc_bin="$MINGW_PREFIX/bin/gcc.exe"
cxx_bin="$MINGW_PREFIX/bin/g++.exe"
rc_bin="$MINGW_PREFIX/bin/windres.exe"
make_bin="$(command -v make || true)"
for tool in "$cc_bin" "$cxx_bin" "$rc_bin" "$make_bin"; do
    if [ -z "$tool" ] || [ ! -x "$tool" ]; then
        echo "ERROR: required MSYS2/$MSYSTEM build tool not found: ${tool:-make}" >&2
        exit 1
    fi
done

echo "Building dokan_fuse ($MSYSTEM):"
echo "  cmake: $cmake_bin ($("$cmake_bin" --version | sed -n '1p'))"
echo "  gcc:   $cc_bin ($("$cc_bin" --version | sed -n '1p'))"
echo "  make:  $make_bin ($("$make_bin" --version | sed -n '1p'))"

"$cmake_bin" ../../../ \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="../../../../$DOKAN_INSTALL_DIR" \
    -DCMAKE_INSTALL_BINDIR=. \
    -DCMAKE_C_COMPILER="$(cygpath -m "$cc_bin")" \
    -DCMAKE_CXX_COMPILER="$(cygpath -m "$cxx_bin")" \
    -DCMAKE_RC_COMPILER="$(cygpath -m "$rc_bin")" \
    -DCMAKE_MAKE_PROGRAM="$(cygpath -m "$make_bin")" \
    -G "MSYS Makefiles"
"$make_bin" -j "$(getconf _NPROCESSORS_ONLN)" install
'@
        $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
        [System.IO.File]::WriteAllText($tempScript, ($bashScript -replace "`r`n", "`n") + "`n", $utf8NoBom)
        & $msys2Bash -l $tempScript
        if ($LASTEXITCODE -ne 0) {
            $script:failed = $LASTEXITCODE
        }
    } finally {
        Remove-Item -LiteralPath $tempScript -Force -ErrorAction SilentlyContinue
        foreach ($key in $envBackup.Keys) {
            if ($null -eq $envBackup[$key]) {
                Remove-Item "Env:\$key" -ErrorAction SilentlyContinue
            } else {
                Set-Item "Env:\$key" $envBackup[$key]
            }
        }
    }
}

if ($Targets -contains 'cygwin') {
    Invoke-CygwinFuseBuild -BuildDir 'dokan_fuse/build/x64/Cygwin' -InstallDir 'x64/Cygwin'
}

if ($Targets -contains 'msys2') {
    $msys2Map = @{
        'MINGW32' = @{ BuildDir = 'dokan_fuse/build/Win32/MINGW32'; InstallDir = 'Win32/Msys2' }
        'MINGW64' = @{ BuildDir = 'dokan_fuse/build/x64/MINGW64';   InstallDir = 'x64/Msys2' }
        'UCRT64'  = @{ BuildDir = 'dokan_fuse/build/x64/UCRT64';    InstallDir = 'x64/Msys2' }
    }
    $winCmake = Resolve-WindowsCMake
    foreach ($msystem in $Msys2Envs) {
        $cfg = $msys2Map[$msystem]
        Invoke-Msys2FuseBuild -Msystem $msystem -BuildDir $cfg.BuildDir -InstallDir $cfg.InstallDir -WinCmake $winCmake
    }
}

if ($script:failed -ne 0) {
    Write-Error "At least one build-command failed. The last command that failed returned with error $script:failed"
    Exit $script:failed
}
