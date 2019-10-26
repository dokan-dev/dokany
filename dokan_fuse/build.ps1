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
        cmake ../../../ -DCMAKE_TOOLCHAIN_FILE=../../../cmake/toolchain-i686-pc-cygwin.cmake -DCMAKE_INSTALL_PREFIX='../../../../$installDir' -DCMAKE_INSTALL_BINDIR=. &&
        make -j `$(getconf _NPROCESSORS_ONLN) install"
    & $env:CYGWIN_INST_DIR\bin\bash -lc "
        cd '$currentPath' &&
        i686-pc-cygwin-gcc -o '$installDir'/mirror samples/fuse_mirror/fusexmp.c `$(PKG_CONFIG_PATH='$installDir/lib/pkgconfig' pkg-config fuse --cflags --libs)"
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
& {
    $buildDir="dokan_fuse/build/Win32/Msys2/"
    $installDir="Win32/Msys2/"
    New-Item -Force -Type Directory $buildDir
    $env:MSYSTEM = "MINGW32"
    & $env:MSYS2_INST_DIR\usr\bin\bash -lc "
        cd '$currentPath'/'$buildDir' &&
        cmake ../../../ -DCMAKE_INSTALL_PREFIX='../../../../$installDir' -DCMAKE_INSTALL_BINDIR=. -G 'MSYS Makefiles' &&
        make -j `$(getconf _NPROCESSORS_ONLN) install"
    }
    Remove-Item Env:\MSYSTEM
    if ($LASTEXITCODE -ne 0) {
        $script:failed = $LASTEXITCODE
    }
& {
    $buildDir="dokan_fuse/build/x64/Msys2/"
    $installDir="x64/Msys2/"
    New-Item -Force -Type Directory $buildDir
    $env:MSYSTEM = "MINGW64"
    & $env:MSYS2_INST_DIR\usr\bin\bash -lc "
        cd '$currentPath'/'$buildDir' &&
        cmake ../../../ -DCMAKE_INSTALL_PREFIX='../../../../$installDir' -DCMAKE_INSTALL_BINDIR=. -G 'MSYS Makefiles' &&
        make -j `$(getconf _NPROCESSORS_ONLN) install"
    Remove-Item Env:\MSYSTEM
    if ($LASTEXITCODE -ne 0) {
        $script:failed = $LASTEXITCODE
    }
}

if ($LASTEXITCODE -ne 0) {
    Write-Error "At least one build-command failed. The last command that failed returned with error $script:failed"
    Exit $script:failed
}
