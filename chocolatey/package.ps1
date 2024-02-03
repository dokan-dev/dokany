param($version)

$ErrorActionPreference = 'Stop'

Remove-Item .\chocolatey\build -Recurse -Force -ErrorAction SilentlyContinue

New-Item .\chocolatey\build -Force -ItemType Directory
Copy-Item .\chocolatey\dokany.nuspec.template .\chocolatey\build\dokany.nuspec
Copy-Item .\chocolatey\tools .\chocolatey\build\ -Recurse
Rename-Item .\chocolatey\build\tools\chocolateyinstall.ps1.template chocolateyinstall.ps1

(Get-Content -Encoding UTF8 .\chocolatey\build\dokany.nuspec).Replace('[[PackageVersion]]', $version) | Set-Content -Encoding UTF8 .\chocolatey\build\dokany.nuspec

function Hash {
    param (
        $path
    )
    $hash = Get-FileHash $path -Algorithm SHA256
    # return hex string
    return $hash.Hash
}

$hash64 = Hash .\dokan_wix\Dokan_x64.msi
$hash32 = Hash .\dokan_wix\Dokan_x86.msi

$install = (Get-Content .\chocolatey\build\tools\chocolateyinstall.ps1)
$install = $install.Replace('[[Url]]', $url32).Replace('[[Checksum]]', $hash32)
$install = $install.Replace('[[Url64]]', $url64).Replace('[[Checksum64]]', $hash64)
Set-Content -Encoding UTF8 .\chocolatey\build\tools\chocolateyinstall.ps1 -Value $install

choco pack .\chocolatey\build\dokany.nuspec --out .\chocolatey\build
