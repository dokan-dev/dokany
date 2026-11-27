<#
.SYNOPSIS
    Collects concise guest-side failure diagnostics inside a CI test VM.

.DESCRIPTION
    Staged into C:\ci\ by run_vm_tests.sh and invoked over SSH only when a test shard fails
    (or produces an unexpected MEMORY.DMP). Captures OS/boot metadata, Session-1 trampoline
    file/task state, live test processes, and recent Windows Event Log entries without failing
    the caller if any individual subsystem (such as WMI or a log channel) is unhealthy.
#>
param(
    [int]$LookbackMinutes = 60,
    [int]$MaxEvents = 40
)

$ErrorActionPreference = 'Continue'
$since = (Get-Date).AddMinutes(-$LookbackMinutes)

function Write-DiagSection {
    param(
        [Parameter(Mandatory = $true)][string]$Title,
        [Parameter(Mandatory = $true)][scriptblock]$Body
    )
    Write-Output ''
    Write-Output "=== $Title ==="
    try {
        & $Body
    } catch {
        Write-Output "[collection error] $($_.Exception.Message)"
    }
}

function Write-DiagEvents {
    param(
        [Parameter(Mandatory = $true)][string]$LogName,
        [int[]]$Levels = @(),
        [int]$Limit = 40
    )
    Write-DiagSection -Title "EventLog: $LogName" -Body {
        $logInfo = Get-WinEvent -ListLog $LogName -ErrorAction SilentlyContinue
        if ($null -ne $logInfo -and -not $logInfo.IsEnabled) {
            Write-Output '(event log channel is disabled)'
            return
        }
        $filter = @{ LogName = $LogName; StartTime = $since }
        if ($null -ne $Levels -and $Levels.Count -gt 0) {
            $filter['Level'] = $Levels
        }
        $events = @(Get-WinEvent -FilterHashtable $filter -MaxEvents $Limit -ErrorAction SilentlyContinue)
        if ($events.Count -eq 0) {
            Write-Output "(no matching events in last ${LookbackMinutes}m)"
            return
        }
        foreach ($ev in $events) {
            $ts = if ($null -ne $ev.TimeCreated) { $ev.TimeCreated.ToUniversalTime().ToString('o') } else { '<n/a>' }
            $msg = if ($null -ne $ev.Message) { ($ev.Message -replace '\r?\n', ' ').Trim() } else { '' }
            if ($msg.Length -gt 400) {
                $msg = $msg.Substring(0, 400) + '...'
            }
            Write-Output ("[{0}] Id={1} Level={2} Provider={3} :: {4}" -f $ts, $ev.Id, $ev.LevelDisplayName, $ev.ProviderName, $msg)
        }
    }
}

Write-DiagSection -Title 'System Info' -Body {
    Write-Output "Guest UTC: $([DateTime]::UtcNow.ToString('o'))"
    $cv = Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion' -ErrorAction SilentlyContinue
    if ($null -ne $cv) {
        Write-Output "OS: $($cv.ProductName) ($($cv.CurrentBuildNumber).$($cv.UBR))"
    }
    $os = Get-CimInstance -ClassName Win32_OperatingSystem -ErrorAction SilentlyContinue
    if ($null -ne $os -and $null -ne $os.LastBootUpTime) {
        Write-Output "LastBootUpTime: $($os.LastBootUpTime.ToUniversalTime().ToString('o'))"
    }
}

Write-DiagSection -Title 'Trampoline State' -Body {
    $files = @(Get-ChildItem -Path 'C:\ci\guest_tests*' -ErrorAction SilentlyContinue)
    if ($files.Count -eq 0) {
        Write-Output 'C:\ci\guest_tests*: (none)'
    } else {
        foreach ($f in $files) {
            Write-Output ("File: {0} Size={1} LastWriteUtc={2}" -f $f.Name, $f.Length, $f.LastWriteTimeUtc.ToString('o'))
        }
    }
    foreach ($path in @('C:\ci\guest_tests.pid', 'C:\ci\guest_tests.exit')) {
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            $raw = Get-Content -LiteralPath $path -Raw -ErrorAction SilentlyContinue
            $val = if ($null -ne $raw) { $raw.Trim() } else { '<empty>' }
            Write-Output "$path = $val"
        }
    }
    Get-Service -Name 'Schedule', 'Winmgmt' -ErrorAction SilentlyContinue |
        ForEach-Object { Write-Output "Service $($_.Name): $($_.Status)" }
    try {
        $task = Get-ScheduledTask -TaskName 'DokanCITests' -ErrorAction Stop
        $info = Get-ScheduledTaskInfo -TaskName 'DokanCITests' -ErrorAction SilentlyContinue
        $lastResult = if ($null -ne $info) { $info.LastTaskResult } else { '<unavailable>' }
        $lastRun = if ($null -ne $info) { $info.LastRunTime } else { '<unavailable>' }
        Write-Output "Task DokanCITests: State=$($task.State) LastRunTime=$lastRun LastTaskResult=$lastResult"
    } catch {
        if ($null -ne $_.CategoryInfo -and $_.CategoryInfo.Category -eq [System.Management.Automation.ErrorCategory]::ObjectNotFound) {
            Write-Output 'Task DokanCITests: (not registered)'
        } else {
            Write-Output "Task DokanCITests: $($_.Exception.Message)"
        }
    }
}

Write-DiagSection -Title 'Live Test Processes' -Body {
    $names = @('powershell', 'pwsh', 'cmd', 'ifstest', 'mirror', 'memfs', 'dokan_fstest', 'fsx', 'DokanSetup', 'msiexec', 'WmiPrvSE')
    $procs = @(Get-Process -Name $names -ErrorAction SilentlyContinue | Sort-Object SessionId, ProcessName, Id)
    if ($procs.Count -eq 0) {
        Write-Output '(none)'
        return
    }
    $cmdMap = @{}
    try {
        Get-CimInstance -ClassName Win32_Process -ErrorAction SilentlyContinue |
            ForEach-Object { $cmdMap[[int]$_.ProcessId] = $_.CommandLine }
    } catch {
        $cmdMap = @{}
    }
    foreach ($p in $procs) {
        $startUtc = try {
            $p.StartTime.ToUniversalTime().ToString('o')
        } catch {
            '<n/a>'
        }
        $cmd = if ($cmdMap.ContainsKey($p.Id) -and $null -ne $cmdMap[$p.Id]) { [string]$cmdMap[$p.Id] } else { '' }
        if ($cmd.Length -gt 240) {
            $cmd = $cmd.Substring(0, 240) + '...'
        }
        $wsMb = [math]::Round($p.WorkingSet64 / 1MB, 1)
        Write-Output ("PID={0} Sess={1} Name={2} StartUtc={3} WS={4}MB Cmd={5}" -f $p.Id, $p.SessionId, $p.ProcessName, $startUtc, $wsMb, $cmd)
    }
}

Write-DiagEvents -LogName 'System' -Levels @(1, 2, 3) -Limit $MaxEvents
Write-DiagEvents -LogName 'Application' -Levels @(1, 2, 3) -Limit $MaxEvents
Write-DiagEvents -LogName 'Microsoft-Windows-TaskScheduler/Operational' -Levels @() -Limit ($MaxEvents + 10)
Write-DiagEvents -LogName 'Microsoft-Windows-WMI-Activity/Operational' -Levels @(1, 2, 3) -Limit $MaxEvents
