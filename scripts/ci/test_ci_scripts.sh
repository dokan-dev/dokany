#!/usr/bin/env bash
# Behavioral unit tests for CI failure/post-merge paths that green CI runs never
# exercise directly:
#   1. cleanup-pr-caches.yml jq run-matcher (fork/deleted-fork/same-repo/self/pagination)
#   2. scripts/run_ifstest.ps1 Find-IfsTestCreateProcFailure (Set-StrictMode -Version 2)
#   3. scripts/ci/install_cygwin.ps1 Assert-CygwinGpgVerification (GOODSIG/EXPKEYSIG/REVKEYSIG)
#   4. scripts/ci/vm/provisioning/verifier_helpers.ps1 Driver Verifier guards
#   5. scripts/ci/vm/run_guest_tests.ps1 Session-0 trampoline & collect_guest_diagnostics.ps1 syntax
# Uses only tools preinstalled on ubuntu-latest (jq, pwsh, python3 stdlib).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT"

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

echo "=== 1. cleanup-pr-caches.yml jq run matcher ==="
python3 - << 'EOF'
import json
import re
import subprocess
import textwrap

with open('.github/workflows/cleanup-pr-caches.yml', encoding='utf-8') as f:
    workflow = f.read()

m = re.search(r"read -r -d '' jq_filter <<'JQ' \|\| true\n(.*?)\n[ \t]*JQ(?:\r?\n|\Z)", workflow, re.S)
assert m, 'Failed to extract jq_filter heredoc from cleanup-pr-caches.yml'
jq_filter = textwrap.dedent(m.group(1))

# Simulate two concatenated pages from gh api --paginate.
pages = json.dumps({
    'workflow_runs': [
        # Active fork PR run at current HEAD SHA
        {'id': 101, 'status': 'in_progress', 'head_sha': 'a' * 40, 'head_branch': 'feat',
         'head_repository': {'full_name': 'Liryna/dokany'}, 'pull_requests': []},
        # Active fork PR run at superseded older SHA on same branch
        {'id': 102, 'status': 'queued', 'head_sha': 'b' * 40, 'head_branch': 'feat',
         'head_repository': {'full_name': 'Liryna/dokany'}, 'pull_requests': []},
        # Active fork PR run on a different branch of the same fork
        {'id': 103, 'status': 'in_progress', 'head_sha': 'c' * 40, 'head_branch': 'other',
         'head_repository': {'full_name': 'Liryna/dokany'}, 'pull_requests': []},
        # Active run on an unrelated live fork with identical SHA and branch
        {'id': 104, 'status': 'in_progress', 'head_sha': 'a' * 40, 'head_branch': 'feat',
         'head_repository': {'full_name': 'victim/dokany'}, 'pull_requests': []},
        # Active run whose fork was deleted (head_repository null) at matching SHA
        {'id': 105, 'status': 'in_progress', 'head_sha': 'a' * 40, 'head_branch': 'feat',
         'head_repository': None, 'pull_requests': []},
        # Completed run at matching SHA (must be ignored)
        {'id': 106, 'status': 'completed', 'head_sha': 'a' * 40, 'head_branch': 'feat',
         'head_repository': {'full_name': 'Liryna/dokany'}, 'pull_requests': []},
        # Self run ID guard (must be ignored)
        {'id': 999, 'status': 'in_progress', 'head_sha': 'a' * 40, 'head_branch': 'feat',
         'head_repository': {'full_name': 'Liryna/dokany'}, 'pull_requests': [{'number': 1368}]},
    ]
}) + '\n' + json.dumps({
    'workflow_runs': [
        # Active same-repo PR run with pull_requests populated (older SHA)
        {'id': 201, 'status': 'waiting', 'head_sha': 'd' * 40, 'head_branch': 'same-repo',
         'head_repository': {'full_name': 'dokan-dev/dokany'}, 'pull_requests': [{'number': 1368}]},
        # Active same-repo PR run belonging to a different PR at same SHA
        {'id': 202, 'status': 'in_progress', 'head_sha': 'a' * 40, 'head_branch': 'feat',
         'head_repository': {'full_name': 'dokan-dev/dokany'}, 'pull_requests': [{'number': 9999}]},
        # Active same-repo PR run whose .pull_requests was cleared after close
        {'id': 203, 'status': 'in_progress', 'head_sha': 'e' * 40, 'head_branch': 'same-repo',
         'head_repository': {'full_name': 'dokan-dev/dokany'}, 'pull_requests': []},
    ]
})

def run_filter(pr, sha, head_repo, head_ref, self_id=999):
    out = subprocess.check_output(
        [
            'jq', '-r',
            '--argjson', 'self_id', str(self_id),
            '--argjson', 'pr', str(pr),
            '--arg', 'sha', sha,
            '--arg', 'head_repo', head_repo,
            '--arg', 'head_ref', head_ref,
            jq_filter,
        ],
        input=pages.encode('utf-8'),
    ).decode('utf-8')
    return out.split()

# Intact fork PR: matches 101 (HEAD SHA), 102 (superseded SHA on same fork+branch),
# and 201 (same PR number). Excludes 103, 104 (victim fork), 105 (deleted), 106, 999, 202.
assert run_filter(1368, 'A' * 40, 'liryna/dokany', 'feat') == ['101', '102', '201']

# Deleted fork PR (head_repo=''): matches deleted-fork run 105 + PR number 201 only;
# NEVER matches live forks 101/102/104 even with identical SHA and branch.
assert run_filter(1368, 'a' * 40, '', 'feat') == ['105', '201']

# Deleted fork PR with a DIFFERENT SHA but same branch must NOT match deleted-fork run 105.
assert run_filter(1368, 'b' * 40, '', 'feat') == ['201']

# Closed same-repo PR whose .pull_requests was cleared: matches 201 (pr) and 203 (branch).
assert run_filter(1368, 'f' * 40, 'dokan-dev/dokany', 'same-repo') == ['201', '203']

print('cleanup-pr-caches.yml jq unit tests passed.')
EOF

echo "=== 2. PowerShell behavioral unit tests (run_ifstest, install_cygwin, Verifier guards) ==="
cat <<'PWSH' > "$WORK_DIR/test_ps.ps1"
Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'

function Import-AstFunction {
    param([string]$FilePath, [string]$FunctionName)
    $resolved = (Resolve-Path $FilePath).Path
    $ast = [System.Management.Automation.Language.Parser]::ParseFile($resolved, [ref]$null, [ref]$null)
    $fn = $ast.Find({
        param($n)
        $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
        $n.Name -eq $FunctionName
    }, $true)
    if ($null -eq $fn) { throw "$FunctionName not found in $FilePath" }
    return $fn.Extent.Text
}

# 2a. Find-IfsTestCreateProcFailure (scripts/run_ifstest.ps1)
Invoke-Expression (Import-AstFunction -FilePath 'scripts/run_ifstest.ps1' -FunctionName 'Find-IfsTestCreateProcFailure')

foreach ($emptyInput in @(@(), $null, 'single line')) {
    $r = Find-IfsTestCreateProcFailure -OutputLines $emptyInput
    if ($r.Guarded.Count -ne 0 -or $r.Other.Count -ne 0) { throw 'Empty/null/scalar case failed' }
}

$rOther = Find-IfsTestCreateProcFailure -OutputLines @(
    '--------------------------------------------------',
    '1Test         :SetDaclSecurityTest',
    'Status       :00000000 (IFSTEST_SUCCESS)',
    'Test         :SetOwnerSecurityTest',
    'Status       :00000000 (IFSTEST_SUCCESS)',
    'Test         :DirectoryTraverseTest',
    'Status       :C000001E (IFSTEST_TEST_NTAPI_FAILURE_CODE)',
    'Test         :LockKeyTest',
    'Status       :C0000095 (IFSTEST_SETUP_PROCESS_CREATEPROC_FAILURE)',
    'Description  :IFSTEST_SETUP_PROCESS_CREATEPROC_FAILURE repeated'
)
if ($rOther.Guarded.Count -ne 0 -or $rOther.Other.Count -ne 1 -or $rOther.Other[0] -notmatch '^LockKeyTest:') {
    throw 'Non-guarded LockKeyTest case failed'
}

$rGuarded = Find-IfsTestCreateProcFailure -OutputLines @(
    '1Test         :SetOwnerSecurityTest',
    'Status       :C0000095 (IFSTEST_SETUP_PROCESS_CREATEPROC_FAILURE)',
    'Description  :IFSTEST_SETUP_PROCESS_CREATEPROC_FAILURE duplicate',
    'Test         :DirectoryTraverseTest',
    'Status       :C0000095 (IFSTEST_SETUP_PROCESS_CREATEPROC_FAILURE)'
)
if ($rGuarded.Guarded.Count -ne 2 -or $rGuarded.Other.Count -ne 0 -or
    $rGuarded.Guarded[0] -notmatch '^SetOwnerSecurityTest:' -or
    $rGuarded.Guarded[1] -notmatch '^DirectoryTraverseTest:') {
    throw 'Guarded deduplication/ordering case failed'
}

$rUnknown = Find-IfsTestCreateProcFailure -OutputLines @(
    'Test         :LockKeyTest',
    '--------------------------------------------------',
    'Status       :C0000095 (IFSTEST_SETUP_PROCESS_CREATEPROC_FAILURE)'
)
if ($rUnknown.Guarded.Count -ne 1 -or $rUnknown.Guarded[0] -notmatch '^<unknown>:') {
    throw 'Unknown fail-closed case failed'
}
Write-Output 'run_ifstest.ps1 Find-IfsTestCreateProcFailure unit tests passed.'

# 2b. Assert-CygwinGpgVerification (scripts/ci/install_cygwin.ps1)
$cygText = Get-Content -LiteralPath (Resolve-Path 'scripts/ci/install_cygwin.ps1').Path -Raw
$fprMatch = [regex]::Match($cygText, "CygwinKeyFingerprint\s*=\s*'([0-9A-F]{40})'")
if (-not $fprMatch.Success) { throw 'CygwinKeyFingerprint constant not found in scripts/ci/install_cygwin.ps1' }
$script:CygwinKeyFingerprint = $fprMatch.Groups[1].Value
$keyId = $script:CygwinKeyFingerprint.Substring($script:CygwinKeyFingerprint.Length - 16)
Invoke-Expression (Import-AstFunction -FilePath 'scripts/ci/install_cygwin.ps1' -FunctionName 'Assert-CygwinGpgVerification')

$goodOut = "[GNUPG:] GOODSIG $keyId Cygwin <cygwin@cygwin.com>`n[GNUPG:] VALIDSIG $($script:CygwinKeyFingerprint) 2026-11-01"
Assert-CygwinGpgVerification -ExitCode 0 -StatusOutput $goodOut

foreach ($badCase in @(
    @{ Name = 'expired'; Rc = 0; Out = "[GNUPG:] EXPKEYSIG $keyId Cygwin`n[GNUPG:] VALIDSIG $($script:CygwinKeyFingerprint) 2026-11-01" },
    @{ Name = 'revoked'; Rc = 0; Out = "[GNUPG:] REVKEYSIG $keyId Cygwin`n[GNUPG:] VALIDSIG $($script:CygwinKeyFingerprint) 2026-11-01" },
    @{ Name = 'wrong-fpr'; Rc = 0; Out = "[GNUPG:] GOODSIG $keyId Cygwin`n[GNUPG:] VALIDSIG 0000000000000000000000000000000000000000 2026-11-01" },
    @{ Name = 'nonzero-rc'; Rc = 1; Out = $goodOut }
)) {
    $threw = $false
    try { Assert-CygwinGpgVerification -ExitCode $badCase.Rc -StatusOutput $badCase.Out } catch { $threw = $true }
    if (-not $threw) { throw "Assert-CygwinGpgVerification unexpectedly accepted $($badCase.Name)" }
}
Write-Output 'install_cygwin.ps1 Assert-CygwinGpgVerification unit tests passed.'

# 2c. Assert-DriverVerifierSettings / Assert-DriverVerifierActive (scripts/ci/vm/provisioning/verifier_helpers.ps1)
. (Resolve-Path 'scripts/ci/vm/provisioning/verifier_helpers.ps1').Path

$goodSettings2016 = "Verifier Flags: 0x020209bb`r`n    [X] 0x00000001 Special pool.`r`n    [X] 0x02000000 Code integrity checks.`r`n  Verified Drivers:`r`n    dokan3.sys`r`n"
$goodSettings2025 = "Verifier Flags: 0x021209bb`n    [X] 0x00000001 Special pool.`n    [X] 0x02000000 Code integrity checks.`n    [X] 0x00100000 Extended Verifier flags (internal).`n  Active Rule Classes:`n    26 - Code integrity checks.`n  Verified Drivers:`n    dokan3.sys`n"
Assert-DriverVerifierSettings -QuerySettingsOutput $goodSettings2016
Assert-DriverVerifierSettings -QuerySettingsOutput $goodSettings2025

foreach ($badCase in @(
    @{ Name = 'empty'; Out = '' },
    @{ Name = 'missing-ci-bit'; Out = "Verifier Flags: 0x001209bb`n    [ ] 0x02000000 Code integrity checks.`n    dokan3.sys`n" },
    @{ Name = 'missing-rc26'; Out = "Verifier Flags: 0x021209bb`n    [X] 0x02000000 Code integrity checks.`n  Active Rule Classes:`n    21 - Extended Verifier flags (internal).`n    dokan3.sys`n" },
    @{ Name = 'dropped-standard'; Out = "Verifier Flags: 0x02000000`n    [X] 0x02000000 Code integrity checks.`n    dokan3.sys`n" },
    @{ Name = 'missing-driver'; Out = "Verifier Flags: 0x021209bb`n    [X] 0x02000000 Code integrity checks.`n    other.sys`n" }
)) {
    $threw = $false
    try { Assert-DriverVerifierSettings -QuerySettingsOutput $badCase.Out } catch { $threw = $true }
    if (-not $threw) { throw "Assert-DriverVerifierSettings unexpectedly accepted $($badCase.Name)" }
}
Write-Output 'verifier_helpers.ps1 Assert-DriverVerifierSettings unit tests passed.'

$goodQuery2016 = "Verifier Flags: 0x020209bb`r`n    [X] 0x02000000 Code integrity checks.`r`n    MODULE: dokan3.sys (load: 1 / unload: 0)`r`n"
$goodQuery2025 = "Verifier Flags: 0x021209bb`n    [X] 0x02000000 Code integrity checks.`n    [X] 0x00100000 Extended Verifier flags (internal).`n    MODULE: dokan3.sys (load: 1 / unload: 0)`n"
Assert-DriverVerifierActive -QueryOutput $goodQuery2016
Assert-DriverVerifierActive -QueryOutput $goodQuery2025

foreach ($badCase in @(
    @{ Name = 'empty'; Out = '' },
    @{ Name = 'missing-ci-bit'; Out = "Verifier Flags: 0x001209bb`n    [ ] 0x02000000 Code integrity checks.`n    MODULE: dokan3.sys (load: 1 / unload: 0)`n" },
    @{ Name = 'dropped-standard'; Out = "Verifier Flags: 0x02000000`n    [X] 0x02000000 Code integrity checks.`n    MODULE: dokan3.sys (load: 1 / unload: 0)`n" },
    @{ Name = 'missing-module'; Out = "Verifier Flags: 0x021209bb`n    [X] 0x02000000 Code integrity checks.`n    MODULE: other.sys (load: 1 / unload: 0)`n" }
)) {
    $threw = $false
    try { Assert-DriverVerifierActive -QueryOutput $badCase.Out } catch { $threw = $true }
    if (-not $threw) { throw "Assert-DriverVerifierActive unexpectedly accepted $($badCase.Name)" }
}
Write-Output 'verifier_helpers.ps1 Assert-DriverVerifierActive unit tests passed.'

# 2d. Session-0 -> Session-1 trampoline (scripts/ci/vm/run_guest_tests.ps1) & collect_guest_diagnostics.ps1 syntax
foreach ($psFile in @('scripts/ci/vm/run_guest_tests.ps1', 'scripts/ci/vm/collect_guest_diagnostics.ps1')) {
    $parseErrors = $null
    [void][System.Management.Automation.Language.Parser]::ParseFile((Resolve-Path $psFile).Path, [ref]$null, [ref]$parseErrors)
    if ($parseErrors.Count -gt 0) {
        throw "Parse error(s) in ${psFile}: $(($parseErrors | ForEach-Object { $_.Message }) -join '; ')"
    }
}

foreach ($fn in @('Read-GuestLogChunk', 'Open-GuestSession1Process', 'Wait-GuestSession1Task')) {
    Invoke-Expression (Import-AstFunction -FilePath 'scripts/ci/vm/run_guest_tests.ps1' -FunctionName $fn)
}

$trampDir = Join-Path -Path $PSScriptRoot -ChildPath 'trampoline_tests'
function New-TrampolineCase {
    param([string]$Name)
    $dir = Join-Path -Path $trampDir -ChildPath $Name
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    return @{
        Log  = Join-Path $dir 'guest.log'
        Pid  = Join-Path $dir 'guest.pid'
        Exit = Join-Path $dir 'guest.exit'
    }
}

function New-MockProcess {
    param(
        [string]$ProcessName = 'powershell',
        [int]$ExitAfterMs = 40,
        [int]$ExitCode = 0,
        [scriptblock]$OnExit = $null
    )
    $state = @{
        Disposed    = $false
        Exited      = ($ExitAfterMs -eq 0)
        ExitAfterMs = $ExitAfterMs
        ExitAt      = $null
        OnExit      = $OnExit
    }
    $obj = New-Object PSObject -Property @{
        ProcessName = $ProcessName
        ExitCode    = $ExitCode
        _State      = $state
    }
    $obj | Add-Member -MemberType ScriptProperty -Name 'HasExited' -Value {
        if (-not $this._State.Exited -and $this._State.ExitAfterMs -gt 0) {
            if ($null -eq $this._State.ExitAt) {
                $this._State.ExitAt = (Get-Date).AddMilliseconds($this._State.ExitAfterMs)
            } elseif ((Get-Date) -ge $this._State.ExitAt) {
                $this._State.Exited = $true
                if ($null -ne $this._State.OnExit) { & $this._State.OnExit }
            }
        }
        return $this._State.Exited
    }
    $obj | Add-Member -MemberType ScriptMethod -Name 'Dispose' -Value {
        $this._State.Disposed = $true
    }
    return $obj
}

# Read-GuestLogChunk: missing file, incremental read, no-op re-read, trailing bytes without newline
$c = New-TrampolineCase 'chunks'
$buf = New-Object System.Text.StringBuilder
$off = [int64]0
$writer = { param([string]$t) [void]$buf.Append($t) }
Read-GuestLogChunk -Path $c.Log -ByteOffset ([ref]$off) -Writer $writer
[System.IO.File]::WriteAllText($c.Log, "line1`r`n", [System.Text.Encoding]::ASCII)
Read-GuestLogChunk -Path $c.Log -ByteOffset ([ref]$off) -Writer $writer
Read-GuestLogChunk -Path $c.Log -ByteOffset ([ref]$off) -Writer $writer
[System.IO.File]::AppendAllText($c.Log, 'trailing', [System.Text.Encoding]::ASCII)
Read-GuestLogChunk -Path $c.Log -ByteOffset ([ref]$off) -Writer $writer
if ($buf.ToString() -ne "line1`r`ntrailing") { throw "Read-GuestLogChunk mismatch: '$($buf.ToString())'" }

# Happy path: PID attach stops all WMI/CIM Get-ScheduledTask polling and drains trailing output
$c = New-TrampolineCase 'happy'
$out = New-Object System.Text.StringBuilder
$polls = 0
$pidReady = $false
$proc = New-MockProcess -ExitAfterMs 50 -ExitCode 0 -OnExit {
    [System.IO.File]::AppendAllText($c.Log, "tail`n", [System.Text.Encoding]::ASCII)
    [System.IO.File]::WriteAllText($c.Exit, '0', [System.Text.Encoding]::ASCII)
}
$r = Wait-GuestSession1Task `
    -TaskName 'DokanCITests' -LogPath $c.Log -PidPath $c.Pid -ExitPath $c.Exit `
    -StartTimeoutSeconds 3 -ExitGraceSeconds 2 -PollIntervalMs 10 -TaskPollIntervalMs 10 -FinalDrainMs 10 `
    -WarningVariable warns -WarningAction SilentlyContinue `
    -GetTaskState {
        param($Name)
        if ($pidReady) { throw 'Get-ScheduledTask must not be called after PID attach!' }
        $script:polls++
        [System.IO.File]::WriteAllText($c.Log, "start`n", [System.Text.Encoding]::ASCII)
        [System.IO.File]::WriteAllText($c.Pid, '4242', [System.Text.Encoding]::ASCII)
        $script:pidReady = $true
        return 'Queued'
    } `
    -GetProcessById { param($Id) if ($Id -ne 4242) { throw "Unexpected PID $Id" }; return $proc } `
    -LogWriter { param([string]$t) [void]$out.Append($t) }
if ($r.ExitCode -ne 0 -or -not $r.ChildExited -or $r.ExitReason -ne 'ExitFile' -or
    $polls -ne 1 -or -not $proc._State.Disposed -or $out.ToString() -ne "start`ntail`n" -or $warns.Count -ne 0) {
    throw "Happy path failed: $($r | ConvertTo-Json -Compress) polls=$polls out='$($out.ToString())'"
}

# Transient Get-ScheduledTask 0x80070002 during startup recovers once PID file appears
$c = New-TrampolineCase 'transient_cim'
$cimCalls = 0
$proc = New-MockProcess -ExitAfterMs 40 -ExitCode 0 -OnExit {
    [System.IO.File]::WriteAllText($c.Exit, '0', [System.Text.Encoding]::ASCII)
}
$r = Wait-GuestSession1Task `
    -TaskName 'DokanCITests' -LogPath $c.Log -PidPath $c.Pid -ExitPath $c.Exit `
    -StartTimeoutSeconds 3 -ExitGraceSeconds 2 -PollIntervalMs 10 -TaskPollIntervalMs 10 -FinalDrainMs 10 `
    -WarningVariable warns -WarningAction SilentlyContinue `
    -GetTaskState {
        param($Name)
        $script:cimCalls++
        if ($script:cimCalls -ge 2) { [System.IO.File]::WriteAllText($c.Pid, '5150', [System.Text.Encoding]::ASCII) }
        throw 'The system cannot find the file specified. (HRESULT 0x80070002)'
    } `
    -GetProcessById { param($Id) return $proc } `
    -LogWriter { param([string]$t) }
if ($r.ExitCode -ne 0 -or -not $r.ChildExited -or $r.TaskPollErrors -lt 2 -or $warns.Count -lt 1 -or -not $proc._State.Disposed) {
    throw "Transient CIM recovery case failed: $($r | ConvertTo-Json -Compress)"
}

# Fast exit race: exercises real default Process.GetProcessById on a non-existent PID
$c = New-TrampolineCase 'fast_exit'
$out = New-Object System.Text.StringBuilder
[System.IO.File]::WriteAllText($c.Log, "fast-done`n", [System.Text.Encoding]::ASCII)
[System.IO.File]::WriteAllText($c.Pid, '2147483647', [System.Text.Encoding]::ASCII)
[System.IO.File]::WriteAllText($c.Exit, '42', [System.Text.Encoding]::ASCII)
$r = Wait-GuestSession1Task `
    -TaskName 'DokanCITests' -LogPath $c.Log -PidPath $c.Pid -ExitPath $c.Exit `
    -StartTimeoutSeconds 3 -PollIntervalMs 10 -TaskPollIntervalMs 10 -FinalDrainMs 10 `
    -GetTaskState { param($Name) throw 'Should not query task when PID file already exists' } `
    -LogWriter { param([string]$t) [void]$out.Append($t) }
if ($r.ExitCode -ne 42 -or -not $r.ChildExited -or $r.ExitReason -ne 'ExitFile' -or $out.ToString() -ne "fast-done`n") {
    throw "Fast exit race case failed: $($r | ConvertTo-Json -Compress)"
}

# Abnormal child crash without ExitFile (0xC0000005) exits 1 and warns with hex exit code
$c = New-TrampolineCase 'crash_no_exitfile'
$out = New-Object System.Text.StringBuilder
[System.IO.File]::WriteAllText($c.Pid, '7001', [System.Text.Encoding]::ASCII)
$proc = New-MockProcess -ExitAfterMs 30 -ExitCode ([int]-1073741819) -OnExit {
    [System.IO.File]::WriteAllText($c.Log, "before-crash`n", [System.Text.Encoding]::ASCII)
}
$r = Wait-GuestSession1Task `
    -TaskName 'DokanCITests' -LogPath $c.Log -PidPath $c.Pid -ExitPath $c.Exit `
    -StartTimeoutSeconds 3 -PollIntervalMs 10 -TaskPollIntervalMs 10 -FinalDrainMs 10 `
    -WarningVariable warns -WarningAction SilentlyContinue `
    -GetTaskState { param($Name) throw 'Should not query task after PID attach' } `
    -GetProcessById { param($Id) return $proc } `
    -LogWriter { param([string]$t) [void]$out.Append($t) }
if ($r.ExitCode -ne 1 -or -not $r.ChildExited -or $r.SawExitFile -or $r.ExitReason -ne 'ProcessExitedWithoutExitFile' -or
    -not $proc._State.Disposed -or $out.ToString() -ne "before-crash`n" -or ($warns -join '; ') -notmatch '0xC0000005') {
    throw "Abnormal child crash case failed: $($r | ConvertTo-Json -Compress) warns=$($warns -join '; ')"
}

# Recycled PID mismatch before attach returns promptly without hanging
$c = New-TrampolineCase 'pid_reused'
[System.IO.File]::WriteAllText($c.Pid, '8001', [System.Text.Encoding]::ASCII)
$proc = New-MockProcess -ProcessName 'svchost' -ExitAfterMs -1
$r = Wait-GuestSession1Task `
    -TaskName 'DokanCITests' -LogPath $c.Log -PidPath $c.Pid -ExitPath $c.Exit `
    -StartTimeoutSeconds 3 -PollIntervalMs 10 -TaskPollIntervalMs 10 -FinalDrainMs 10 `
    -WarningVariable warns -WarningAction SilentlyContinue `
    -GetTaskState { param($Name) throw 'Should not query task when PID file already exists' } `
    -GetProcessById { param($Id) return $proc } `
    -LogWriter { param([string]$t) }
if ($r.ExitCode -ne 1 -or -not $r.ChildExited -or $r.ExitReason -ne 'PidReused' -or
    -not $proc._State.Disposed -or ($warns -join '; ') -notmatch 'svchost') {
    throw "PID reuse guard case failed: $($r | ConvertTo-Json -Compress)"
}

# Task exits during startup before writing PID file -> fails fast and drains error output
$c = New-TrampolineCase 'task_exit_before_pid'
$out = New-Object System.Text.StringBuilder
$states = @('Running', 'Ready')
$idx = 0
$r = Wait-GuestSession1Task `
    -TaskName 'DokanCITests' -LogPath $c.Log -PidPath $c.Pid -ExitPath $c.Exit `
    -StartTimeoutSeconds 10 -PollIntervalMs 10 -TaskPollIntervalMs 10 -FinalDrainMs 10 `
    -GetTaskState {
        param($Name)
        $s = $states[[math]::Min($script:idx, $states.Count - 1)]
        if ($script:idx -eq 1) { [System.IO.File]::WriteAllText($c.Log, "parse error`n", [System.Text.Encoding]::ASCII) }
        $script:idx++
        return $s
    } `
    -LogWriter { param([string]$t) [void]$out.Append($t) }
if ($r.ExitCode -ne 1 -or -not $r.ChildExited -or $r.ExitReason -ne 'TaskExitedBeforePid' -or $out.ToString() -ne "parse error`n") {
    throw "TaskExitedBeforePid case failed: $($r | ConvertTo-Json -Compress) out='$($out.ToString())'"
}

# Startup timeout MUST fire even after seeing Running once if PID never arrives and CIM errors begin
$c = New-TrampolineCase 'start_timeout_after_running'
$threw = $false
$errMsg = ''
$pollIdx = 0
try {
    [void](Wait-GuestSession1Task `
        -TaskName 'DokanCITests' -LogPath $c.Log -PidPath $c.Pid -ExitPath $c.Exit `
        -StartTimeoutSeconds 1 -PollIntervalMs 20 -TaskPollIntervalMs 40 -FinalDrainMs 0 `
        -WarningAction SilentlyContinue `
        -GetTaskState {
            param($Name)
            $script:pollIdx++
            if ($script:pollIdx -eq 1) { return 'Running' }
            throw 'CIM boom 0x80070002'
        } `
        -GetTaskInfo { param($Name) return $null } `
        -LogWriter { param([string]$t) })
} catch {
    $threw = $true
    $errMsg = $_.Exception.Message
}
if (-not $threw -or $errMsg -notmatch 'never started Session 1' -or
    $errMsg -notmatch 'LastTaskResult=<unavailable>' -or $errMsg -notmatch 'CIM boom 0x80070002') {
    throw "Startup timeout case failed: threw=$threw msg='$errMsg'"
}

# ExitFile written while child hangs on teardown -> grace timeout leaves ChildExited=$false so caller stops task
$c = New-TrampolineCase 'exit_grace_timeout'
[System.IO.File]::WriteAllText($c.Pid, '9001', [System.Text.Encoding]::ASCII)
[System.IO.File]::WriteAllText($c.Exit, '0', [System.Text.Encoding]::ASCII)
$proc = New-MockProcess -ExitAfterMs -1
$r = Wait-GuestSession1Task `
    -TaskName 'DokanCITests' -LogPath $c.Log -PidPath $c.Pid -ExitPath $c.Exit `
    -StartTimeoutSeconds 3 -ExitGraceSeconds 1 -PollIntervalMs 20 -TaskPollIntervalMs 20 -FinalDrainMs 0 `
    -WarningVariable warns -WarningAction SilentlyContinue `
    -GetTaskState { param($Name) throw 'Should not query task after PID attach' } `
    -GetProcessById { param($Id) return $proc } `
    -LogWriter { param([string]$t) }
if ($r.ExitCode -ne 0 -or $r.ChildExited -or -not $r.SawExitFile -or $r.ExitReason -ne 'ExitFileGraceTimeout' -or
    -not $proc._State.Disposed -or $warns.Count -lt 1) {
    throw "ExitFileGraceTimeout case failed: $($r | ConvertTo-Json -Compress)"
}

Write-Output 'run_guest_tests.ps1 trampoline & collect_guest_diagnostics.ps1 unit tests passed.'
PWSH
pwsh -NoProfile -File "$WORK_DIR/test_ps.ps1"

echo "=== All CI script unit tests passed ==="
