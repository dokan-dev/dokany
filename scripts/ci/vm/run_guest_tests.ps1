<#
.SYNOPSIS
    Runs inside the CI test VM: installs Dokan, checks Driver Verifier, and runs one test shard.

.DESCRIPTION
    Expects the C:\ci layout staged by run_vm_tests.sh:
        C:\ci\DokanSetup.exe
        C:\ci\verifier_helpers.ps1
        C:\ci\fstest\dokan_fstest.exe, C:\ci\fstest\baselines\
        C:\ci\np\x64\dokan_np_test.exe, C:\ci\np\x86\dokan_np_test.exe
        C:\ci\repo\samples\memfs_test.ps1, mirror_test.ps1, fstools\src\fsx\fsx.exe
        C:\ci\repo\scripts\run_ifstest.ps1, fstest_common.ps1, build_helper.ps1
    The samples\ harnesses are the same scripts developers run locally, so CI and local runs stay identical.

    When invoked over Win32-OpenSSH (Session 0), relaunches itself inside interactive Session 1
    (WinSta0\Default) via a one-shot Scheduled Task so IFSTest multi-process security tests
    (CreateProcessWithLogonW) succeed and do not suffer Session 0 desktop/namespace failures.

    Shards (measured 10-16 min each under Driver Verifier, shard1 being the longer one):
        shard1: real NTFS --strict gate, memfs x64 + x86, mirror x64 (drive, dirJunction, driveRemovable, netUnc)
        shard2: mirror x64 (driveMntMgr, driveRemovableMntMgr), mirror x86 (drive, netUnc), dokan_np x64 + x86 (mpr.dll, Shell, multi-session, hygiene)
        all:    everything above
#>
param(
    [ValidateSet('all', 'shard1', 'shard2')]
    [string]$Shard = 'all',
    # Crash-capture self-tests: crash or hang the guest after Dokan is loaded under Driver Verifier.
    [switch]$SimulateBugCheck,
    [switch]$SimulateHang,
    # Internal parameters used when relaunched inside interactive Session 1.
    [switch]$InSession1,
    [string]$PidFile = '',
    [string]$ExitFile = ''
)

$ErrorActionPreference = 'Stop'

function Write-AtomicAsciiFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Value
    )
    $tmp = "$Path.tmp"
    [System.IO.File]::WriteAllText($tmp, $Value, [System.Text.Encoding]::ASCII)
    Move-Item -LiteralPath $tmp -Destination $Path -Force
}

function Read-GuestLogChunk {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][ref]$ByteOffset,
        [scriptblock]$Writer = { param([string]$Text) [Console]::Write($Text) }
    )
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return }
    $share = [System.IO.FileShare]([System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete)
    $fs = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, $share)
    try {
        if ($fs.Length -le $ByteOffset.Value) { return }
        $fs.Seek($ByteOffset.Value, [System.IO.SeekOrigin]::Begin) | Out-Null
        $sr = New-Object System.IO.StreamReader($fs)
        $text = $sr.ReadToEnd()
        $ByteOffset.Value = $fs.Position
        if ($text.Length -gt 0) {
            & $Writer $text
        }
    } finally {
        $fs.Close()
    }
}

function Open-GuestSession1Process {
    param(
        [Parameter(Mandatory = $true)][int]$ProcessId,
        [Parameter(Mandatory = $true)][string]$ExpectedProcessName,
        [Parameter(Mandatory = $true)][scriptblock]$GetProcessById
    )
    try {
        $proc = & $GetProcessById $ProcessId
    } catch [System.ArgumentException] {
        return @{ Process = $null; Reason = 'ProcessExited'; ExitCode = $null; Warning = '' }
    }
    if ($null -eq $proc) {
        return @{ Process = $null; Reason = 'ProcessExited'; ExitCode = $null; Warning = '' }
    }

    $attached = $false
    try {
        # Reading HasExited opens and retains the native OS process handle before checking
        # ProcessName, preventing PID reuse while the handle is held.
        if ([bool]$proc.HasExited) {
            $code = try { $proc.ExitCode } catch { $null }
            return @{ Process = $null; Reason = 'ProcessExited'; ExitCode = $code; Warning = '' }
        }
        $procName = try { [string]$proc.ProcessName } catch { '' }
        if ([string]::IsNullOrEmpty($procName) -and [bool]$proc.HasExited) {
            $code = try { $proc.ExitCode } catch { $null }
            return @{ Process = $null; Reason = 'ProcessExited'; ExitCode = $code; Warning = '' }
        }
        if (-not [string]::IsNullOrEmpty($ExpectedProcessName) -and $procName -ine $ExpectedProcessName) {
            return @{
                Process  = $null
                Reason   = 'PidReused'
                ExitCode = $null
                Warning  = "Session 1 PID $ProcessId belongs to '$procName' instead of '$ExpectedProcessName'; treating original process as exited."
            }
        }
        $attached = $true
        return @{ Process = $proc; Reason = 'Attached'; ExitCode = $null; Warning = '' }
    } finally {
        if (-not $attached) {
            try {
                $proc.Dispose()
            } catch {
                Write-Verbose "Ignoring Session 1 process handle dispose error: $($_.Exception.Message)"
            }
        }
    }
}

function Wait-GuestSession1Task {
    [CmdletBinding()]
    [OutputType([hashtable])]
    param(
        [Parameter(Mandatory = $true)][string]$TaskName,
        [Parameter(Mandatory = $true)][string]$LogPath,
        [Parameter(Mandatory = $true)][string]$PidPath,
        [Parameter(Mandatory = $true)][string]$ExitPath,
        [int]$StartTimeoutSeconds = 60,
        [int]$ExitGraceSeconds = 15,
        [int]$PollIntervalMs = 400,
        [int]$TaskPollIntervalMs = 2000,
        [int]$FinalDrainMs = 200,
        [string]$ExpectedProcessName = 'powershell',
        [scriptblock]$GetTaskState = { param($Name) (Get-ScheduledTask -TaskName $Name).State.ToString() },
        [scriptblock]$GetTaskInfo = { param($Name) Get-ScheduledTaskInfo -TaskName $Name -ErrorAction SilentlyContinue },
        [scriptblock]$GetProcessById = { param($Id) [System.Diagnostics.Process]::GetProcessById($Id) },
        [scriptblock]$LogWriter = { param([string]$Text) [Console]::Write($Text) }
    )

    $offset = [int64]0
    $taskPollErrors = 0
    $childPid = $null
    $childProc = $null
    $childProcExitCode = $null
    $childExited = $false
    $hasExit = $false
    $exitReason = ''

    try {
        # Phase 1: Startup window — wait until Session 1 writes its PID (or exit) file.
        # Poll Get-ScheduledTask slowly inside try/catch so transient WMI/CIM errors (e.g., 0x80070002)
        # warn and retry instead of aborting. Always enforce StartTimeoutSeconds until a handoff file appears.
        $sawTaskRunning = $false
        $lastTaskState = '<unknown>'
        $lastTaskError = ''
        $startDeadline = (Get-Date).AddSeconds($StartTimeoutSeconds)
        $nextTaskPoll = Get-Date

        while ($true) {
            Read-GuestLogChunk -Path $LogPath -ByteOffset ([ref]$offset) -Writer $LogWriter

            if (Test-Path -LiteralPath $PidPath -PathType Leaf) {
                $rawPid = Get-Content -LiteralPath $PidPath -Raw -ErrorAction SilentlyContinue
                if ($null -ne $rawPid -and $rawPid.Trim() -match '^\d+$') {
                    $childPid = [int]$rawPid.Trim()
                    break
                }
            }
            if (Test-Path -LiteralPath $ExitPath -PathType Leaf) {
                $childExited = $true
                $exitReason = 'ExitFile'
                break
            }

            $now = Get-Date
            if ($now -ge $nextTaskPoll) {
                $nextTaskPoll = $now.AddMilliseconds($TaskPollIntervalMs)
                try {
                    $state = & $GetTaskState $TaskName
                    if (-not [string]::IsNullOrWhiteSpace($state)) {
                        $lastTaskState = $state
                        if ($state -eq 'Running') { $sawTaskRunning = $true }
                    }
                } catch {
                    $taskPollErrors++
                    $lastTaskError = $_.Exception.Message
                    if ($taskPollErrors -eq 1 -or ($taskPollErrors % 5) -eq 0) {
                        [Console]::Out.Flush()
                        Write-Warning "Transient scheduled task query error #$taskPollErrors for '$TaskName': $lastTaskError"
                    }
                }
            }

            if ($sawTaskRunning -and $lastTaskState -ne 'Running' -and $lastTaskState -ne 'Queued') {
                $childExited = $true
                $exitReason = 'TaskExitedBeforePid'
                break
            }
            if ($now -gt $startDeadline) {
                $taskInfo = try { & $GetTaskInfo $TaskName } catch { $null }
                $lastResult = if ($null -ne $taskInfo -and $null -ne $taskInfo.PSObject.Properties['LastTaskResult']) {
                    $taskInfo.LastTaskResult
                } else {
                    '<unavailable>'
                }
                $errSuffix = if (-not [string]::IsNullOrEmpty($lastTaskError)) { ", LastCimError=$lastTaskError" } else { '' }
                throw "Scheduled task '$TaskName' never started Session 1 within ${StartTimeoutSeconds}s (State=$lastTaskState, LastTaskResult=$lastResult$errSuffix)."
            }

            Start-Sleep -Milliseconds $PollIntervalMs
        }

        # Phase 2: Direct OS process monitoring — zero WMI/CIM calls for the rest of the shard.
        if ($null -ne $childPid) {
            $attach = Open-GuestSession1Process `
                -ProcessId $childPid `
                -ExpectedProcessName $ExpectedProcessName `
                -GetProcessById $GetProcessById
            $childProc = $attach.Process
            if (-not [string]::IsNullOrEmpty($attach.Warning)) {
                [Console]::Out.Flush()
                Write-Warning $attach.Warning
            }

            if ($null -eq $childProc) {
                $childExited = $true
                $exitReason = $attach.Reason
                $childProcExitCode = $attach.ExitCode
            } else {
                $exitFileSeenAt = $null
                while ($true) {
                    Read-GuestLogChunk -Path $LogPath -ByteOffset ([ref]$offset) -Writer $LogWriter
                    $hasExit = Test-Path -LiteralPath $ExitPath -PathType Leaf
                    if ($hasExit -and $null -eq $exitFileSeenAt) {
                        $exitFileSeenAt = Get-Date
                    }
                    if ([bool]$childProc.HasExited) {
                        $childExited = $true
                        $exitReason = 'ProcessExited'
                        break
                    }
                    if ($hasExit -and ((Get-Date) - $exitFileSeenAt).TotalSeconds -ge $ExitGraceSeconds) {
                        [Console]::Out.Flush()
                        Write-Warning "Session 1 wrote $ExitPath but PID $childPid did not exit within ${ExitGraceSeconds}s; proceeding."
                        $exitReason = 'ExitFileGraceTimeout'
                        break
                    }
                    Start-Sleep -Milliseconds $PollIntervalMs
                }
            }
        }

        # Final log drain and exit-file reconciliation.
        Read-GuestLogChunk -Path $LogPath -ByteOffset ([ref]$offset) -Writer $LogWriter
        if ($FinalDrainMs -gt 0) {
            Start-Sleep -Milliseconds $FinalDrainMs
            Read-GuestLogChunk -Path $LogPath -ByteOffset ([ref]$offset) -Writer $LogWriter
        }

        $hasExit = Test-Path -LiteralPath $ExitPath -PathType Leaf
        if ($hasExit) {
            if ($exitReason -ne 'ExitFileGraceTimeout') {
                $exitReason = 'ExitFile'
            }
        } elseif ($exitReason -eq 'ProcessExited') {
            $exitReason = 'ProcessExitedWithoutExitFile'
            $procExitCode = if ($null -ne $childProc) {
                try { $childProc.ExitCode } catch { $null }
            } else {
                $childProcExitCode
            }
            $codeDesc = if ($null -ne $procExitCode) {
                $u32 = [BitConverter]::ToUInt32([BitConverter]::GetBytes([int]$procExitCode), 0)
                "$procExitCode (0x{0:X8})" -f $u32
            } else {
                '<unknown>'
            }
            [Console]::Out.Flush()
            Write-Warning "Session 1 process (PID $childPid) exited with code $codeDesc without writing $ExitPath."
        }
    } finally {
        if ($null -ne $childProc) {
            try {
                $childProc.Dispose()
            } catch {
                Write-Verbose "Ignoring Session 1 process handle dispose error: $($_.Exception.Message)"
            }
        }
    }

    $childExit = 1
    if ($hasExit) {
        $rawExit = Get-Content -LiteralPath $ExitPath -Raw -ErrorAction SilentlyContinue
        if ($null -ne $rawExit -and $rawExit.Trim() -match '^-?\d+$') {
            $childExit = [int]$rawExit.Trim()
        }
    }
    return @{
        ExitCode       = $childExit
        ChildPid       = $childPid
        ChildExited    = $childExited
        SawExitFile    = $hasExit
        ExitReason     = $exitReason
        TaskPollErrors = $taskPollErrors
    }
}

# Win32-OpenSSH runs in Session 0 (non-interactive service WindowStation), where IFSTest's
# CreateProcessWithLogonW tests (SetOwnerSecurityTest, DirectoryTraverseTest) fail with
# C0000095 (IFSTEST_SETUP_PROCESS_CREATEPROC_FAILURE). Trampoline into interactive Session 1
# (logged in via AutoAdminLogon) and stream the output back over SSH.
if (-not $InSession1 -and (Get-Process -Id $PID).SessionId -eq 0) {
    # Do NOT use Win32_ComputerSystem.UserName: it relies on explorer.exe and returns empty on
    # Windows Server Core. Wait for an active interactive logon (LogonType = 2) plus a user-shell
    # process running in Session >= 1 instead.
    $sessionReady = $false
    for ($i = 0; $i -lt 60; $i++) {
        $interactiveLogon = @(Get-CimInstance -ClassName Win32_LogonSession -Filter 'LogonType = 2' -ErrorAction SilentlyContinue)
        $shellProc = @(Get-Process -Name 'cmd', 'powershell', 'explorer', 'sihost', 'conhost' -ErrorAction SilentlyContinue |
            Where-Object { $_.SessionId -ge 1 })
        if ($interactiveLogon.Count -gt 0 -and $shellProc.Count -gt 0) {
            $sessionReady = $true
            break
        }
        Start-Sleep -Seconds 1
    }
    if (-not $sessionReady) {
        $diagProcs = (Get-Process | Where-Object { $_.SessionId -ge 1 } | Select-Object -First 10 -ExpandProperty ProcessName) -join ', '
        throw "Timed out waiting for interactive Session 1 AutoAdminLogon (Session>=1 processes: $diagProcs)."
    }
    $consoleUser = "$env:COMPUTERNAME\Administrator"
    Write-Host "=== Trampolining from Session 0 into interactive Session 1 as $consoleUser ===" -ForegroundColor Cyan

    # TaskScheduler/Operational is disabled by default on Windows Server; enable it best-effort
    # so failure diagnostics capture task lifecycle events without modifying baked VM images.
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & wevtutil.exe sl 'Microsoft-Windows-TaskScheduler/Operational' /e:true 2>&1 | Out-Null
    } catch {
        Write-Verbose "Skipping TaskScheduler/Operational enablement: $($_.Exception.Message)"
    } finally {
        $ErrorActionPreference = $prevEap
    }

    $taskName = 'DokanCITests'
    $logPath = 'C:\ci\guest_tests.log'
    $pidPath = 'C:\ci\guest_tests.pid'
    $exitPath = 'C:\ci\guest_tests.exit'
    Remove-Item -LiteralPath $logPath, $pidPath, "$pidPath.tmp", $exitPath, "$exitPath.tmp" -Force -ErrorAction SilentlyContinue

    $psArgs = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -InSession1 -Shard $Shard -PidFile `"$pidPath`" -ExitFile `"$exitPath`""
    if ($SimulateBugCheck) { $psArgs += ' -SimulateBugCheck' }
    if ($SimulateHang) { $psArgs += ' -SimulateHang' }

    $action = New-ScheduledTaskAction -Execute 'cmd.exe' -Argument "/d /c `"powershell.exe $psArgs > `"$logPath`" 2>&1`""
    $principal = New-ScheduledTaskPrincipal -UserId $consoleUser -LogonType Interactive -RunLevel Highest
    $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Hours 2)
    Register-ScheduledTask -TaskName $taskName -Action $action -Principal $principal -Settings $settings -Force | Out-Null

    $childExited = $false
    $waitResult = $null
    try {
        Start-ScheduledTask -TaskName $taskName
        $waitResult = Wait-GuestSession1Task `
            -TaskName $taskName `
            -LogPath $logPath `
            -PidPath $pidPath `
            -ExitPath $exitPath
        $childExited = $waitResult.ChildExited
    } finally {
        if (-not $childExited) {
            Stop-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
        }
        Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue
    }

    if ($waitResult.ExitCode -ne 0 -or $waitResult.TaskPollErrors -gt 0) {
        Write-Host "=== Session 1 trampoline summary: PID=$($waitResult.ChildPid), Reason=$($waitResult.ExitReason), Exit=$($waitResult.ExitCode), TaskPollErrors=$($waitResult.TaskPollErrors) ===" -ForegroundColor Yellow
    }
    exit $waitResult.ExitCode
}

if (-not [string]::IsNullOrWhiteSpace($PidFile)) {
    Write-AtomicAsciiFile -Path $PidFile -Value "$PID"
}

function Assert-NoBugCheck {
    $bugChecks = Get-WinEvent -FilterHashtable @{ LogName = 'System'; Id = 1001; ProviderName = 'Microsoft-Windows-WER-SystemErrorReporting' } -ErrorAction SilentlyContinue
    if ($bugChecks) {
        $bugChecks | Format-List TimeCreated, Message | Out-String | Write-Host
        throw 'BugCheck event found in the System event log.'
    }
}

$scriptExit = 1
try {
    . (Join-Path -Path $PSScriptRoot -ChildPath 'repo\scripts\build_helper.ps1')
    . (Join-Path -Path $PSScriptRoot -ChildPath 'verifier_helpers.ps1')
    $os = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
    $sessionId = (Get-Process -Id $PID).SessionId
    Write-Host "=== $($os.ProductName), build $($os.CurrentBuildNumber).$($os.UBR), Session $sessionId, shard $Shard ===" -ForegroundColor Cyan

    # The image clears the pagefile at shutdown only so the bake compacts well; skip that for test VMs.
    Set-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management' -Name 'ClearPageFileAtShutdown' -Type DWord -Value 0

    if ((& bcdedit.exe /enum '{current}' | Out-String) -notmatch 'testsigning\s+Yes') {
        throw 'Test signing is not enabled in this image.'
    }

    Write-Host '=== Installing Dokan ===' -ForegroundColor Cyan
    $install = Start-Process -PassThru -Wait -FilePath 'C:\ci\DokanSetup.exe' -ArgumentList @('/quiet', '/norestart', '/log', 'C:\ci\dokan_install.log')
    if ($install.ExitCode -ne 0) {
        Get-Content 'C:\ci\dokan_install*.log' -ErrorAction SilentlyContinue | Select-Object -Last 80
        throw "DokanSetup.exe failed with exit code $($install.ExitCode)"
    }

    $verifierState = & verifier.exe /query | Out-String
    Write-Host $verifierState
    Assert-DriverVerifierActive -QueryOutput $verifierState

    if ($SimulateHang) {
        Write-Host '=== Self-test: blocking so the host detects a hang and injects an NMI ===' -ForegroundColor Yellow
        Start-Sleep -Seconds 3600
        throw 'The host did not interrupt the simulated hang.'
    }
    if ($SimulateBugCheck) {
        Write-Host '=== Self-test: raising BugCheck 0xDEADDEAD ===' -ForegroundColor Yellow
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class CrashTrigger {
    [DllImport("ntdll.dll")]
    static extern uint RtlAdjustPrivilege(int privilege, bool enable, bool currentThread, out bool enabled);
    [DllImport("ntdll.dll")]
    static extern uint NtRaiseHardError(uint status, uint parameterCount, uint stringMask, IntPtr parameters, uint responseOption, out uint response);
    public static void BugCheck() {
        bool enabled;
        uint response;
        RtlAdjustPrivilege(19 /* SeShutdownPrivilege */, true, false, out enabled);
        NtRaiseHardError(0xDEADDEAD, 0, 0, IntPtr.Zero, 6 /* OptionShutdownSystem */, out response);
    }
}
'@
        [CrashTrigger]::BugCheck()
        throw 'NtRaiseHardError returned without crashing the system.'
    }

    # Normalize token privileges to match an interactive elevated administrator (only SeChangeNotify,
    # SeImpersonate and SeCreateGlobal enabled; all others remain present-but-disabled so tools that
    # need them can re-enable them on demand, while Kernel32 FILE_OPEN_FOR_BACKUP_INTENT never silently
    # bypasses deny DACLs in security tests).
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
public static class TokenPrivileges {
    const uint SE_PRIVILEGE_ENABLED = 0x2;
    const uint TOKEN_ADJUST_PRIVILEGES = 0x20, TOKEN_QUERY = 0x8;
    const int TokenPrivilegesClass = 3;
    const int ERROR_INSUFFICIENT_BUFFER = 122;
    [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool GetTokenInformation(IntPtr token, int infoClass, IntPtr info, int length, out int returnLength);
    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool AdjustTokenPrivileges(IntPtr token, bool disableAll, IntPtr newState, int length, IntPtr previous, IntPtr returnLength);
    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool LookupPrivilegeNameW(string system, ref long luid, StringBuilder name, ref int length);

    // Disables every enabled privilege whose name is not in 'keep'; returns the names it disabled.
    public static string[] DisableAllExcept(string[] keep) {
        IntPtr token;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, out token)) throw new Win32Exception();
        var disabled = new List<string>();
        IntPtr info = IntPtr.Zero;
        try {
            int length;
            if (!GetTokenInformation(token, TokenPrivilegesClass, IntPtr.Zero, 0, out length)) {
                int err = Marshal.GetLastWin32Error();
                if (err != ERROR_INSUFFICIENT_BUFFER || length <= 0) throw new Win32Exception(err);
            }
            info = Marshal.AllocHGlobal(length);
            if (!GetTokenInformation(token, TokenPrivilegesClass, info, length, out length)) throw new Win32Exception();
            int count = Marshal.ReadInt32(info);
            for (int i = 0; i < count; i++) {
                IntPtr entry = IntPtr.Add(info, 4 + i * 12);  // TOKEN_PRIVILEGES: count, then LUID_AND_ATTRIBUTES[]
                long luid = Marshal.ReadInt64(entry);
                uint attributes = (uint)Marshal.ReadInt32(IntPtr.Add(entry, 8));
                if ((attributes & SE_PRIVILEGE_ENABLED) == 0) continue;
                var name = new StringBuilder(64);
                int nameLength = name.Capacity;
                string privilege = LookupPrivilegeNameW(null, ref luid, name, ref nameLength) ? name.ToString() : luid.ToString();
                if (Array.IndexOf(keep, privilege) >= 0) continue;
                IntPtr state = Marshal.AllocHGlobal(16);
                try {
                    Marshal.WriteInt32(state, 1);
                    Marshal.WriteInt64(IntPtr.Add(state, 4), luid);
                    Marshal.WriteInt32(IntPtr.Add(state, 12), 0);
                    if (!AdjustTokenPrivileges(token, false, state, 0, IntPtr.Zero, IntPtr.Zero)) throw new Win32Exception();
                    int adjustErr = Marshal.GetLastWin32Error();
                    if (adjustErr != 0) throw new Win32Exception(adjustErr);
                } finally {
                    Marshal.FreeHGlobal(state);
                }
                disabled.Add(privilege);
            }
        } finally {
            if (info != IntPtr.Zero) Marshal.FreeHGlobal(info);
            CloseHandle(token);
        }
        return disabled.ToArray();
    }
}
'@
    $disabledPrivileges = [TokenPrivileges]::DisableAllExcept(@('SeChangeNotifyPrivilege', 'SeImpersonatePrivilege', 'SeCreateGlobalPrivilege'))
    Write-Host "Disabled privileges for the tests: $($disabledPrivileges -join ', ')"
    & whoami.exe /user /priv

    $env:DokanLibrary3 = [System.Environment]::GetEnvironmentVariable('DokanLibrary3', 'Machine')
    if ([string]::IsNullOrWhiteSpace($env:DokanLibrary3)) {
        throw 'DokanLibrary3 machine environment variable was not set by DokanSetup.exe.'
    }
    $fsTestExe = 'C:\ci\fstest\dokan_fstest.exe'
    $baselinesDir = 'C:\ci\fstest\baselines'
    $resultsDir = 'C:\ci\fstest-results'
    New-Item -ItemType Directory -Force -Path $resultsDir | Out-Null
    $fsTestArgs = @{ FsTestExe = $fsTestExe; BaselinesDir = $baselinesDir; ResultsDir = $resultsDir }

    $memfs = @("$env:DokanLibrary3\sample\memfs\memfs.exe", "$env:DokanLibrary3\x86\sample\memfs\memfs.exe")
    $mirrorX64 = "$env:DokanLibrary3\sample\mirror\mirror.exe"
    $mirrorX86 = "$env:DokanLibrary3\x86\sample\mirror\mirror.exe"

    if ($Shard -in @('all', 'shard1')) {
        Write-Host '=== Real NTFS --strict gate (C:\fstest_ntfs) ===' -ForegroundColor Cyan
        $ntfsRoot = 'C:\fstest_ntfs'
        New-Item -ItemType Directory -Force -Path $ntfsRoot | Out-Null
        try {
            Exec-External { & $fsTestExe "--root=$ntfsRoot" '--strict' "--json-out=$resultsDir\ntfs_strict.json" }
        } finally {
            & $fsTestExe "--cleanup-dir=$ntfsRoot"
            if ($LASTEXITCODE -ne 0) {
                & cmd.exe /d /c "rmdir /s /q \\?\$ntfsRoot"
            }
        }
    }

    Push-Location 'C:\ci\repo\samples'
    try {
        switch ($Shard) {
            'shard1' {
                .\memfs_test.ps1 -Memfs $memfs @fsTestArgs
                .\mirror_test.ps1 -Mirrors @($mirrorX64) -ConfigNames @('drive', 'dirJunction', 'driveRemovable', 'netUnc') @fsTestArgs
            }
            'shard2' {
                .\mirror_test.ps1 -Mirrors @($mirrorX64) -ConfigNames @('driveMntMgr', 'driveRemovableMntMgr') @fsTestArgs
                .\mirror_test.ps1 -Mirrors @($mirrorX86) -ConfigNames @('drive', 'netUnc') @fsTestArgs
            }
            'all' {
                .\memfs_test.ps1 -Memfs $memfs @fsTestArgs
                .\mirror_test.ps1 -Mirrors @($mirrorX64) @fsTestArgs
                .\mirror_test.ps1 -Mirrors @($mirrorX86) -ConfigNames @('drive', 'netUnc') @fsTestArgs
            }
        }
    } finally {
        Pop-Location
    }

    if ($Shard -in @('all', 'shard2')) {
        $npResultsDir = 'C:\ci\np-results'
        New-Item -ItemType Directory -Force -Path $npResultsDir | Out-Null
        $npTestX64 = 'C:\ci\np\x64\dokan_np_test.exe'
        $npTestX86 = 'C:\ci\np\x86\dokan_np_test.exe'
        Write-Host '=== Running dokan_np_test (x64) ===' -ForegroundColor Cyan
        Exec-External { & $npTestX64 "--mirror_exe=$mirrorX64" '--secondary_user=dokan_ifstest' '--secondary_pass=D0kan_1fstest' '--test_live_registry' "--gtest_output=xml:$npResultsDir\dokan_np_test_x64.xml" }
        Write-Host '=== Running dokan_np_test (x86 / WOW64) ===' -ForegroundColor Cyan
        Exec-External { & $npTestX86 "--mirror_exe=$mirrorX86" '--secondary_user=dokan_ifstest' '--secondary_pass=D0kan_1fstest' "--gtest_output=xml:$npResultsDir\dokan_np_test_x86.xml" }
    }

    Write-Host '=== Driver Verifier statistics ===' -ForegroundColor Cyan
    Exec-External { & verifier.exe /query }
    Assert-NoBugCheck
    Write-Host "=== Shard $Shard passed ===" -ForegroundColor Green
    $scriptExit = 0
} catch {
    Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
    if ($_.ScriptStackTrace) {
        Write-Host $_.ScriptStackTrace -ForegroundColor Red
    }
    $scriptExit = 1
} finally {
    if (-not [string]::IsNullOrWhiteSpace($ExitFile)) {
        Write-AtomicAsciiFile -Path $ExitFile -Value "$scriptExit"
    }
}
exit $scriptExit
