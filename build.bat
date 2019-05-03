@setlocal ENABLEDELAYEDEXPANSION
REM Build release to prepare for packaging

REM Make sure MSBUILD is available
FOR /f "delims=" %%A IN (
'"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -property installationPath'
) DO SET "VS_PATH=%%A"

SET MSBUILD_BIN_PATH=%VS_PATH%\MSBuild\15.0\Bin
IF NOT EXIST "%VS_PATH%" (
	ECHO Visual C++ 2017 NOT Installed.
	PAUSE
	EXIT /B
)

set PATH=%PATH%;%MSBUILD_BIN_PATH%

REM Enable AppVeyor build message logging if running under AppVeyor
IF "%APPVEYOR%"=="True" set CI_BUILD_ARG="/l:C:\Program Files\AppVeyor\BuildAgent\Appveyor.MSBuildLogger.dll"
REM Build
msbuild dokan.sln /p:Configuration=Release /p:Platform=Win32 /t:Build !CI_BUILD_ARG! || goto :failure
msbuild dokan.sln /p:Configuration=Release /p:Platform=x64 /t:Build !CI_BUILD_ARG! || goto :failure
msbuild dokan.sln /p:Configuration=Release /p:Platform=ARM /t:Build !CI_BUILD_ARG! || goto :failure
msbuild dokan.sln /p:Configuration=Release /p:Platform=ARM64 /t:Build !CI_BUILD_ARG! || goto :failure
msbuild dokan.sln /p:Configuration="Win7 Release" /p:Platform=Win32 /t:Build !CI_BUILD_ARG! || goto :failure
msbuild dokan.sln /p:Configuration="Win7 Release" /p:Platform=x64 /t:Build !CI_BUILD_ARG! || goto :failure
msbuild dokan.sln /p:Configuration="Win8 Release" /p:Platform=Win32 /t:Build !CI_BUILD_ARG! || goto :failure
msbuild dokan.sln /p:Configuration="Win8 Release" /p:Platform=x64 /t:Build !CI_BUILD_ARG! || goto :failure
msbuild dokan.sln /p:Configuration="Win8 Release" /p:Platform=ARM /t:Build !CI_BUILD_ARG! || goto :failure
msbuild dokan.sln /p:Configuration="Win8.1 Release" /p:Platform=Win32 /t:Build !CI_BUILD_ARG! || goto :failure
msbuild dokan.sln /p:Configuration="Win8.1 Release" /p:Platform=x64 /t:Build !CI_BUILD_ARG! || goto :failure
msbuild dokan.sln /p:Configuration="Win8.1 Release" /p:Platform=ARM /t:Build !CI_BUILD_ARG! || goto :failure
msbuild dokan.sln /p:Configuration="Win10 Release" /p:Platform=Win32 /t:Build !CI_BUILD_ARG! || goto :failure
msbuild dokan.sln /p:Configuration="Win10 Release" /p:Platform=x64 /t:Build !CI_BUILD_ARG! || goto :failure
msbuild dokan.sln /p:Configuration="Win10 Release" /p:Platform=ARM /t:Build !CI_BUILD_ARG! || goto :failure
msbuild dokan.sln /p:Configuration="Win10 Release" /p:Platform=ARM64 /t:Build !CI_BUILD_ARG! || goto :failure

msbuild dokan.sln /p:Configuration=Debug /p:Platform=Win32 /t:Build !CI_BUILD_ARG! || goto :failure
msbuild dokan.sln /p:Configuration=Debug /p:Platform=x64 /t:Build !CI_BUILD_ARG! || goto :failure
msbuild dokan.sln /p:Configuration=Debug /p:Platform=ARM /t:Build !CI_BUILD_ARG! || goto :failure
msbuild dokan.sln /p:Configuration=Debug /p:Platform=ARM64 /t:Build !CI_BUILD_ARG! || goto :failure
msbuild dokan.sln /p:Configuration="Win7 Debug" /p:Platform=Win32 /t:Build !CI_BUILD_ARG! || goto :failure
msbuild dokan.sln /p:Configuration="Win7 Debug" /p:Platform=x64 /t:Build !CI_BUILD_ARG! || goto :failure
msbuild dokan.sln /p:Configuration="Win8 Debug" /p:Platform=Win32 /t:Build !CI_BUILD_ARG! || goto :failure
msbuild dokan.sln /p:Configuration="Win8 Debug" /p:Platform=x64 /t:Build !CI_BUILD_ARG! || goto :failure
msbuild dokan.sln /p:Configuration="Win8 Debug" /p:Platform=ARM /t:Build !CI_BUILD_ARG! || goto :failure
msbuild dokan.sln /p:Configuration="Win8.1 Debug" /p:Platform=Win32 /t:Build !CI_BUILD_ARG! || goto :failure
msbuild dokan.sln /p:Configuration="Win8.1 Debug" /p:Platform=x64 /t:Build !CI_BUILD_ARG! || goto :failure
msbuild dokan.sln /p:Configuration="Win8.1 Debug" /p:Platform=ARM /t:Build !CI_BUILD_ARG! || goto :failure
msbuild dokan.sln /p:Configuration="Win10 Debug" /p:Platform=Win32 /t:Build !CI_BUILD_ARG! || goto :failure
msbuild dokan.sln /p:Configuration="Win10 Debug" /p:Platform=x64 /t:Build !CI_BUILD_ARG! || goto :failure
msbuild dokan.sln /p:Configuration="Win10 Debug" /p:Platform=ARM /t:Build !CI_BUILD_ARG! || goto :failure
msbuild dokan.sln /p:Configuration="Win10 Debug" /p:Platform=ARM64 /t:Build !CI_BUILD_ARG! || goto :failure

IF EXIST C:\cygwin64 ( Powershell.exe -executionpolicy remotesigned -File dokan_fuse/build.ps1 || goto :failure ) ELSE ( echo "Cygwin/Msys2 build disabled" )

goto :EOF

:failure
echo At least one build-command failed. The last command that failed returned with error %errorlevel% 1>&2
exit /b %errorlevel%

