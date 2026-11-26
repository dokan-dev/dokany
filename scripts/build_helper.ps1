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
