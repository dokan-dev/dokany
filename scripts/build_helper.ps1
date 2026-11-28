# Runs a native executable (or a script that sets $ErrorActionPreference = 'Stop')
# while temporarily using 'Continue' so PS 5.1 does not abort on exit-0 native stderr.
# Throws on non-zero $LASTEXITCODE or on a non-native trailing PowerShell error.
function Invoke-DokanExternal {
    [CmdletBinding()]
    param(
        [Parameter(Position = 0, Mandatory = $true)]
        [scriptblock]$Command
    )

    $global:LASTEXITCODE = 0
    $prevTopError = if ($global:Error.Count -gt 0) { $global:Error[0] } else { $null }
    $prevPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $Command
        $commandOk = $?
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $prevPreference
    }
    if ($exitCode -and $exitCode -ne 0) {
        throw "Command returned non-zero exit code ${exitCode}: $Command"
    }
    if (-not $commandOk -and $global:Error.Count -gt 0 -and -not [object]::ReferenceEquals($global:Error[0], $prevTopError)) {
        $lastErr = $global:Error[0]
        if ($lastErr.FullyQualifiedErrorId -notlike 'NativeCommandError*') {
            throw $lastErr
        }
    }
}

function Exec-External {
    param(
        [Parameter(Position = 0, Mandatory = $true)]
        [scriptblock]$command
    )

    Invoke-DokanExternal -Command $command
}

function Get-DokanVsWherePath {
    [CmdletBinding()]
    [OutputType([string])]
    param(
        [switch]$RequireExists
    )

    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere -PathType Leaf) {
        return $vswhere
    }
    if ($RequireExists) {
        throw "Visual Studio Installer (vswhere.exe) not found at $vswhere"
    }
    return $null
}

function Find-DokanVisualStudio {
    [CmdletBinding()]
    [OutputType([hashtable[]])]
    param(
        [string[]]$Requires = @(),
        [switch]$Latest,
        [switch]$IncludeVersion,
        [switch]$RequireVsWhere
    )

    $vswhere = Get-DokanVsWherePath -RequireExists:$RequireVsWhere
    if (-not $vswhere) {
        return @()
    }

    $vswhereArgs = @()
    if ($Latest) {
        $vswhereArgs += '-latest'
    } else {
        $vswhereArgs += '-sort'
    }
    $vswhereArgs += @('-products', '*')
    foreach ($component in $Requires) {
        if (-not [string]::IsNullOrWhiteSpace($component)) {
            $vswhereArgs += @('-requires', $component)
        }
    }
    $vswhereArgs += @('-property', 'installationPath')

    $prevPreference = $ErrorActionPreference
    $prevExitCode = $global:LASTEXITCODE
    $ErrorActionPreference = 'Continue'
    try {
        $global:LASTEXITCODE = 0
        $paths = @(& $vswhere @vswhereArgs 2>$null | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
        if ($LASTEXITCODE -ne 0) {
            return @()
        }
        if ($Latest) {
            $paths = @($paths | Select-Object -First 1)
        }
        $results = @()
        foreach ($vsPath in $paths) {
            if (-not (Test-Path -LiteralPath $vsPath -PathType Container)) {
                continue
            }
            $version = ''
            if ($IncludeVersion) {
                $global:LASTEXITCODE = 0
                $verLines = @(& $vswhere -path $vsPath -property installationVersion 2>$null)
                if ($LASTEXITCODE -eq 0) {
                    $version = [string]($verLines | Select-Object -First 1)
                }
            }
            $results += @{
                Path    = $vsPath
                Version = $version
            }
        }
    } finally {
        $global:LASTEXITCODE = $prevExitCode
        $ErrorActionPreference = $prevPreference
    }
    return $results
}

function Add-VisualStudio-Path {
    $vs = Find-DokanVisualStudio -Requires 'Microsoft.Component.MSBuild' -Latest -RequireVsWhere | Select-Object -First 1
    $vsPath = if ($vs) { $vs.Path } else { '' }
    $msBuild_VSPath = if ($vsPath) { Join-Path -Path $vsPath -ChildPath 'MSBuild\Current\Bin\amd64' } else { '' }
    if ($vsPath -and -not (Test-Path -LiteralPath $msBuild_VSPath -PathType Container)) {
        $msBuild_VSPath = Join-Path -Path $vsPath -ChildPath 'MSBuild\Current\Bin'
    }

    if ([string]::IsNullOrEmpty($vsPath) -or -not (Test-Path -LiteralPath $msBuild_VSPath -PathType Container)) {
        throw 'Visual Studio with MSBuild NOT Installed.'
    }

    $env:Path = "$msBuild_VSPath;$env:Path"
}

function Resolve-DokanTargetPath {
    [CmdletBinding()]
    [OutputType([string])]
    param(
        [Parameter(Mandatory = $true)][string]$BasePath,
        [Parameter(Mandatory = $true)][string]$Path
    )

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return $Path
    }
    return (Join-Path -Path $BasePath -ChildPath $Path)
}

function Invoke-DokanWithRetry {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Description,
        [Parameter(Mandatory = $true)][scriptblock]$Action,
        [int]$MaxAttempts = 3,
        [int]$DelaySeconds = 10
    )

    for ($attempt = 1; $attempt -le $MaxAttempts; $attempt++) {
        try {
            & $Action
            return
        } catch {
            if ($attempt -ge $MaxAttempts) {
                throw
            }
            Write-Warning "$Description failed (attempt $attempt/$MaxAttempts): $($_.Exception.Message). Retrying in ${DelaySeconds}s..."
            Start-Sleep -Seconds $DelaySeconds
        }
    }
}

function Get-DokanExcludedUnixPrefixes {
    $cygwinDir = if (-not [string]::IsNullOrWhiteSpace($env:CYGWIN_INST_DIR)) { $env:CYGWIN_INST_DIR } else { 'C:\cygwin64' }
    $msys2Dir = if (-not [string]::IsNullOrWhiteSpace($env:MSYS2_INST_DIR)) { $env:MSYS2_INST_DIR } else { 'C:\msys64' }
    return @(
        ([System.IO.Path]::GetFullPath($cygwinDir).TrimEnd('\') + '\'),
        ([System.IO.Path]::GetFullPath($msys2Dir).TrimEnd('\') + '\')
    )
}

function Test-DokanExcludedPath {
    param(
        [Parameter(Mandatory = $true)][string]$FullPath,
        [Parameter(Mandatory = $true)][string[]]$ExcludedPrefixes
    )

    foreach ($prefix in $ExcludedPrefixes) {
        if ($FullPath.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
            return $true
        }
    }
    return $false
}

function Resolve-DokanVisualStudioInfo {
    $vs = Find-DokanVisualStudio -Requires 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64' -Latest -IncludeVersion -RequireVsWhere | Select-Object -First 1
    if (-not $vs) {
        $vs = Find-DokanVisualStudio -Requires 'Microsoft.Component.MSBuild' -Latest -IncludeVersion -RequireVsWhere | Select-Object -First 1
    }
    if (-not $vs) {
        throw 'Visual Studio installation with C++ tools not found via vswhere.exe.'
    }
    return $vs
}

function Resolve-DokanWindowsTool {
    [CmdletBinding()]
    [OutputType([string])]
    param(
        [Parameter(Mandatory = $true)][string]$ExeName,
        [string[]]$Candidates = @(),
        [switch]$Required
    )

    $excludedPrefixes = Get-DokanExcludedUnixPrefixes
    foreach ($cmd in @(Get-Command $ExeName -All -CommandType Application -ErrorAction SilentlyContinue)) {
        if (-not $cmd.Source) { continue }
        $fullSource = [System.IO.Path]::GetFullPath($cmd.Source)
        if (-not (Test-DokanExcludedPath -FullPath $fullSource -ExcludedPrefixes $excludedPrefixes)) {
            return $fullSource
        }
    }

    foreach ($candidate in ($Candidates | Select-Object -Unique)) {
        if (-not [string]::IsNullOrWhiteSpace($candidate) -and (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            return [System.IO.Path]::GetFullPath($candidate)
        }
    }
    if ($Required) {
        throw "Native Windows $ExeName not found (Cygwin and MSYS2 tools are excluded)."
    }
    return $null
}

function Resolve-DokanVisualStudioGenerator {
    [CmdletBinding()]
    [OutputType([string])]
    param(
        [Parameter(Mandatory = $true)][string]$CMakeExe,
        [string]$VsVersion = ''
    )

    $majorVersion = 0
    if (-not [string]::IsNullOrWhiteSpace($VsVersion) -and ($VsVersion -match '^(\d+)')) {
        $majorVersion = [int]$Matches[1]
    }
    $knownGenerators = @{
        18 = 'Visual Studio 18 2026'
        17 = 'Visual Studio 17 2022'
        16 = 'Visual Studio 16 2019'
    }

    $helpText = (@(& $CMakeExe --help 2>&1) -join "`n")
    if ($knownGenerators.ContainsKey($majorVersion)) {
        $preferred = $knownGenerators[$majorVersion]
        if ($helpText -match [regex]::Escape($preferred)) {
            return $preferred
        }
        throw "CMake at $CMakeExe does not support generator '$preferred' and ninja.exe was not found. Install Ninja or upgrade CMake."
    }

    foreach ($ver in @(18, 17, 16)) {
        $candidate = $knownGenerators[$ver]
        if ($helpText -match [regex]::Escape($candidate)) {
            return $candidate
        }
    }
    throw "No supported Visual Studio generator found in $CMakeExe --help and ninja.exe was not found."
}

function Invoke-DokanWithVsDevEnv {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$VsPath,
        [string]$Arch = 'amd64',
        [Parameter(Mandatory = $true)][scriptblock]$Action
    )

    $vsDevCmd = Join-Path $VsPath 'Common7\Tools\VsDevCmd.bat'
    if (-not (Test-Path -LiteralPath $vsDevCmd -PathType Leaf)) {
        throw "VsDevCmd.bat not found at $vsDevCmd"
    }

    $savedEnv = @{}
    foreach ($entry in [System.Environment]::GetEnvironmentVariables('Process').GetEnumerator()) {
        $savedEnv[$entry.Key] = $entry.Value
    }

    $sentinel = '===DOKAN_VSDEV_ENV_START==='
    $tempCmd = Join-Path ([System.IO.Path]::GetTempPath()) ("dokan_vsdev_" + [guid]::NewGuid().ToString('N') + '.cmd')
    try {
        $cmdContent = @(
            '@echo off'
            'set VSCMD_SKIP_SENDTELEMETRY=1'
            "call `"$vsDevCmd`" -arch=$Arch -host_arch=amd64 -no_logo"
            'if errorlevel 1 exit /b %errorlevel%'
            "echo $sentinel"
            'set'
        ) -join "`r`n"
        $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
        [System.IO.File]::WriteAllText($tempCmd, $cmdContent + "`r`n", $utf8NoBom)

        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = if ($env:ComSpec) { $env:ComSpec } else { 'cmd.exe' }
        $psi.Arguments = "/d /c `"$tempCmd`""
        $psi.UseShellExecute = $false
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true
        $psi.CreateNoWindow = $true

        $proc = New-Object System.Diagnostics.Process
        $proc.StartInfo = $psi
        try {
            [void]$proc.Start()
            $stderrTask = $proc.StandardError.ReadToEndAsync()
            $stdout = $proc.StandardOutput.ReadToEnd()
            $proc.WaitForExit()
            $stderr = $stderrTask.GetAwaiter().GetResult()
            $exitCode = $proc.ExitCode
        } finally {
            $proc.Dispose()
        }

        if ($exitCode -ne 0) {
            throw "VsDevCmd.bat -arch=$Arch failed (exit $exitCode). Stdout:`n$stdout`nStderr:`n$stderr"
        }

        $inEnv = $false
        $parsed = @{}
        foreach ($rawLine in ($stdout -split "`r?`n")) {
            if (-not $inEnv) {
                if ($rawLine -eq $sentinel) {
                    $inEnv = $true
                }
                continue
            }
            $eqIndex = $rawLine.IndexOf('=')
            if ($eqIndex -gt 0) {
                $parsed[$rawLine.Substring(0, $eqIndex)] = $rawLine.Substring($eqIndex + 1)
            }
        }

        if (-not $inEnv -or $parsed.Count -eq 0) {
            throw "Failed to capture environment variables from $vsDevCmd"
        }

        foreach ($kv in $parsed.GetEnumerator()) {
            [System.Environment]::SetEnvironmentVariable($kv.Key, $kv.Value, 'Process')
        }

        & $Action
    } finally {
        Remove-Item -LiteralPath $tempCmd -Force -ErrorAction SilentlyContinue
        foreach ($key in @([System.Environment]::GetEnvironmentVariables('Process').Keys)) {
            if (-not $savedEnv.ContainsKey($key)) {
                [System.Environment]::SetEnvironmentVariable($key, $null, 'Process')
            }
        }
        foreach ($kv in $savedEnv.GetEnumerator()) {
            [System.Environment]::SetEnvironmentVariable($kv.Key, $kv.Value, 'Process')
        }
    }
}

function Test-DokanStaticCrtImports {
    [CmdletBinding()]
    [OutputType([string[]])]
    param(
        [Parameter(Mandatory = $true)][string]$BinaryPath,
        [string[]]$Allowlist = @(
            'KERNEL32.dll',
            'ADVAPI32.dll',
            'ntdll.dll',
            'bcrypt.dll',
            'bcryptprimitives.dll'
        )
    )

    $dumpbinCmd = Get-Command dumpbin.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $dumpbinCmd) {
        throw "dumpbin.exe not found on PATH; cannot verify static CRT imports for $BinaryPath."
    }

    $prevPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = @(& $dumpbinCmd.Source /nologo /imports $BinaryPath 2>&1)
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $prevPreference
    }
    if ($exitCode -ne 0) {
        throw "dumpbin /imports failed for $BinaryPath with exit code ${exitCode}:`n$($output -join "`n")"
    }

    $importedDlls = New-Object System.Collections.Generic.List[string]
    $seen = @{}
    foreach ($line in $output) {
        $text = [string]$line
        if ($text -match '^\s*Summary\s*$') {
            break
        }
        if ($text -match '^\s+([A-Za-z0-9._-]+\.dll)\s*$') {
            $dll = $Matches[1]
            $key = $dll.ToLowerInvariant()
            if (-not $seen.ContainsKey($key)) {
                $seen[$key] = $true
                $importedDlls.Add($dll)
            }
        }
    }

    if ($importedDlls.Count -eq 0) {
        throw "Failed to parse any imported DLLs from dumpbin /imports output for $BinaryPath."
    }

    $denylistPatterns = @(
        'VCRUNTIME*',
        'MSVCP*',
        'ucrtbase*',
        'CONCRT*',
        'api-ms-win-crt-*',
        'dokan*'
    )

    $deniedHits = @()
    $disallowedHits = @()
    foreach ($dll in $importedDlls) {
        foreach ($pattern in $denylistPatterns) {
            if ($dll -like $pattern) {
                $deniedHits += $dll
                break
            }
        }
        if ($Allowlist -notcontains $dll) {
            $disallowedHits += $dll
        }
    }

    if ($deniedHits.Count -gt 0 -or $disallowedHits.Count -gt 0) {
        $details = @("Import portability gate failed for $BinaryPath.")
        $details += "Imported DLLs: $($importedDlls -join ', ')"
        if ($deniedHits.Count -gt 0) {
            $details += "Denied CRT/Dokan imports: $($deniedHits -join ', ')"
        }
        if ($disallowedHits.Count -gt 0) {
            $details += "Imports outside Server 2016 base-OS allowlist ($($Allowlist -join ', ')): $($disallowedHits -join ', ')"
        }
        throw ($details -join "`n")
    }

    return [string[]]$importedDlls.ToArray()
}
