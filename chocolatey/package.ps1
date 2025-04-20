param($version)

$ErrorActionPreference = 'Stop'

Remove-Item .\chocolatey\build -Recurse -Force -ErrorAction SilentlyContinue

New-Item .\chocolatey\build -Force -ItemType Directory
Copy-Item .\chocolatey\dokany.nuspec.template .\chocolatey\build\dokany.nuspec
Copy-Item .\chocolatey\tools .\chocolatey\build\ -Recurse
Copy-Item .\dokan_wix\Dokan_x64.msi .\chocolatey\build\tools\Dokan_x64.msi
Copy-Item .\dokan_wix\Dokan_x86.msi .\chocolatey\build\tools\Dokan_x86.msi
Rename-Item .\chocolatey\build\tools\chocolateyinstall.ps1.template chocolateyinstall.ps1

(Get-Content -Encoding UTF8 .\chocolatey\build\dokany.nuspec).Replace('[[PackageVersion]]', $version) | Set-Content -Encoding UTF8 .\chocolatey\build\dokany.nuspec

choco pack .\chocolatey\build\dokany.nuspec --out .\chocolatey\build
