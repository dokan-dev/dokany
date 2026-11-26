#!/usr/bin/env bash
# Behavioral unit tests for CI failure/post-merge paths that green CI runs never
# exercise directly:
#   1. cleanup-pr-caches.yml jq run-matcher (fork/deleted-fork/same-repo/self/pagination)
#   2. scripts/run_ifstest.ps1 Find-IfsTestCreateProcFailure (Set-StrictMode -Version 2)
#   3. scripts/ci/install_cygwin.ps1 Assert-CygwinGpgVerification (GOODSIG/EXPKEYSIG/REVKEYSIG)
#   4. scripts/ci/vm/provisioning/verifier_helpers.ps1 Driver Verifier guards
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
PWSH
pwsh -NoProfile -File "$WORK_DIR/test_ps.ps1"

echo "=== All CI script unit tests passed ==="
