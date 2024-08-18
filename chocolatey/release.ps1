param($version)

$ErrorActionPreference = 'Stop'

.\chocolatey\package.ps1 $version

choco push ".\chocolatey\build\dokany2.${version}.nupkg" --api-key="$env:CHOCO_API_KEY" --source="https://push.chocolatey.org/"