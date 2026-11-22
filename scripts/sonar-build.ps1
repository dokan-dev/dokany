param (
	[Parameter(Mandatory=$true)][string]$BUILD_VERSION
)

$ErrorActionPreference = "Stop"

$env:SONAR_SCANNER_VERSION = "8.5.0.16571"
$env:SONAR_DIRECTORY = [System.IO.Path]::Combine($(get-location).Path,".sonar")
$env:SONAR_SCANNER_HOME = "$env:SONAR_DIRECTORY/sonar-scanner-$env:SONAR_SCANNER_VERSION-windows-x64"
New-Item -Force -Path $env:SONAR_DIRECTORY -Type Directory | Out-Null
rm $env:SONAR_SCANNER_HOME -Force -Recurse -ErrorAction SilentlyContinue
(New-Object System.Net.WebClient).DownloadFile("https://binaries.sonarsource.com/Distribution/sonar-scanner-cli/sonar-scanner-cli-$env:SONAR_SCANNER_VERSION-windows-x64.zip", "$env:SONAR_DIRECTORY/sonar-scanner.zip")
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::ExtractToDirectory("$env:SONAR_DIRECTORY/sonar-scanner.zip", "$env:SONAR_DIRECTORY")
rm "$env:SONAR_DIRECTORY/sonar-scanner.zip" -Force -ErrorAction SilentlyContinue
$env:SONAR_SCANNER_OPTS="-server"

rm "$env:SONAR_DIRECTORY/build-wrapper-win-x86" -Force -Recurse -ErrorAction SilentlyContinue
(New-Object System.Net.WebClient).DownloadFile("https://sonarcloud.io/static/cpp/build-wrapper-win-x86.zip", "$env:SONAR_DIRECTORY/build-wrapper-win-x86.zip")
[System.IO.Compression.ZipFile]::ExtractToDirectory("$env:SONAR_DIRECTORY/build-wrapper-win-x86.zip", "$env:SONAR_DIRECTORY")
rm "$env:SONAR_DIRECTORY/build-wrapper-win-x86.zip" -Force -ErrorAction SilentlyContinue

rm bw-output -Force -Recurse -ErrorAction SilentlyContinue

function buildWrapper {
	& $env:SONAR_DIRECTORY/build-wrapper-win-x86/build-wrapper-win-x86-64.exe --out-dir bw-output msbuild /nodeReuse:False $args
}

.\scripts\build.ps1 -BuildPart win -Platforms x64 -Configurations Debug

$compileCommands = "bw-output/compile_commands.json"
if (!(Test-Path -Path $compileCommands -PathType Leaf) -or (Get-Item -Path $compileCommands).Length -eq 0) {
	throw "Sonar build-wrapper did not generate a non-empty $compileCommands"
}

$sonarScannerBat = "$env:SONAR_SCANNER_HOME/bin/sonar-scanner.bat"
if (!(Test-Path -Path $sonarScannerBat -PathType Leaf)) {
	throw "SonarScanner CLI not found at $sonarScannerBat"
}

# Allow non-fatal analyzer stderr without aborting PowerShell; enforce exit code explicitly.
$ErrorActionPreference = "Continue"
try {
	& $sonarScannerBat `
	  -D"sonar.organization=dokan-dev" `
	  -D"sonar.projectKey=dokany" `
	  -D"sonar.projectVersion=$BUILD_VERSION" `
	  -D"sonar.sources=." `
	  -D"sonar.exclusions=.sonar/**,bw-output/**,packages/**,samples/dokan_memfs/spdlog/**" `
	  -D"sonar.cfamily.compile-commands=$compileCommands" `
	  -D"sonar.host.url=https://sonarcloud.io"
	$scannerExitCode = $LASTEXITCODE
} finally {
	$ErrorActionPreference = "Stop"
}
if ($scannerExitCode -ne 0) {
	throw "sonar-scanner failed with exit code $scannerExitCode"
}
