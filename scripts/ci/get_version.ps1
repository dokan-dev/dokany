<#
.SYNOPSIS
    Prints the installer version for a CI build: <BaseVersion>.<BuildVersion + BuildNumber>.

.EXAMPLE
    scripts\ci\get_version.ps1 -BuildNumber 1042   # -> 3.0.0.2042 (BaseVersion 3.0.0, BuildVersion 1000)
#>
param(
    [Parameter(Mandatory = $true)]
    [ValidateRange(0, 65535)]
    [int]$BuildNumber
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$versionXml = [xml](Get-Content (Join-Path $repoRoot 'dokan_wix\version.xml'))

function Get-VersionDefine([string]$Name) {
    $define = $versionXml.Include.define | Where-Object { $_.StartsWith("$Name=") } | Select-Object -First 1
    if (-not $define) {
        throw "Define '$Name' not found in dokan_wix\version.xml"
    }
    return ($define.Split('=')[1] -replace '"', '').Trim()
}

$baseVersion = Get-VersionDefine 'BaseVersion'
$baseBuild = [int](Get-VersionDefine 'BuildVersion')
$revision = $baseBuild + $BuildNumber
if ($revision -lt 0 -or $revision -gt 65535) {
    throw "Computed revision $revision ($baseBuild + $BuildNumber) is outside the 16-bit PE/MSI limit (0..65535)."
}
$version = "$baseVersion.$($revision.ToString('0000'))"
if ($version -notmatch '^\d+\.\d+\.\d+\.\d+$') {
    throw "Computed version '$version' is not a valid 4-part version."
}
Write-Output $version
