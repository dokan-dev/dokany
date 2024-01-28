param($ver)

$ErrorActionPreference = 'Stop'

.\package.ps1 $ver

Set-Location .\build
choco push "dokany2.${ver}.nupkg" --api-key="${CHOCO_API_KEY}" --source="https://push.chocolatey.org/"
Set-Location ..
