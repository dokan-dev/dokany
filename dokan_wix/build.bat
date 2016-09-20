set PATH=%PATH%;%PROGRAMFILES(x86)%\MSBuild\14.0\Bin
set VCTargetsPath=%PROGRAMFILES%
IF %processor_architecture%==AMD64 set VCTargetsPath=%PROGRAMFILES(x86)%
set VCTargetsPath=%VCTargetsPath%\MSBuild\Microsoft.Cpp\v4.0\V140

REM set version info, edit version.txt before running the batch
if NOT exist SetAssemblyVersion\bin\Release\SetAssemblyVersion.exe (
	msbuild SetAssemblyVersion.sln /p:Configuration=Release /p:Platform="Any CPU" /t:rebuild 
)
SetAssemblyVersion\bin\Release\SetAssemblyVersion ..\CHANGELOG.md version.xml ..\

cd ..
call .\build.bat
Powershell.exe -executionpolicy remotesigned -File sign.ps1

MakeCab /f dokanx64.ddf
MakeCab /f dokanx86.ddf

set /p DUMMY=Please submit drivers to sysdev portal. Hit ENTER when it is done...

cd dokan_wix

IF EXIST C:\cygwin ( powershell -Command "(gc version.xml) -replace 'BuildCygwin=\"false\"', 'BuildCygwin=\"true\"' | sc version.xml" ) ELSE ( powershell -Command "(gc version.xml) -replace 'BuildCygwin=\"true\"', 'BuildCygwin=\"false\"' | sc version.xml" )

REM build light installer
powershell -Command "(gc version.xml) -replace 'Compressed=\"yes\"', 'Compressed=\"no\"' | sc version.xml"
msbuild Dokan_WiX.sln /p:Configuration=Release /p:Platform="Mixed Platforms" /t:rebuild /fileLogger
copy Bootstrapper\bin\Release\DokanSetup.exe .
copy bin\x64\Release\Dokan_x64.msi .
copy bin\x86\Release\Dokan_x86.msi .
msbuild Dokan_WiX.sln /p:Configuration=Debug /p:Platform="Mixed Platforms" /t:rebuild /fileLogger
copy Bootstrapper\bin\Debug\DokanSetup.exe DokanSetupDbg.exe

REM build full installer
powershell -Command "(gc version.xml) -replace 'Compressed=\"no\"', 'Compressed=\"yes\"' | sc version.xml"
msbuild Dokan_WiX.sln /p:Configuration=Release /p:Platform="Mixed Platforms" /t:rebuild /fileLogger
copy Bootstrapper\bin\Release\DokanSetup.exe DokanSetup_redist.exe
msbuild Dokan_WiX.sln /p:Configuration=Debug /p:Platform="Mixed Platforms" /t:rebuild /fileLogger
copy Bootstrapper\bin\Debug\DokanSetup.exe DokanSetupDbg_redist.exe

REM build archive
"C:\Program Files\7-Zip\7z.exe" a -tzip dokan.zip ../Win32 ../x64