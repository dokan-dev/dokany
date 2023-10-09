param($ver)

$ErrorActionPreference = 'Stop'

Remove-Item build -Recurse -Force -ErrorAction SilentlyContinue

New-Item ./build -Force -ItemType Directory
Copy-Item dokany.nuspec.template build/dokany.nuspec
Copy-Item tools build/ -Recurse
Rename-Item build/tools/chocolateyinstall.ps1.template chocolateyinstall.ps1

(Get-Content build/dokany.nuspec).Replace('[[PackageVersion]]', $ver) | Set-Content build/dokany.nuspec

$url64 = "https://github.com/dokan-dev/dokany/releases/download/v${ver}/Dokan_x64.msi"
$url32 = "https://github.com/dokan-dev/dokany/releases/download/v${ver}/Dokan_x86.msi"

function Hash {
    param (
        $url
    )
    Invoke-WebRequest $url -OutFile build/dokany.msi
    $hash = Get-FileHash build/dokany.msi -Algorithm SHA256
    # return hex string
    return $hash.Hash
}

$hash64 = Hash $url64
$hash32 = Hash $url32

$install = (Get-Content build/tools/chocolateyinstall.ps1)
$install = $install.Replace('[[Url]]', $url32).Replace('[[Checksum]]', $hash32)
$install = $install.Replace('[[Url64]]', $url64).Replace('[[Checksum64]]', $hash64)
Set-Content build/tools/chocolateyinstall.ps1 -Value $install

Set-Location .\build
choco pack
Set-Location ..
