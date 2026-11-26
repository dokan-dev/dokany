<#
.SYNOPSIS
    Downloads, GPG-verifies, and installs the Cygwin build toolchain for Dokan CI.

.DESCRIPTION
    Caches setup-x86_64.exe, its detached GPG signature, and downloaded Cygwin packages
    under $PackageDir. Always attempts a fresh download of Cygwin's pubring.asc outside
    the weekly cache so signing-key expiry extensions and revocations take effect
    immediately, falling back with a warning to the weekly cached keyring only if
    cygwin.com is transiently unreachable.
#>
param(
    [Parameter(Mandatory = $true)]
    [string]$PackageDir,
    [string]$InstallRoot = 'C:\cygwin64',
    # True when $PackageDir was restored from the weekly Actions cache.
    [switch]$CacheHit
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

# Pinned Cygwin signing key fingerprint (current key expires 2027-02-27).
# Require GOODSIG (rejects expired/revoked keys; fails closed if Cygwin briefly
# lapses past expiry) alongside VALIDSIG (pins the full 40-hex fingerprint).
$script:CygwinKeyFingerprint = '56405CF6FCC81574682A5D561A698DE9E2E56300'

function Assert-CygwinGpgVerification {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [int]$ExitCode,
        [AllowEmptyString()]
        [string]$StatusOutput = '',
        [string]$ExpectedFingerprint = $script:CygwinKeyFingerprint
    )

    $expectedKeyId = $ExpectedFingerprint.Substring($ExpectedFingerprint.Length - 16)
    if ($ExitCode -ne 0 `
        -or $StatusOutput -notmatch "(?m)^\[GNUPG:\] GOODSIG ${expectedKeyId}\b" `
        -or $StatusOutput -notmatch "(?m)^\[GNUPG:\] VALIDSIG ${ExpectedFingerprint}\b") {
        throw "GPG verification of Cygwin setup-x86_64.exe failed (exit code $ExitCode)."
    }
}

function Invoke-DownloadWithRetry {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$Uri,
        [Parameter(Mandatory = $true)]
        [string]$OutFile,
        [int]$MaxAttempts = 3,
        [int]$DelaySeconds = 10
    )

    for ($attempt = 1; $attempt -le $MaxAttempts; $attempt++) {
        try {
            Invoke-WebRequest -UseBasicParsing -Uri $Uri -OutFile $OutFile
            return
        } catch {
            Remove-Item -LiteralPath $OutFile -Force -ErrorAction SilentlyContinue
            if ($attempt -ge $MaxAttempts) {
                throw
            }
            Write-Warning "Download of $Uri failed (attempt $attempt/$MaxAttempts): $($_.Exception.Message); retrying in ${DelaySeconds}s..."
            Start-Sleep -Seconds $DelaySeconds
        }
    }
}

$resolvedPackageDir = $PackageDir.TrimEnd('\')
New-Item -ItemType Directory -Force -Path $resolvedPackageDir | Out-Null
$setup = Join-Path -Path $resolvedPackageDir -ChildPath 'setup-x86_64.exe'
$setupSig = Join-Path -Path $resolvedPackageDir -ChildPath 'setup-x86_64.exe.sig'
$cachedPubring = Join-Path -Path $resolvedPackageDir -ChildPath 'pubring.asc'

if (-not (Test-Path -LiteralPath $setup -PathType Leaf) -or -not (Test-Path -LiteralPath $setupSig -PathType Leaf)) {
    Invoke-DownloadWithRetry -Uri 'https://cygwin.com/setup-x86_64.exe' -OutFile $setup
    Invoke-DownloadWithRetry -Uri 'https://cygwin.com/setup-x86_64.exe.sig' -OutFile $setupSig
}

$gpgCmd = Get-Command gpg.exe -ErrorAction SilentlyContinue | Select-Object -First 1
$gpg = if ($null -ne $gpgCmd) { $gpgCmd.Source } else { $null }
if (-not $gpg) {
    $gpg = @('C:\Program Files\Git\usr\bin\gpg.exe', 'C:\msys64\usr\bin\gpg.exe') |
        Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
        Select-Object -First 1
}
if (-not $gpg) {
    throw 'gpg.exe not found to verify Cygwin setup-x86_64.exe'
}

$cygpath = Join-Path -Path (Split-Path -Parent $gpg) -ChildPath 'cygpath.exe'
$useCygpath = Test-Path -LiteralPath $cygpath -PathType Leaf
function Convert-GpgPath {
    [CmdletBinding()]
    [OutputType([string])]
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if (-not $useCygpath) {
        return $Path
    }
    $converted = ((& $cygpath -u $Path) | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrEmpty($converted)) {
        throw "cygpath failed for $Path (exit code $LASTEXITCODE)"
    }
    return $converted
}

$tempRoot = if (-not [string]::IsNullOrWhiteSpace($env:RUNNER_TEMP)) {
    $env:RUNNER_TEMP
} else {
    [System.IO.Path]::GetTempPath()
}
$gpgHome = Join-Path -Path $tempRoot -ChildPath "cygwin-gpg-$PID"
$freshPubring = Join-Path -Path $tempRoot -ChildPath "cygwin-pubring-$PID.asc"
Remove-Item -LiteralPath $gpgHome -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $freshPubring -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $gpgHome | Out-Null

try {
    $pubring = $freshPubring
    $fetchedFreshPubring = $false
    try {
        # Fetch pubring.asc (~3 KB) fresh outside the weekly cache so key expiry
        # extensions and revocations take effect immediately.
        Invoke-DownloadWithRetry -Uri 'https://cygwin.com/key/pubring.asc' -OutFile $freshPubring
        $fetchedFreshPubring = $true
    } catch {
        if (Test-Path -LiteralPath $cachedPubring -PathType Leaf) {
            Write-Warning "Failed to refresh pubring.asc from cygwin.com ($($_.Exception.Message)); falling back to cached keyring at $cachedPubring."
            $pubring = $cachedPubring
        } else {
            throw
        }
    }

    $gpgHomeArg = Convert-GpgPath -Path $gpgHome
    $pubringArg = Convert-GpgPath -Path $pubring
    $setupSigArg = Convert-GpgPath -Path $setupSig
    $setupArg = Convert-GpgPath -Path $setup

    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $importOut = @(& $gpg --homedir $gpgHomeArg --batch --import $pubringArg 2>&1 | ForEach-Object { "$_" })
        $importRc = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $prevEap
    }
    if ($importRc -ne 0) {
        Write-Output ($importOut -join "`n")
        throw "gpg --import failed with exit code $importRc"
    }

    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $verifyOut = @(& $gpg --homedir $gpgHomeArg --batch --status-fd=1 --verify $setupSigArg $setupArg 2>&1 | ForEach-Object { "$_" })
        $verifyRc = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $prevEap
    }
    $verifyText = $verifyOut -join "`n"
    try {
        Assert-CygwinGpgVerification -ExitCode $verifyRc -StatusOutput $verifyText
    } catch {
        if (-not [string]::IsNullOrWhiteSpace($verifyText)) {
            Write-Output $verifyText
        }
        throw
    }

    # Persist the verified fresh keyring into the weekly cache on cache misses so
    # warm/PR runs stay resilient to transient cygwin.com outages without mutating caches.
    if (-not $CacheHit -and $fetchedFreshPubring) {
        Copy-Item -LiteralPath $freshPubring -Destination $cachedPubring -Force
    }
} finally {
    Remove-Item -LiteralPath $gpgHome -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $freshPubring -Force -ErrorAction SilentlyContinue
}

$resolvedInstallRoot = $InstallRoot.TrimEnd('\')
$setupArgs = @(
    '-gqnNdO', '-R', "`"$resolvedInstallRoot`"", '-l', "`"$resolvedPackageDir`"",
    '-P', 'cmake', '-P', 'make', '-P', 'gcc-core', '-P', 'gcc-g++', '-P', 'pkg-config'
)
# Packages downloaded this week are installed offline; otherwise (or if that fails) fall back to HTTPS mirrors.
if ($CacheHit) {
    $p = Start-Process -FilePath $setup -ArgumentList ($setupArgs + '-L') -Wait -PassThru
    if ($p.ExitCode -eq 0) {
        exit 0
    }
    Write-Warning "Offline Cygwin install failed with exit code $($p.ExitCode); falling back to HTTPS mirrors."
}
$mirrors = @(
    'https://mirrors.kernel.org/sourceware/cygwin/',
    'https://mirrors.rit.edu/cygwin/'
)
foreach ($mirror in $mirrors) {
    $p = Start-Process -FilePath $setup -ArgumentList ($setupArgs + @('-s', $mirror)) -Wait -PassThru
    if ($p.ExitCode -eq 0) {
        exit 0
    }
    Write-Warning "Cygwin setup failed with mirror $mirror (exit code $($p.ExitCode))."
}
throw "Cygwin setup failed across all mirrors (last exit code $($p.ExitCode))"
