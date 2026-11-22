$currentPath=Get-Location
if (-not (Test-Path env:MSYS2_INST_DIR)) { $env:MSYS2_INST_DIR = 'C:\msys64' }
if (-not (Test-Path env:CYGWIN_INST_DIR)) { $env:CYGWIN_INST_DIR = 'C:\cygwin64' }
$script:failed = 0
& {
    $buildDir="dokan_fuse/build/Win32/Cygwin/"
    $installDir="Win32/Cygwin/"
    New-Item -Force -Type Directory $buildDir
    & $env:CYGWIN_INST_DIR\bin\bash -lc "
        cd '$currentPath'/'$buildDir' &&
        cmake ../../../ -DCMAKE_INSTALL_PREFIX='../../../../$installDir' -DCMAKE_INSTALL_BINDIR=. &&
        make -j `$(getconf _NPROCESSORS_ONLN) install"
    & $env:CYGWIN_INST_DIR\bin\bash -lc "
        cd '$currentPath' &&
        gcc -o '$installDir'/mirror samples/fuse_mirror/fusexmp.c `$(PKG_CONFIG_PATH='$installDir/lib/pkgconfig' pkg-config fuse --cflags --libs)"
    if ($LASTEXITCODE -ne 0) {
        $script:failed = $LASTEXITCODE
    }
}
& {
    $buildDir="dokan_fuse/build/x64/Cygwin/"
    $installDir="x64/Cygwin/"
    New-Item -Force -Type Directory $buildDir
    & $env:CYGWIN_INST_DIR\bin\bash -lc "
        cd '$currentPath'/'$buildDir' &&
        cmake ../../../ -DCMAKE_INSTALL_PREFIX='../../../../$installDir' -DCMAKE_INSTALL_BINDIR=. &&
        make -j `$(getconf _NPROCESSORS_ONLN) install"
    & $env:CYGWIN_INST_DIR\bin\bash -lc "
        cd '$currentPath' &&
        gcc -o '$installDir'/mirror samples/fuse_mirror/fusexmp.c `$(PKG_CONFIG_PATH='$installDir/lib/pkgconfig' pkg-config fuse --cflags --libs)"
    if ($LASTEXITCODE -ne 0) {
        $script:failed = $LASTEXITCODE
    }
}
function Resolve-WindowsCMake {
    $excludedPrefixes = @(
        ([System.IO.Path]::GetFullPath($env:CYGWIN_INST_DIR).TrimEnd('\') + '\'),
        ([System.IO.Path]::GetFullPath((Join-Path $env:MSYS2_INST_DIR 'usr\bin')).TrimEnd('\') + '\')
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
        & $env:MSYS2_INST_DIR\usr\bin\bash -l $tempScript
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

$winCmake = Resolve-WindowsCMake
Invoke-Msys2FuseBuild -Msystem 'MINGW32' -BuildDir 'dokan_fuse/build/Win32/Msys2/' -InstallDir 'Win32/Msys2/' -WinCmake $winCmake
Invoke-Msys2FuseBuild -Msystem 'MINGW64' -BuildDir 'dokan_fuse/build/x64/Msys2/' -InstallDir 'x64/Msys2/' -WinCmake $winCmake

if ($script:failed -ne 0) {
    Write-Error "At least one build-command failed. The last command that failed returned with error $script:failed"
    Exit $script:failed
}
