Set-StrictMode -Version 2
#TODO: Get Path via some MSI-CmdLet or Registry instead of hardcoding it
$os_version = [Version](Get-CimInstance Win32_OperatingSystem).Version
if ($os_version -lt [Version]'10.0.10240') {
    throw "Unsupported Windows version: $os_version. Windows 10 (build 10240) or newer is required."
}
$hlk_path = "C:\Program Files (x86)\Windows Kits\10\Hardware Lab Kit"

$nttest_path = "${hlk_path}\Tests\${env:PROCESSOR_ARCHITECTURE}\NTTEST"
$ifstest_exe = "${nttest_path}\BASETEST\core_file_services\ifs_test_kit\ifstest.exe"

if (!(Test-Path $ifstest_exe)) {
    throw "$ifstest_exe not found!"
}

#IFSTest does not return error for wrong param so we need to analyse stderr
& $ifstest_exe $args 2>&1 | Tee-Object -Variable allOutput | ForEach-Object {
    if ($_ -is [System.Management.Automation.ErrorRecord]) {
        Write-Host $_.Exception.Message -ForegroundColor Red
    }
    else {
        $_
    }
}
$stderr = $allOutput | ?{ $_ -is [System.Management.Automation.ErrorRecord] }
if ($stderr) {
   Write-Error "IFSTest printed error"
   Exit 1
}

if ($LASTEXITCODE -ne 0) {
   Write-Error "Non-zero exit-code: $LASTEXITCODE"
   Exit $LASTEXITCODE
}

function Find-IfsTestCreateProcFailure {
    [CmdletBinding()]
    [OutputType([hashtable])]
    param(
        [AllowNull()]
        [AllowEmptyCollection()]
        $OutputLines
    )

    $guardedTests = @('SetOwnerSecurityTest', 'DirectoryTraverseTest')
    $guarded = [ordered]@{}
    $other = [ordered]@{}
    $currentTest = $null

    foreach ($item in $OutputLines) {
        if ($item -isnot [string]) {
            continue
        }
        if ($item -match '^\s*\d*Test\s*:\s*(\S+)') {
            $currentTest = $Matches[1]
        } elseif ($item -match '^\s*(-{10,}|\+{3})') {
            $currentTest = $null
        } elseif ($item -match 'IFSTEST_SETUP_PROCESS_CREATEPROC_FAILURE') {
            $testName = if ($null -ne $currentTest) { $currentTest } else { '<unknown>' }
            $entry = "${testName}: $($item.Trim())"
            $bucket = if ($null -eq $currentTest -or ($guardedTests -contains $testName)) { $guarded } else { $other }
            if (-not $bucket.Contains($testName)) {
                $bucket[$testName] = $entry
            }
        }
    }

    return @{
        Guarded = @($guarded.Values)
        Other   = @($other.Values)
    }
}

# Regression guard: ifstest.exe exits 0 even when multi-process security tests
# (CreateProcessWithLogonW) fail setup with C0000095 (e.g., Session 0 or broken
# logon/account setup). Scoped to SetOwnerSecurityTest and DirectoryTraverseTest
# (plus <unknown> to fail closed on output-format drift) so transient LockKeyTest
# C0000095 flakes warn without failing the run.
$createProcFailures = Find-IfsTestCreateProcFailure -OutputLines $allOutput
if ($createProcFailures.Other.Count -gt 0) {
    Write-Warning "Non-guarded IFSTest C0000095 occurrence(s) (not failing run): $($createProcFailures.Other -join '; ')"
}
if ($createProcFailures.Guarded.Count -gt 0) {
    Write-Error "IFSTest CreateProcessWithLogonW regression detected ($($createProcFailures.Guarded -join '; '))."
    Exit 1
}

