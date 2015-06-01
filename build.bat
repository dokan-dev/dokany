REM Build release to prepare for packaging

REM Make sure MSBUILD is available
set PATH=%PATH%;%WINDIR%\Microsoft.NET\Framework\v4.0.30319\
set VCTargetsPath=%PROGRAMFILES%
IF %processor_architecture%==AMD64 set VCTargetsPath=%PROGRAMFILES(x86)%
set VCTargetsPath=%VCTargetsPath%\MSBuild\Microsoft.Cpp\v4.0\V120

REM Build
msbuild dokan.sln /p:Configuration=Release /p:Platform=Win32 /t:Build
msbuild dokan.sln /p:Configuration=Release /p:Platform=x64 /t:Build
msbuild dokan.sln /p:Configuration="Win7 Release" /p:Platform=Win32 /t:Build
msbuild dokan.sln /p:Configuration="Win7 Release" /p:Platform=x64 /t:Build
msbuild dokan.sln /p:Configuration="Win8 Release" /p:Platform=Win32 /t:Build
msbuild dokan.sln /p:Configuration="Win8 Release" /p:Platform=x64 /t:Build
msbuild dokan.sln /p:Configuration="Win8.1 Release" /p:Platform=Win32 /t:Build
msbuild dokan.sln /p:Configuration="Win8.1 Release" /p:Platform=x64 /t:Build