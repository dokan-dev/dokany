@echo off
setlocal ENABLEDELAYEDEXPANSION
REM Build release to prepare for packaging

REM Make sure MSBUILD is available
REM (the -sort option reverses the sort order. If/when dokany moves to VS2019, this will need to be changed.  The logic will need to be revisited when a VS version later than 2019 comes out.)
FOR /f "delims=" %%A IN (
	'"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -property installationPath -sort'
) DO SET "VS_PATH=%%A"

SET MSBUILD_BIN_PATH="%VS_PATH%\MSBuild\15.0\Bin"
IF NOT EXIST "%VS_PATH%" (
	ECHO Visual C++ 2017 NOT Installed.
	PAUSE
	EXIT /B
)

rem This is where you should set the environment variables needed for integration with SonarQube.
IF EXIST "sonarvars.bat" (
	CALL sonarvars.bat
)

rem Seriously, the space after the openparen must be there to separate the tokens, and there must be no space between the end of the SET values and the closeparen. Otherwise, the trailing space
rem gets put into the value of the environment variable being set, which causes all the string equality tests to fail.
IF NOT "%SONARBUILD%"=="" ( SET shouldsonar=true)	
IF NOT "%SONAR_KEY%"==""  ( SET shouldsonar=true)
IF NOT "%shouldsonar%"=="true" goto skipsonar 
	ECHO Building using SonarBuild. Environment variables which must be set are:
	ECHO SONAR_KEY:				The parameter to /k: for user-space components (%SONAR_KEY%)
	ECHO SONAR_KEY_SYS:			The parameter to /k: for kernel-mode components (%SONAR_KEY_SYS%)
	ECHO SONAR_LOGIN:			The parameter to /d:sonar.login= (%SONAR_LOGIN%)
	ECHO SONAR_ORGANIZATION:	The parameter to /d:sonar.organization= (%SONAR_ORGANIZATION%)
	ECHO SONAR_HOST_URL:		The parameter to /d:sonar.host.url= (%SONAR_HOST_URL%)
	ECHO Also, both the Build Wrapper for Windows and Scanner for MSBuild must be located in the PATH.
	SET SONAR_CFAMILY_BUILDWRAPPEROUTPUT=bw-output
	SET SONAR_WRAPPER=build-wrapper-win-x86-64.exe
	SET BUILD_WRAPPER=!SONAR_WRAPPER! --out-dir !SONAR_CFAMILY_BUILDWRAPPEROUTPUT!
	SET SONAR_SCANNER=SonarScanner.MSBuild.exe


	where /q %SONAR_WRAPPER%
	if ERRORLEVEL 1 ( 
		echo Cannot find the build wrapper %SONAR_WRAPPER%
		set BUILD_WRAPPER=)
	where /q %SONAR_SCANNER%
	if ERRORLEVEL 1 (
		echo Cannot find the Sonar Scanner %SONAR_SCANNER%
		set BUILD_WRAPPER=)
	
	if "%SONAR_KEY%"=="" ( set BUILD_WRAPPER=)
	if "%SONAR_LOGIN%"=="" ( set BUILD_WRAPPER=)
	if "%SONAR_ORGANIZATION%"=="" ( set BUILD_WRAPPER=)
	if "%SONAR_HOST_URL%"=="" ( set BUILD_WRAPPER=)
	IF "%BUILD_WRAPPER%"=="" (
		echo Proceeding WITHOUT SonarQube analysis
	)
	if NOT "%BUILD_WRAPPER%"=="" (
		echo Everything looks good. SonarQube analysis is enabled for this build.
	)
rem Clean up after ourselves
	SET shouldsonar=
	
:skipsonar

set PATH=%PATH%;%MSBUILD_BIN_PATH%

REM Enable AppVeyor build message logging if running under AppVeyor
IF "%APPVEYOR%"=="True" set CI_BUILD_ARG="/l:C:\Program Files\AppVeyor\BuildAgent\Appveyor.MSBuildLogger.dll"
REM Set up SonarQube user-mode analysis if BUILD_WRAPPER environment variable set
IF NOT "%BUILD_WRAPPER%"=="" (
	%SONAR_SCANNER% begin /k:"%SONAR_KEY%" /d:sonar.login="%SONAR_LOGIN%" /d:sonar.organization="%SONAR_ORGANIZATION%" /d:sonar.host.url="%SONAR_HOST_URL%" /d:sonar.cfamily.build-wrapper-output=!SONAR_CFAMILY_BUILDWRAPPEROUTPUT!
)
REM Build
%BUILD_WRAPPER% msbuild dokan.sln /p:Configuration=Release;Platform=x64 /t:Rebuild !CI_BUILD_ARG! || goto buildfailed
%BUILD_WRAPPER% msbuild dokan.sln /p:Configuration=Release;Platform=Win32 /t:Rebuild !CI_BUILD_ARG! || goto buildfailed
%BUILD_WRAPPER% msbuild dokan.sln /p:Configuration=Release;Platform=ARM /t:Rebuild !CI_BUILD_ARG! || goto buildfailed
%BUILD_WRAPPER% msbuild dokan.sln /p:Configuration=Release;Platform=ARM64 /t:Rebuild !CI_BUILD_ARG! || goto buildfailed
%BUILD_WRAPPER% msbuild dokan.sln /p:Configuration=Debug;Platform=x64 /t:Rebuild !CI_BUILD_ARG! || goto buildfailed
%BUILD_WRAPPER% msbuild dokan.sln /p:Configuration=Debug;Platform=Win32 /t:Rebuild !CI_BUILD_ARG! || goto buildfailed
%BUILD_WRAPPER% msbuild dokan.sln /p:Configuration=Debug;Platform=ARM /t:Rebuild !CI_BUILD_ARG! || goto buildfailed
%BUILD_WRAPPER% msbuild dokan.sln /p:Configuration=Debug;Platform=ARM64 /t:Rebuild !CI_BUILD_ARG! || goto buildfailed
goto usermodedone

:buildfailed
SET failedERROR=%ERRORLEVEL%
SET buildfailed=true

:usermodedone
REM End user-space wrapper
IF NOT "%BUILD_WRAPPER%"=="" (
	%SONAR_SCANNER% end /d:sonar.login="%SONAR_LOGIN%"
)

IF "%buildfailed%"=="true" ( goto buildfailed)


REM Set up SonarQube kernel-mode analysis if SONAR_KEY_SYS environment variable set
IF NOT "%BUILD_WRAPPER%"=="" (
	IF NOT "%SONAR_KEY_SYS%"=="" (
		%SONAR_SCANNER% begin /k:"%SONAR_KEY_SYS%" /d:sonar.login="%SONAR_LOGIN%" /d:sonar.organization="%SONAR_ORGANIZATION%" /d:sonar.host.url="%SONAR_HOST_URL%" /d:sonar.cfamily.build-wrapper-output=!SONAR_CFAMILY_BUILDWRAPPEROUTPUT!
	)
)

%BUILD_WRAPPER% msbuild dokan.sln /p:Configuration="Win10 Release";Platform=x64 /t:Rebuild !CI_BUILD_ARG! || goto buildfailedsys
%BUILD_WRAPPER% msbuild dokan.sln /p:Configuration="Win10 Release";Platform=Win32 /t:Rebuild !CI_BUILD_ARG! || goto buildfailedsys
%BUILD_WRAPPER% msbuild dokan.sln /p:Configuration="Win10 Release";Platform=ARM /t:Rebuild !CI_BUILD_ARG! || goto buildfailedsys
%BUILD_WRAPPER% msbuild dokan.sln /p:Configuration="Win10 Release";Platform=ARM64 /t:Rebuild !CI_BUILD_ARG! || goto buildfailedsys
%BUILD_WRAPPER% msbuild dokan.sln /p:Configuration="Win7 Release";Platform=x64 /t:Rebuild !CI_BUILD_ARG! || goto buildfailedsys
%BUILD_WRAPPER% msbuild dokan.sln /p:Configuration="Win7 Release";Platform=Win32 /t:Rebuild !CI_BUILD_ARG! || goto buildfailedsys
%BUILD_WRAPPER% msbuild dokan.sln /p:Configuration="Win8 Release";Platform=x64 /t:Rebuild !CI_BUILD_ARG! || goto buildfailedsys
%BUILD_WRAPPER% msbuild dokan.sln /p:Configuration="Win8 Release";Platform=Win32 /t:Rebuild !CI_BUILD_ARG! || goto buildfailedsys
%BUILD_WRAPPER% msbuild dokan.sln /p:Configuration="Win8 Release";Platform=ARM /t:Rebuild !CI_BUILD_ARG! || goto buildfailedsys
%BUILD_WRAPPER% msbuild dokan.sln /p:Configuration="Win8.1 Release";Platform=x64 /t:Rebuild !CI_BUILD_ARG! || goto buildfailedsys
%BUILD_WRAPPER% msbuild dokan.sln /p:Configuration="Win8.1 Release";Platform=Win32 /t:Rebuild !CI_BUILD_ARG! || goto buildfailedsys
%BUILD_WRAPPER% msbuild dokan.sln /p:Configuration="Win8.1 Release";Platform=ARM /t:Rebuild !CI_BUILD_ARG! || goto buildfailedsys

%BUILD_WRAPPER% msbuild dokan.sln /p:Configuration="Win7 Debug";Platform=x64 /t:Rebuild !CI_BUILD_ARG! || goto buildfailedsys
%BUILD_WRAPPER% msbuild dokan.sln /p:Configuration="Win7 Debug";Platform=Win32 /t:Rebuild !CI_BUILD_ARG! || goto buildfailedsys
%BUILD_WRAPPER% msbuild dokan.sln /p:Configuration="Win8 Debug";Platform=x64 /t:Rebuild !CI_BUILD_ARG! || goto buildfailedsys
%BUILD_WRAPPER% msbuild dokan.sln /p:Configuration="Win8 Debug";Platform=Win32 /t:Rebuild !CI_BUILD_ARG! || goto buildfailedsys
%BUILD_WRAPPER% msbuild dokan.sln /p:Configuration="Win8 Debug";Platform=ARM /t:Rebuild !CI_BUILD_ARG! || goto buildfailedsys
%BUILD_WRAPPER% msbuild dokan.sln /p:Configuration="Win8.1 Debug";Platform=x64 /t:Rebuild !CI_BUILD_ARG! || goto buildfailedsys
%BUILD_WRAPPER% msbuild dokan.sln /p:Configuration="Win8.1 Debug";Platform=Win32 /t:Rebuild !CI_BUILD_ARG! || goto buildfailedsys
%BUILD_WRAPPER% msbuild dokan.sln /p:Configuration="Win8.1 Debug";Platform=ARM /t:Rebuild !CI_BUILD_ARG! || goto buildfailedsys
%BUILD_WRAPPER% msbuild dokan.sln /p:Configuration="Win10 Debug";Platform=x64 /t:Rebuild !CI_BUILD_ARG! || goto buildfailedsys
%BUILD_WRAPPER% msbuild dokan.sln /p:Configuration="Win10 Debug";Platform=Win32 /t:Rebuild !CI_BUILD_ARG! || goto buildfailedsys
%BUILD_WRAPPER% msbuild dokan.sln /p:Configuration="Win10 Debug";Platform=ARM /t:Rebuild !CI_BUILD_ARG! || goto buildfailedsys
%BUILD_WRAPPER% msbuild dokan.sln /p:Configuration="Win10 Debug";Platform=ARM64 /t:Rebuild !CI_BUILD_ARG! || goto buildfailedsys
:buildfailedsys
set failedERROR=%ERRORLEVEL%
set buildfailed=true

REM Finish SonarQube analysis, if BUILD_WRAPPER environment variable is set
IF NOT "%BUILD_WRAPPER%"=="" (
	%SONAR_SCANNER% end /d:sonar.login="%SONAR_LOGIN%"
)
	
IF EXIST C:\cygwin64 ( Powershell.exe -executionpolicy remotesigned -File dokan_fuse/build.ps1 || goto buildfailed ) ELSE ( echo "Cygwin/Msys2 build disabled" )
goto end

:buildfailed

@if !failed! neq 0 (
    echo A build-command failed.
	echo The command that failed returned with error !failedERROR! 1>&2
    exit /b !failedERROR!
)

:end
ENDLOCAL
