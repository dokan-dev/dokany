<#
.SYNOPSIS
    Builds, test-signs and packages Dokan for CI (replaces the former AppVeyor "All" job).

.DESCRIPTION
    - Builds Win32/x64/ARM64 (and Cygwin + MSYS2 dokan_fuse when the toolchains are installed).
    - Temporarily imports and test-signs all binaries with the public Dokan test certificate
      (cert/DokanSign.pfx), removing the imported test certificates in a finally block.
    - Builds the WiX installer bundle (DokanSetup.exe).
    - Stages:
        * $ReleaseDir: DokanSetup-<version>.exe, dokan-<version>.zip
        * $PayloadDir: what the VM test jobs need (DokanSetup.exe, dokan3.sys and dokan3.pdb for crash analysis)
    The test tools (dokan_fstest.exe, fsx.exe) are built in parallel by build_test_tools.ps1.
#>
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [Parameter(Mandatory = $true)]
    [int]$BuildNumber,
    [string]$PayloadDir = 'ci-payload',
    [string]$ReleaseDir = 'ci-release',
    # Fail instead of silently skipping dokan_fuse when Cygwin/MSYS2 toolchains are missing.
    [switch]$RequireFuse,
    # Opt in to importing cert\DokanCA.cer into LocalMachine\Root outside CI (CI=true).
    [switch]$InstallTestCert,
    # Optional filename prefix for staged release artifacts (e.g. 'pr-42-' on pull requests).
    [string]$ArtifactPrefix = ''
)

$ErrorActionPreference = 'Stop'

if ($ArtifactPrefix -notmatch '^[A-Za-z0-9_.-]*$') {
    throw "Invalid ArtifactPrefix '$ArtifactPrefix': only ASCII letters, digits, '_', '.', and '-' are allowed."
}

function Convert-PeRvaToFileOffset {
    param(
        [Parameter(Mandatory = $true)][uint32]$Rva,
        [Parameter(Mandatory = $true)][byte[]]$Bytes,
        [Parameter(Mandatory = $true)][int]$SectionTableOffset,
        [Parameter(Mandatory = $true)][uint16]$NumberOfSections
    )

    if ($Rva -eq 0) { return -1 }
    for ($i = 0; $i -lt $NumberOfSections; $i++) {
        $secOff = $SectionTableOffset + ($i * 40)
        if ($secOff + 40 -gt $Bytes.Length) { break }
        $vAddr = [BitConverter]::ToUInt32($Bytes, $secOff + 12)
        $rawSize = [BitConverter]::ToUInt32($Bytes, $secOff + 16)
        $rawPtr = [BitConverter]::ToUInt32($Bytes, $secOff + 20)
        if ($rawSize -eq 0 -or ([int64]$rawPtr + [int64]$rawSize) -gt $Bytes.Length) { continue }
        if ($Rva -ge $vAddr -and ($Rva - $vAddr) -lt $rawSize) {
            $off = [int64]$rawPtr + ([int64]$Rva - [int64]$vAddr)
            return [int]$off
        }
    }
    return -1
}

function Read-PeAsciiStringAtRva {
    param(
        [Parameter(Mandatory = $true)][uint32]$Rva,
        [Parameter(Mandatory = $true)][byte[]]$Bytes,
        [Parameter(Mandatory = $true)][int]$SectionTableOffset,
        [Parameter(Mandatory = $true)][uint16]$NumberOfSections
    )

    $off = Convert-PeRvaToFileOffset -Rva $Rva -Bytes $Bytes -SectionTableOffset $SectionTableOffset -NumberOfSections $NumberOfSections
    if ($off -lt 0) {
        throw "Invalid PE string RVA 0x$($Rva.ToString('X8'))."
    }
    $end = $off
    while ($end -lt $Bytes.Length -and $Bytes[$end] -ne 0) {
        $end++
    }
    if ($end -ge $Bytes.Length) {
        throw "Unterminated PE string at offset $off."
    }
    return [System.Text.Encoding]::ASCII.GetString($Bytes, $off, $end - $off)
}

function Get-DokanPEImportedDlls {
    param([Parameter(Mandatory = $true)][string]$BinaryPath)

    [byte[]]$bytes = [System.IO.File]::ReadAllBytes($BinaryPath)
    if ($bytes.Length -lt 64 -or $bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) {
        throw "$BinaryPath is not a valid PE file (missing MZ header)."
    }
    $lfanew = [int64][BitConverter]::ToInt32($bytes, 60)
    if ($lfanew -lt 0 -or ($lfanew + 24) -gt $bytes.Length) {
        throw "$BinaryPath has invalid e_lfanew ($lfanew)."
    }
    if ($bytes[$lfanew] -ne 0x50 -or $bytes[$lfanew + 1] -ne 0x45 -or $bytes[$lfanew + 2] -ne 0 -or $bytes[$lfanew + 3] -ne 0) {
        throw "$BinaryPath is missing PE signature."
    }

    $fileHeaderOff = [int]$lfanew + 4
    $numSections = [BitConverter]::ToUInt16($bytes, $fileHeaderOff + 2)
    $sizeOfOpt = [int][BitConverter]::ToUInt16($bytes, $fileHeaderOff + 16)
    $optOff = [int64]$fileHeaderOff + 20
    $secOff = $optOff + $sizeOfOpt
    if ($sizeOfOpt -lt 2 -or $secOff + ([int64]$numSections * 40) -gt $bytes.Length) {
        throw "$BinaryPath has truncated OptionalHeader or section headers."
    }

    $magic = [BitConverter]::ToUInt16($bytes, [int]$optOff)
    if ($magic -eq 0x010B) {
        # PE32
        $numRvaOff = $optOff + 92
        $dirOff = $optOff + 96
    } elseif ($magic -eq 0x020B) {
        # PE32+
        $numRvaOff = $optOff + 108
        $dirOff = $optOff + 112
    } else {
        throw "$BinaryPath has unsupported OptionalHeader magic 0x$($magic.ToString('X4'))."
    }
    if (($numRvaOff + 4) -gt $secOff) {
        throw "$BinaryPath OptionalHeader truncated before NumberOfRvaAndSizes."
    }
    $numRva = [BitConverter]::ToUInt32($bytes, [int]$numRvaOff)

    $imports = New-Object System.Collections.Generic.List[string]
    if ($numRva -gt 1 -and ($dirOff + 16) -le $secOff) {
        $impRva = [BitConverter]::ToUInt32($bytes, [int]$dirOff + 8)
        if ($impRva -ne 0) {
            $impOff = Convert-PeRvaToFileOffset -Rva $impRva -Bytes $bytes -SectionTableOffset ([int]$secOff) -NumberOfSections $numSections
            if ($impOff -lt 0) {
                throw "$BinaryPath has invalid Import Directory RVA."
            }
            while ($impOff + 20 -le $bytes.Length) {
                $origFirstThunk = [BitConverter]::ToUInt32($bytes, $impOff)
                $nameRva = [BitConverter]::ToUInt32($bytes, $impOff + 12)
                $firstThunk = [BitConverter]::ToUInt32($bytes, $impOff + 16)
                if ($origFirstThunk -eq 0 -and $nameRva -eq 0 -and $firstThunk -eq 0) {
                    break
                }
                if ($nameRva -eq 0) {
                    throw "$BinaryPath has malformed Import Descriptor with zero Name RVA."
                }
                $dllName = Read-PeAsciiStringAtRva -Rva $nameRva -Bytes $bytes -SectionTableOffset ([int]$secOff) -NumberOfSections $numSections
                $imports.Add($dllName)
                $impOff += 20
            }
        }
    }

    $delayImports = New-Object System.Collections.Generic.List[string]
    if ($numRva -gt 13 -and ($dirOff + (14 * 8)) -le $secOff) {
        $delayRva = [BitConverter]::ToUInt32($bytes, [int]$dirOff + (13 * 8))
        if ($delayRva -ne 0) {
            $delayOff = Convert-PeRvaToFileOffset -Rva $delayRva -Bytes $bytes -SectionTableOffset ([int]$secOff) -NumberOfSections $numSections
            if ($delayOff -lt 0) {
                throw "$BinaryPath has invalid Delay-Load Import Directory RVA."
            }
            while ($delayOff + 32 -le $bytes.Length) {
                $dllNameRva = [BitConverter]::ToUInt32($bytes, $delayOff + 4)
                if ($dllNameRva -eq 0) {
                    break
                }
                $dllName = Read-PeAsciiStringAtRva -Rva $dllNameRva -Bytes $bytes -SectionTableOffset ([int]$secOff) -NumberOfSections $numSections
                $delayImports.Add($dllName)
                $delayOff += 32
            }
        }
    }

    return @{
        Imports      = @($imports)
        DelayImports = @($delayImports)
    }
}

function Assert-DokanNpKernel32OnlyImports {
    param([Parameter(Mandatory = $true)][string]$BinaryPath)

    $pe = Get-DokanPEImportedDlls -BinaryPath $BinaryPath
    if ($pe.Imports.Count -ne 1 -or ($pe.Imports[0] -ine 'KERNEL32.dll')) {
        throw "PE import check failed for ${BinaryPath}: expected ONLY KERNEL32.dll, got [$($pe.Imports -join ', ')]."
    }
    if ($pe.DelayImports.Count -ne 0) {
        throw "PE delay-load import check failed for ${BinaryPath}: expected none, got [$($pe.DelayImports -join ', ')]."
    }
}

if (-not $InstallTestCert -and $env:CI -ne 'true') {
    throw 'Refusing to import cert\DokanCA.cer outside CI: importing DokanCA.cer trusts a public test Root CA in Cert:\LocalMachine\Root. Re-run with -InstallTestCert (or CI=true) to allow temporary test-certificate installation for this build.'
}

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $repoRoot

. .\scripts\build_helper.ps1
Add-VisualStudio-Path
$msbuild = (Get-Command msbuild.exe -CommandType Application | Select-Object -First 1).Source
Write-Host "Using MSBuild: $msbuild"

$sevenZip = (Get-Command 7z.exe -ErrorAction SilentlyContinue | Select-Object -First 1).Source
if (-not $sevenZip) {
    $sevenZip = 'C:\Program Files\7-Zip\7z.exe'
}
if (-not (Test-Path -LiteralPath $sevenZip -PathType Leaf)) {
    throw "7z.exe not found at $sevenZip"
}

$cygwinDir = if (-not [string]::IsNullOrWhiteSpace($env:CYGWIN_INST_DIR)) { $env:CYGWIN_INST_DIR } else { 'C:\cygwin64' }
$msys2Dir = if (-not [string]::IsNullOrWhiteSpace($env:MSYS2_INST_DIR)) { $env:MSYS2_INST_DIR } else { 'C:\msys64' }
$hasCygwin = Test-Path -LiteralPath (Join-Path $cygwinDir 'bin\bash.exe') -PathType Leaf
$hasMsys2 = (Test-Path -LiteralPath (Join-Path $msys2Dir 'ucrt64\bin\gcc.exe') -PathType Leaf) -and
    (Test-Path -LiteralPath (Join-Path $msys2Dir 'mingw32\bin\gcc.exe') -PathType Leaf) -and
    (Test-Path -LiteralPath (Join-Path $msys2Dir 'usr\bin\make.exe') -PathType Leaf)
if ($RequireFuse -and -not ($hasCygwin -and $hasMsys2)) {
    throw "Cygwin ($cygwinDir, found: $hasCygwin) and MSYS2 MINGW32/UCRT64 ($msys2Dir, found: $hasMsys2) toolchains are required for dokan_fuse."
}

# 1. Restore all NuGet packages BEFORE importing the public DokanCA test Root CA.
$sysPackagesConfig = Join-Path $repoRoot 'sys\packages.config'
$packagesDir = Join-Path $repoRoot 'packages'
$packagesXml = [xml](Get-Content -LiteralPath $sysPackagesConfig)
$packagesComplete = $true
foreach ($pkg in @($packagesXml.packages.package)) {
    $pkgDir = Join-Path $packagesDir "$($pkg.id).$($pkg.version)"
    if (-not (Test-Path -LiteralPath $pkgDir -PathType Container) -or
        -not (Get-ChildItem -LiteralPath $pkgDir -ErrorAction SilentlyContinue | Select-Object -First 1)) {
        $packagesComplete = $false
        break
    }
}
if (-not $packagesComplete) {
    Write-Host 'Restoring WDK NuGet packages from sys\packages.config...'
    Invoke-DokanWithRetry -Description 'nuget restore sys\packages.config' -Action {
        Exec-External { & nuget restore $sysPackagesConfig -PackagesDirectory $packagesDir }
    }
}

Push-Location "$repoRoot\dokan_wix"
try {
    # Avoid `Get-ChildItem -Path .\* -Include ... -Recurse -File`: in Windows PowerShell 5.1,
    # `-File` with a wildcard `-Path` filters out subdirectories before recursion.
    $wixProjects = @(
        Get-ChildItem -LiteralPath "$repoRoot\dokan_wix" -Recurse -File |
            Where-Object { $_.Extension -in @('.wixproj', '.vcxproj') }
    )
    if ($wixProjects.Count -eq 0) {
        throw "No .wixproj or .vcxproj files found under $repoRoot\dokan_wix"
    }
    foreach ($proj in $wixProjects) {
        $projPath = $proj.FullName
        Invoke-DokanWithRetry -Description "nuget restore $($proj.Name)" -Action {
            Exec-External { & nuget restore $projPath -SolutionDirectory . }
        }
    }
} finally {
    Pop-Location
}

# 2. Compute the installer version and stamp only the first matching version in lines 1..10 of CHANGELOG.md
#    (matching SetAssemblyVersion/Program.cs:103 so historical releases and WDK versions stay untouched).
$installerVersion = (& "$PSScriptRoot\get_version.ps1" -BuildNumber $BuildNumber | Select-Object -First 1).ToString().Trim()
if ($installerVersion -notmatch '^\d+\.\d+\.\d+\.\d+$') {
    throw "Invalid installer version '$installerVersion' returned by get_version.ps1"
}
Write-Host "Installer version: $installerVersion"

$changelogPath = Join-Path $repoRoot 'CHANGELOG.md'
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)
$changelogLines = [System.IO.File]::ReadAllLines($changelogPath, $utf8NoBom)
$versionRegex = [regex]'\b[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,5}\.[0-9]{1,5}\b'
$stampedChangelog = $false
for ($i = 0; $i -lt [Math]::Min(10, $changelogLines.Length); $i++) {
    if ($versionRegex.IsMatch($changelogLines[$i])) {
        $changelogLines[$i] = $versionRegex.Replace($changelogLines[$i], $installerVersion, 1)
        $stampedChangelog = $true
        break
    }
}
if (-not $stampedChangelog) {
    throw "No 4-part version found in the first 10 lines of $changelogPath"
}
[System.IO.File]::WriteAllLines($changelogPath, $changelogLines, $utf8NoBom)

# 3. Locate signtool.exe (Windows SDK first, WDK NuGet package as fallback) for cert\dokan-sign.ps1.
$signtool = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.*\x64\signtool.exe' -ErrorAction SilentlyContinue |
    Sort-Object FullName -Descending | Select-Object -First 1
if (-not $signtool) {
    $signtool = Get-ChildItem "$repoRoot\packages\Microsoft.Windows.SDK.CPP.x64.*\c\bin\*\x64\signtool.exe" -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending | Select-Object -First 1
}
if (-not $signtool) {
    throw 'signtool.exe not found'
}
Write-Host "Using signtool: $($signtool.FullName)"
$env:Path += ";$($signtool.DirectoryName)"
$env:CERTISSUER = 'DokanCA'
$env:ADDITIONALCERT = "$repoRoot\cert\DokanCA.cer"
$env:SIGNTOOL = $signtool.FullName

# 4. Stamp assembly versions.
Push-Location "$repoRoot\dokan_wix"
try {
    Exec-External { & $msbuild 'SetAssemblyVersion.sln' '/m' '/p:Configuration=Release' '/p:Platform=Any CPU' }
    Exec-External { & .\SetAssemblyVersion\bin\Release\SetAssemblyVersion.exe ..\CHANGELOG.md version.xml ..\ }
} finally {
    Pop-Location
}

# 5. Temporarily import test-signing certs, build Dokan, test-sign binaries, and build the WiX bundle.
#    cert/dokan-import.ps1 is not used because it also runs 'bcdedit /set testsigning on', which is
#    neither needed nor allowed on hosted runners.
$dokanCaThumbprint = '0FE89BCAB1F3D14145698F95F9C9422ECA62217E'
$dokanSignThumbprint = 'C179DAE2A6BD88C54F765853028E46827AC44C95'
$importedCertPaths = @()
try {
    Write-Host 'Importing Dokan test-signing certificates...'
    $rootCaPath = "Cert:\LocalMachine\Root\$dokanCaThumbprint"
    if (-not (Test-Path -LiteralPath $rootCaPath)) {
        $importedCertPaths += $rootCaPath
    }
    Import-Certificate -FilePath .\cert\DokanCA.cer -CertStoreLocation 'Cert:\LocalMachine\Root' | Out-Null

    $trustedPubCaPath = "Cert:\LocalMachine\TrustedPublisher\$dokanCaThumbprint"
    if (-not (Test-Path -LiteralPath $trustedPubCaPath)) {
        $importedCertPaths += $trustedPubCaPath
    }
    Import-Certificate -FilePath .\cert\DokanCA.cer -CertStoreLocation 'Cert:\LocalMachine\TrustedPublisher' | Out-Null

    $signCertPath = "Cert:\CurrentUser\My\$dokanSignThumbprint"
    if (-not (Test-Path -LiteralPath $signCertPath)) {
        $importedCertPaths += $signCertPath
    }
    Import-PfxCertificate -FilePath .\cert\DokanSign.pfx -CertStoreLocation 'Cert:\CurrentUser\My' | Out-Null

    # Build Dokan (Win32, x64, ARM64) and dokan_fuse (Cygwin x64, MSYS2 MINGW32 + UCRT64).
    $buildParts = @('win')
    if ($hasCygwin) { $buildParts += 'cygwin' }
    if ($hasMsys2) { $buildParts += 'msys2' }
    Write-Host "Building Dokan parts: $($buildParts -join ', ')"
    Exec-External { & .\scripts\build.ps1 -BuildPart $buildParts -Configurations $Configuration }

    # Verify dokannp3.dll PE import table (Win32, x64, ARM64) imports ONLY KERNEL32.dll.
    foreach ($arch in @('Win32', 'x64', 'ARM64')) {
        $npDll = Join-Path $repoRoot "$arch\$Configuration\dokannp3.dll"
        if (-not (Test-Path -LiteralPath $npDll -PathType Leaf)) {
            throw "Expected dokannp3.dll not found at $npDll"
        }
        Assert-DokanNpKernel32OnlyImports -BinaryPath $npDll
        Write-Host "PE import check passed ($arch): $npDll imports KERNEL32.dll only."
    }

    # Test-sign all binaries.
    Exec-External { & .\cert\dokan-sign.ps1 }

    # Build the WiX installer bundle.
    Push-Location "$repoRoot\dokan_wix"
    try {
        $versionXmlText = Get-Content -LiteralPath 'version.xml'
        $versionXmlText = $versionXmlText -replace 'BuildCygwin="(true|false)"', "BuildCygwin=`"$($hasCygwin.ToString().ToLowerInvariant())`""
        $versionXmlText = $versionXmlText -replace 'BuildMsys2="(true|false)"', "BuildMsys2=`"$($hasMsys2.ToString().ToLowerInvariant())`""
        Set-Content -LiteralPath 'version.xml' -Value $versionXmlText
        Exec-External { & $msbuild 'Dokan_WiX.sln' '/m' "/p:Configuration=$Configuration" '/p:Platform=Mixed Platforms' }
    } finally {
        Pop-Location
    }
} finally {
    foreach ($certPath in $importedCertPaths) {
        try {
            if (Test-Path -LiteralPath $certPath) {
                Remove-Item -LiteralPath $certPath -Force -ErrorAction SilentlyContinue
            }
        } catch {
            Write-Warning "Failed to remove temporary test certificate '$certPath': $($_.Exception.Message)"
        }
    }
}

# 6. Stage release artifacts (same names as the former AppVeyor artifacts).
$payloadAbs = Resolve-DokanTargetPath -BasePath $repoRoot -Path $PayloadDir
$releaseAbs = Resolve-DokanTargetPath -BasePath $repoRoot -Path $ReleaseDir
foreach ($dir in @($payloadAbs, $releaseAbs)) {
    Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
}

$setupExe = "$repoRoot\dokan_wix\Bootstrapper\bin\x86\$Configuration\DokanSetup.exe"
Copy-Item -LiteralPath $setupExe -Destination (Join-Path $releaseAbs "${ArtifactPrefix}DokanSetup-$installerVersion.exe")
Exec-External { & $sevenZip a -tzip (Join-Path $releaseAbs "${ArtifactPrefix}dokan-$installerVersion.zip") Win32 x64 ARM64 | Out-Null }

# 7. Stage the VM test payload.
Copy-Item -LiteralPath $setupExe -Destination "$payloadAbs\DokanSetup.exe"
Copy-Item -LiteralPath "$repoRoot\x64\$Configuration\Driver\dokan3.sys" -Destination "$payloadAbs\dokan3.sys"
Copy-Item -LiteralPath "$repoRoot\x64\$Configuration\Driver\dokan3.pdb" -Destination "$payloadAbs\dokan3.pdb"

if ($env:GITHUB_OUTPUT) {
    Add-Content -Path $env:GITHUB_OUTPUT -Value "version=$installerVersion"
}

Write-Host "Release artifacts ($releaseAbs):"
Get-ChildItem -LiteralPath $releaseAbs | Format-Table Name, Length
Write-Host "VM test payload ($payloadAbs):"
Get-ChildItem -LiteralPath $payloadAbs -Recurse -File | Format-Table FullName, Length
