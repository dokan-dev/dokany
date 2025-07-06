param (
	[Parameter(Mandatory=$true)][string]$SONAR_TOKEN,
	[Parameter(Mandatory=$true)][string]$BUILD_VERSION
)

$ErrorActionPreference = "Stop"

$env:SONAR_TOKEN = $SONAR_TOKEN

$env:SONAR_SCANNER_VERSION = "7.0.2.4839"
$env:SONAR_DIRECTORY = [System.IO.Path]::Combine($(get-location).Path,".sonar")
$env:SONAR_SCANNER_HOME = "$env:SONAR_DIRECTORY/sonar-scanner-$env:SONAR_SCANNER_VERSION-windows-x64"
rm $env:SONAR_SCANNER_HOME -Force -Recurse -ErrorAction SilentlyContinue
New-Item -path $env:SONAR_SCANNER_HOME -type directory
(New-Object System.Net.WebClient).DownloadFile("https://binaries.sonarsource.com/Distribution/sonar-scanner-cli/sonar-scanner-cli-$env:SONAR_SCANNER_VERSION-windows-x64.zip", "$env:SONAR_DIRECTORY/sonar-scanner.zip")
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::ExtractToDirectory("$env:SONAR_DIRECTORY/sonar-scanner.zip", "$env:SONAR_DIRECTORY")
rm ./.sonar/sonar-scanner.zip -Force -ErrorAction SilentlyContinue
$env:SONAR_SCANNER_OPTS="-server"

rm "$env:SONAR_DIRECTORY/build-wrapper-win-x86" -Force -Recurse -ErrorAction SilentlyContinue
(New-Object System.Net.WebClient).DownloadFile("https://sonarcloud.io/static/cpp/build-wrapper-win-x86.zip", "$env:SONAR_DIRECTORY/build-wrapper-win-x86.zip")
[System.IO.Compression.ZipFile]::ExtractToDirectory("$env:SONAR_DIRECTORY/build-wrapper-win-x86.zip", "$env:SONAR_DIRECTORY")

& $env:SONAR_DIRECTORY/build-wrapper-win-x86/build-wrapper-win-x86-64.exe --out-dir bw-output .\scripts\build.ps1 -BuildPart win -Platforms x64 -Configurations Debug

& $env:SONAR_SCANNER_HOME/bin/sonar-scanner.bat `
  -D"sonar.organization=dokan-dev" `
  -D"sonar.projectKey=dokany" `
  -D"sonar.projectVersion=$BUILD_VERSION" `
  -D"sonar.sources=." `
  -D"sonar.cfamily.compile-commands=bw-output/compile_commands.json" `
  -D"sonar.host.url=https://sonarcloud.io"
