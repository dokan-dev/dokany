$currentPath=Get-Location

& dokan_fuse/cleancmake.bat
& C:\cygwin\bin\bash -lc "cd '$currentPath'/dokan_fuse/ && cmake . && make"
& C:\cygwin\bin\bash -lc "cd '$currentPath'/samples/fuse_mirror/ && gcc -o mirror fusexmp.c -I../../dokan_fuse/include -D_FILE_OFFSET_BITS=64 -L../../dokan_fuse -lcygdokanfuse1"
New-Item -Force -Type Directory "Win32/Cygwin/"
copy-item "dokan_fuse/cygdokanfuse1.dll" "Win32/Cygwin/"
copy-item "samples/fuse_mirror/mirror.exe" "Win32/Cygwin/" 
Remove-Item "dokan_fuse/cygdokanfuse1.dll"
Remove-Item "samples/fuse_mirror/mirror.exe"

& dokan_fuse/cleancmake.bat
& C:\cygwin64\bin\bash -lc "cd '$currentPath'/dokan_fuse/ && cmake . && make"
& C:\cygwin64\bin\bash -lc "cd '$currentPath'/samples/fuse_mirror/ && gcc -o mirror fusexmp.c -I../../dokan_fuse/include -D_FILE_OFFSET_BITS=64 -L../../dokan_fuse -lcygdokanfuse1"
New-Item -Force -Type Directory "x64/Cygwin/"
copy-item "dokan_fuse/cygdokanfuse1.dll" "x64/Cygwin/"
copy-item "samples/fuse_mirror/mirror.exe" "x64/Cygwin/" 
Remove-Item "dokan_fuse/cygdokanfuse1.dll"
Remove-Item "samples/fuse_mirror/mirror.exe"

& dokan_fuse/cleancmake.bat
& C:\msys64\mingw32_shell.bat -lc "cd '$currentPath'/dokan_fuse/ && cmake . -G 'MSYS Makefiles' && make" | Out-Host
New-Item -Force -Type Directory "Win32/Msys2/"
copy-item "dokan_fuse/libdokanfuse1.dll" "Win32/Msys2/"
Remove-Item "dokan_fuse/libdokanfuse1.dll"

& dokan_fuse/cleancmake.bat
& C:\msys64\mingw64_shell.bat -lc "cd '$currentPath'/dokan_fuse/ && cmake . -G 'MSYS Makefiles' && make" | Out-Host
New-Item -Force -Type Directory "x64/Msys2/"
copy-item "dokan_fuse/libdokanfuse1.dll" "x64/Msys2/"
Remove-Item "dokan_fuse/libdokanfuse1.dll"