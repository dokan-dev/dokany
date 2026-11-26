# Shared Driver Verifier assertions used by VM provisioning (provision_guest.ps1)
# and runtime test execution (run_guest_tests.ps1).

function Assert-DriverVerifierFlags {
    param(
        [AllowEmptyString()][string]$Output,
        [Parameter(Mandatory = $true)][string]$Context
    )
    $flagsMatch = [regex]::Match($Output, 'Verifier Flags:\s*0x([0-9a-fA-F]+)', 'IgnoreCase')
    if (-not $flagsMatch.Success) {
        throw "$Context output did not contain Verifier Flags."
    }
    $flags = [Convert]::ToUInt32($flagsMatch.Groups[1].Value, 16)
    # Require /standard (0x000209bb across Server 2016-2025) plus 0x02000000 (Code integrity checks).
    $requiredMask = [uint32]0x020209bb
    if (($flags -band $requiredMask) -ne $requiredMask) {
        throw ("$Context flags 0x{0:x8} missing required bits 0x{1:x8}." -f $flags, $requiredMask)
    }
    if ($Output -notmatch '(?im)^\s*\[X\]\s+0x02000000\b.*Code integrity') {
        throw "$Context did not show [X] 0x02000000 Code integrity checks enabled."
    }
}

function Assert-DriverVerifierSettings {
    param([AllowEmptyString()][string]$QuerySettingsOutput)
    Assert-DriverVerifierFlags -Output $QuerySettingsOutput -Context 'verifier /querysettings'
    if ($QuerySettingsOutput -match '(?im)^\s*Active Rule Classes:' -and
        $QuerySettingsOutput -notmatch '(?im)^\s*26\s*-\s*Code integrity') {
        throw 'verifier /querysettings Active Rule Classes missing 26 - Code integrity checks.'
    }
    if ($QuerySettingsOutput -notmatch '(?im)^\s*dokan3\.sys\s*$') {
        throw 'verifier /querysettings did not list dokan3.sys under Verified Drivers.'
    }
}

function Assert-DriverVerifierActive {
    param([AllowEmptyString()][string]$QueryOutput)
    Assert-DriverVerifierFlags -Output $QueryOutput -Context 'Driver Verifier /query'
    if ($QueryOutput -notmatch '(?im)^\s*MODULE:\s+dokan3\.sys\b') {
        throw 'Driver Verifier is not monitoring dokan3.sys.'
    }
}
