<#
.SYNOPSIS
    Analyzes MEMORY.DMP.7z crash dumps collected from the CI test VMs with kd.exe.

.DESCRIPTION
    For every <DumpsRoot>\<artifact>\MEMORY.DMP.7z, runs kd.exe with dokan3.sys/dokan3.pdb from
    <SymbolsDir> plus the Microsoft symbol server, writes the full report to <OutputDir>, and adds
    a summary (bugcheck code, faulting module, excerpted report in a collapsed section) to the job summary.
    The command set is a superset of the former AppVeyor one (!analyze -v; kp; dv /t /i /V), extended
    with !running, !locks and !stacks for hangs that the CI turns into dumps with an NMI.
#>
param(
    [Parameter(Mandatory = $true)]
    [string]$DumpsRoot,
    [Parameter(Mandatory = $true)]
    [string]$SymbolsDir,
    [Parameter(Mandatory = $true)]
    [string]$OutputDir,
    # Fail when no dump is found (used by the crash-capture self-test).
    [switch]$RequireDump
)

$ErrorActionPreference = 'Stop'

function Format-SafeCodeBlockLine {
    param(
        [AllowEmptyString()]
        [string]$Line = ''
    )

    if ($Line -match '~~~~') {
        return ($Line -replace '~~~', '~ ~ ~')
    }
    return $Line
}

function Get-SummaryExcerpt {
    param(
        [string[]]$ReportLines,
        [int]$MaxAnalyzeLines = 160,
        [int]$MaxTotalLines = 500
    )

    $analyzeLines = New-Object 'System.Collections.Generic.List[string]'
    $sections = @{}
    $currentSection = $null

    foreach ($line in $ReportLines) {
        if ($line -match '^===== (.+) =====\s*$') {
            $sectionName = $Matches[1].Trim()
            if (-not $sections.ContainsKey($sectionName)) {
                $sections[$sectionName] = New-Object 'System.Collections.Generic.List[string]'
            }
            $currentSection = $sections[$sectionName]
            $currentSection.Add($line)
            continue
        }
        if ($line -match '^(\d+:\s*)?kd>\s+(\.echo\s+=====|q\s*$)') {
            continue
        }
        if ($null -eq $currentSection) {
            if ($line -match '^\s*Hypervisor\.Flags\.') {
                continue
            }
            $analyzeLines.Add($line)
        } else {
            $currentSection.Add($line)
        }
    }

    $result = New-Object 'System.Collections.Generic.List[string]'
    if ($analyzeLines.Count -gt $MaxAnalyzeLines) {
        for ($i = 0; $i -lt ($MaxAnalyzeLines - 1); $i++) {
            $result.Add($analyzeLines[$i])
        }
        $result.Add('[... !analyze -v truncated; see crash-analysis artifact for full report ...]')
    } else {
        foreach ($line in $analyzeLines) {
            $result.Add($line)
        }
    }

    $wantedSections = @(
        'Stack',
        'dokan3.sys',
        'Contended locks',
        'Threads with dokan3 frames',
        'Driver Verifier'
    )
    $presentNames = @($wantedSections | Where-Object { $sections.ContainsKey($_) })
    $budget = $MaxTotalLines - $result.Count
    if ($budget -gt 0 -and $presentNames.Count -gt 0) {
        $totalSectionLines = 0
        foreach ($name in $presentNames) {
            $totalSectionLines += $sections[$name].Count
        }

        $caps = @{}
        if ($totalSectionLines -le $budget) {
            foreach ($name in $presentNames) {
                $caps[$name] = $sections[$name].Count
            }
        } else {
            $remainingBudget = $budget
            $sortedNames = @($presentNames | Sort-Object { $sections[$_].Count })
            for ($i = 0; $i -lt $sortedNames.Count; $i++) {
                $name = $sortedNames[$i]
                $remainingSections = $sortedNames.Count - $i
                $share = [Math]::Max(1, [int][Math]::Floor($remainingBudget / $remainingSections))
                $take = [Math]::Min($sections[$name].Count, $share)
                $caps[$name] = $take
                $remainingBudget -= $take
            }
        }

        foreach ($name in $presentNames) {
            $secLines = $sections[$name]
            $cap = $caps[$name]
            if ($secLines.Count -gt $cap) {
                if ($cap -gt 1) {
                    for ($i = 0; $i -lt ($cap - 1); $i++) {
                        $result.Add($secLines[$i])
                    }
                }
                $result.Add('[... section truncated; see crash-analysis artifact for full report ...]')
            } else {
                foreach ($line in $secLines) {
                    $result.Add($line)
                }
            }
        }
    }

    $safeResult = New-Object 'System.Collections.Generic.List[string]'
    foreach ($line in $result) {
        $safeResult.Add((Format-SafeCodeBlockLine -Line $line))
    }
    return $safeResult.ToArray()
}

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

$kd = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\kd.exe'
if (-not (Test-Path -LiteralPath $kd -PathType Leaf)) {
    throw "kd.exe not found at $kd"
}
$sevenZip = (Get-Command 7z.exe -ErrorAction SilentlyContinue | Select-Object -First 1).Source
if (-not $sevenZip) {
    $sevenZip = 'C:\Program Files\7-Zip\7z.exe'
}
if (-not (Test-Path -LiteralPath $sevenZip -PathType Leaf)) {
    throw "7z.exe not found at $sevenZip"
}

$archives = @(Get-ChildItem -Path $DumpsRoot -Recurse -Filter 'MEMORY.DMP.7z' -File -ErrorAction SilentlyContinue)
if ($archives.Count -eq 0) {
    if ($RequireDump) {
        throw "No MEMORY.DMP.7z found under $DumpsRoot."
    }
    Write-Host "No MEMORY.DMP.7z found under $DumpsRoot."
    exit 0
}

$tempRoot = if (-not [string]::IsNullOrWhiteSpace($env:RUNNER_TEMP)) {
    $env:RUNNER_TEMP
} else {
    [System.IO.Path]::GetTempPath()
}
$symbolCache = Join-Path $tempRoot 'symbols'
$symbolPath = "$SymbolsDir;srv*$symbolCache*https://msdl.microsoft.com/download/symbols"
$kdCommands = @(
    '.reload /f dokan3.sys'
    '!analyze -v'
    '.echo ===== Stack ====='
    'kP 30'
    '.echo ===== Locals ====='
    'dv /t /i /V'
    '.echo ===== dokan3.sys ====='
    'lmvm dokan3'
    '.echo ===== Running threads (all processors) ====='
    '!running -it'
    '.echo ===== Contended locks ====='
    '!locks'
    '.echo ===== Threads with dokan3 frames ====='
    '!stacks 2 dokan'
    '.echo ===== Driver Verifier ====='
    '!verifier 0x1'
    'q'
)
# Commands go through a -cf file: inline -c strings are cut at the first '.echo', swallowing the final 'q'.
$commandFile = Join-Path $tempRoot 'kd_commands.txt'
[System.IO.File]::WriteAllText($commandFile, ($kdCommands -join "`r`n") + "`r`n")

$failed = $false
foreach ($archive in $archives) {
    $label = Split-Path -Leaf (Split-Path -Parent $archive.FullName)
    Write-Host "=== Analyzing $label ($([Math]::Round($archive.Length / 1MB, 1)) MB) ===" -ForegroundColor Yellow
    $extractDir = Join-Path $tempRoot "dump-$label"
    Remove-Item -LiteralPath $extractDir -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $extractDir | Out-Null

    try {
        $prevEap = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            $extractOutput = @(& $sevenZip e -y "-o$extractDir" -- $archive.FullName MEMORY.DMP 2>&1 |
                ForEach-Object { "$_" })
            $extractExitCode = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $prevEap
        }
        if ($extractExitCode -ne 0) {
            Write-Host ($extractOutput | Select-Object -Last 20 | Out-String)
            Write-Host "::error::7z failed to extract MEMORY.DMP from $($archive.FullName) (exit code $extractExitCode)"
            $failed = $true
            continue
        }

        $dump = Join-Path $extractDir 'MEMORY.DMP'
        if (-not (Test-Path -LiteralPath $dump -PathType Leaf)) {
            Write-Host "::error::MEMORY.DMP not found after extracting $($archive.FullName)"
            $failed = $true
            continue
        }
        $dumpItem = Get-Item -LiteralPath $dump -Force
        if ($dumpItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) {
            Write-Host "::error::Refusing to analyze reparse point '$dump' extracted from $($archive.FullName)"
            $failed = $true
            continue
        }

        $prevEap = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            $report = @(& $kd -lines -z $dump -y $symbolPath -i $SymbolsDir -cf $commandFile 2>&1 |
                ForEach-Object { "$_" } |
                Where-Object { $_ -notmatch 'not present in the dump file' })
        } finally {
            $ErrorActionPreference = $prevEap
        }
    } finally {
        Remove-Item -LiteralPath $extractDir -Recurse -Force -ErrorAction SilentlyContinue
    }

    $reportText = $report -join "`n"
    $reportFile = Join-Path $OutputDir "$label.txt"
    [System.IO.File]::WriteAllText($reportFile, $reportText)

    if ($reportText -notmatch 'BUGCHECK_CODE') {
        Write-Host ($report | Select-Object -Last 40 | Out-String)
        Write-Host "::error::kd.exe did not produce a bugcheck analysis for $label"
        $failed = $true
        continue
    }

    $safeLabel = $label -replace '`', "'"
    $summary = @("## Crash dump: ``$safeLabel``", '')
    $fields = @{}
    foreach ($key in @('BUGCHECK_CODE', 'BUGCHECK_P1', 'BUGCHECK_P2', 'BUGCHECK_P3', 'BUGCHECK_P4', 'PROCESS_NAME', 'MODULE_NAME', 'IMAGE_NAME', 'FAILURE_BUCKET_ID')) {
        $match = [regex]::Match($reportText, "(?m)^${key}:\s*(.+)$")
        if ($match.Success) {
            $val = $match.Groups[1].Value.Trim()
            $fields[$key] = $val
            $safeVal = $val -replace '`', "'"
            $summary += "- **${key}**: ``$safeVal``"
        }
    }

    # Keep the step summary well under GitHub's 1 MiB limit; the full report is in the artifact.
    $excerptLines = Get-SummaryExcerpt -ReportLines $report -MaxAnalyzeLines 160 -MaxTotalLines 500
    $summary += '', '<details><summary>kd.exe report (excerpt; full report in crash-analysis artifact)</summary>', '', '~~~~text'
    $summary += $excerptLines
    $summary += '~~~~', '</details>', ''
    if ($env:GITHUB_STEP_SUMMARY) {
        Add-Content -Path $env:GITHUB_STEP_SUMMARY -Value ($summary -join "`n")
    }
    $report | Select-String -Pattern '^(BUGCHECK_CODE|MODULE_NAME|IMAGE_NAME|FAILURE_BUCKET_ID):' | ForEach-Object { Write-Host $_.Line }

    # Best-effort check on tolerated Server 2025 shard1 dumps: fail if dokan3 is directly named
    # (note: asynchronous PatchGuard 0x109 usually blames nt/ntkrnlmp.exe, so inspect the summary too).
    if ($label -match '^crash-dump-server2025-shard1' -and (
        ($fields['MODULE_NAME'] -match 'dokan3') -or ($fields['IMAGE_NAME'] -match 'dokan3')
    )) {
        Write-Host "::error::Crash dump '$label' implicates dokan3 (MODULE_NAME='$($fields['MODULE_NAME'])', IMAGE_NAME='$($fields['IMAGE_NAME'])'); failing analysis."
        $failed = $true
    }
}

if ($failed) {
    exit 1
}
