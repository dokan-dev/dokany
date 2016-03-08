REM Build release to prepare for packaging

REM Make sure MSBUILD is available
set PATH=%PATH%;%PROGRAMFILES(x86)%\MSBuild\14.0\Bin
set VCTargetsPath=%PROGRAMFILES%
IF %processor_architecture%==AMD64 set VCTargetsPath=%PROGRAMFILES(x86)%
set VCTargetsPath=%VCTargetsPath%\MSBuild\Microsoft.Cpp\v4.0\V140

REM Build
msbuild dokan.sln /p:Configuration=Release /p:Platform=Win32 /t:Build
msbuild dokan.sln /p:Configuration=Release /p:Platform=x64 /t:Build
msbuild dokan.sln /p:Configuration="Win7 Release" /p:Platform=Win32 /t:Build
msbuild dokan.sln /p:Configuration="Win7 Release" /p:Platform=x64 /t:Build
msbuild dokan.sln /p:Configuration="Win8 Release" /p:Platform=Win32 /t:Build
msbuild dokan.sln /p:Configuration="Win8 Release" /p:Platform=x64 /t:Build
msbuild dokan.sln /p:Configuration="Win8.1 Release" /p:Platform=Win32 /t:Build
msbuild dokan.sln /p:Configuration="Win8.1 Release" /p:Platform=x64 /t:Build
msbuild dokan.sln /p:Configuration="Win10 Release" /p:Platform=Win32 /t:Build
msbuild dokan.sln /p:Configuration="Win10 Release" /p:Platform=x64 /t:Build

msbuild dokan.sln /p:Configuration=Debug /p:Platform=Win32 /t:Build
msbuild dokan.sln /p:Configuration=Debug /p:Platform=x64 /t:Build
msbuild dokan.sln /p:Configuration="Win7 Debug" /p:Platform=Win32 /t:Build
msbuild dokan.sln /p:Configuration="Win7 Debug" /p:Platform=x64 /t:Build
msbuild dokan.sln /p:Configuration="Win8 Debug" /p:Platform=Win32 /t:Build
msbuild dokan.sln /p:Configuration="Win8 Debug" /p:Platform=x64 /t:Build
msbuild dokan.sln /p:Configuration="Win8.1 Debug" /p:Platform=Win32 /t:Build
msbuild dokan.sln /p:Configuration="Win8.1 Debug" /p:Platform=x64 /t:Build
msbuild dokan.sln /p:Configuration="Win10 Debug" /p:Platform=Win32 /t:Build
msbuild dokan.sln /p:Configuration="Win10 Debug" /p:Platform=x64 /t:Build

IF EXIST C:\cygwin ( Powershell.exe -executionpolicy remotesigned -File dokan_fuse/build.ps1 ) ELSE ( echo "Cygwin/Msys2 build disabled" )