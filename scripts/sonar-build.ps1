param (
	[Parameter(Mandatory=$true)][string]$SONAR_KEY,
	[Parameter(Mandatory=$true)][string]$SONAR_LOGIN,
	[Parameter(Mandatory=$true)][string]$SONAR_ORGANIZATION,
	[Parameter(Mandatory=$true)][string]$SONAR_HOST_URL,
	[Parameter(Mandatory=$true)][string]$BUILD_VERSION
)

. .\scripts\build_helper.ps1

$ErrorActionPreference = "Stop"
 
$SONAR_CFAMILY_BUILDWRAPPEROUTPUT="bw-output"
set-alias sonar-scanner "SonarScanner.MSBuild.exe"

function buildWrapper {
	& build-wrapper-win-x86-64.exe --out-dir $SONAR_CFAMILY_BUILDWRAPPEROUTPUT msbuild $args
}

Exec-External { sonar-scanner begin /k:"$SONAR_KEY" /v:"$BUILD_VERSION" /d:sonar.login="$SONAR_LOGIN" /o:"$SONAR_ORGANIZATION" /d:sonar.host.url="$SONAR_HOST_URL" /d:sonar.cfamily.build-wrapper-output=$SONAR_CFAMILY_BUILDWRAPPEROUTPUT }
Exec-External { .\scripts\build.ps1 -BuildPart win -Platforms x64 -Configurations Debug }
Exec-External { sonar-scanner end /d:sonar.login="$SONAR_LOGIN" }
