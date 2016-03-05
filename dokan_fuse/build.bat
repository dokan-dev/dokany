call dokan_fuse/cleancmake.bat
C:\cygwin\bin\bash -lc "cd '%cd%'/dokan_fuse/ && cmake . && make"
C:\cygwin\bin\bash -lc "cd '%cd%'/samples/fuse_mirror/ && gcc -o mirror fusexmp.c -I../../dokan_fuse/include -D_FILE_OFFSET_BITS=64 -L../../dokan_fuse -lcygdokanfuse1"
echo f | xcopy /f /y "dokan_fuse/cygdokanfuse1.dll" "Win32/Cygwin/"
echo f | xcopy /f /y "samples/fuse_mirror/mirror.exe" "Win32/Cygwin/"

call dokan_fuse/cleancmake.bat
C:\cygwin64\bin\bash -lc "cd '%cd%'/dokan_fuse/ && cmake . && make"
C:\cygwin64\bin\bash -lc "cd '%cd%'/samples/fuse_mirror/ && gcc -o mirror fusexmp.c -I../../dokan_fuse/include -D_FILE_OFFSET_BITS=64 -L../../dokan_fuse -lcygdokanfuse1"
echo f | xcopy /f /y "dokan_fuse/cygdokanfuse1.dll" "x64/Cygwin/"
echo f | xcopy /f /y "samples/fuse_mirror/mirror.exe" "x64/Cygwin/"