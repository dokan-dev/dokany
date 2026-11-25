<#
.SYNOPSIS
    One-time provisioning of the Windows Server Core CI image (runs inside the guest during bake_image.sh).

.DESCRIPTION
    Invoked by run_provisioning.cmd from autounattend.xml FirstLogonCommands with the seed CD drive letter.
    Leaves a guest that boots straight into a state where test-signed dokan3.sys loads under Driver Verifier:
    test signing on, Dokan test CA trusted, Driver Verifier armed for dokan3.sys, kernel memory dumps
    (including NMI-triggered dumps) enabled, OpenSSH server with key-only access, interactive Session 1
    AutoLogon for Administrator (so IFSTest multi-process security tests work), and HLK IFSTest installed.
    Output goes to COM1 (captured by QEMU as serial.log); the host looks for PROVISIONING_SUCCEEDED.
#>
param([string]$SeedDrive = 'E:')
$ErrorActionPreference = 'Stop'

$logFile = 'C:\ci-provisioning.log'
function Write-ProvisionLog {
    param([string]$Message)
    $line = "[$(Get-Date -Format 'HH:mm:ss')] $Message"
    Add-Content -Path $logFile -Value $line
    Write-Output $line
}

function Invoke-NativeCapture {
    param([Parameter(Mandatory = $true)][scriptblock]$Command)
    # In PowerShell 5.1, redirecting native stderr with 2>&1 throws a terminating ErrorRecord
    # whenever $ErrorActionPreference is 'Stop'. Temporarily switch to 'Continue' and stringify.
    $prevPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = (& $Command 2>&1 | ForEach-Object { "$_" }) -join "`n"
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $prevPreference
    }
    return @{ Output = $output.Trim(); ExitCode = $code }
}

try {
    $cv = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
    Write-ProvisionLog "Guest booted: $($cv.ProductName) build $($cv.CurrentBuildNumber).$($cv.UBR)"

    # 1. Disable Windows Update & Defender real-time overhead for deterministic CI
    Write-ProvisionLog 'Disabling Windows Update and adding Defender exclusions...'
    Stop-Service wuauserv -Force -ErrorAction SilentlyContinue
    Set-Service wuauserv -StartupType Disabled -ErrorAction SilentlyContinue
    if (Get-Command Set-MpPreference -ErrorAction SilentlyContinue) {
        try {
            Set-MpPreference -DisableRealtimeMonitoring $true -ErrorAction SilentlyContinue
            foreach ($excl in @('C:\ci', 'C:\FSTMP', 'C:\FSTMP2', 'C:\DokanMount', 'C:\fstest_ntfs', 'M:\')) {
                Add-MpPreference -ExclusionPath $excl -ErrorAction SilentlyContinue
            }
        } catch {
            Write-ProvisionLog "Defender configuration skipped: $($_.Exception.Message)"
        }
    }

    # Copy seed files to local writable temp dir before using them
    $localSeed = 'C:\Windows\Temp\seed'
    New-Item -ItemType Directory -Force -Path $localSeed | Out-Null
    Copy-Item -Path "$SeedDrive\*" -Destination $localSeed -Recurse -Force

    # 2. Import Dokan test CA certificate (certutil works reliably on headless Server Core)
    Write-ProvisionLog 'Importing DokanCA.cer...'
    & certutil.exe -addstore -f Root "$localSeed\DokanCA.cer" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "certutil Root failed: $LASTEXITCODE" }
    & certutil.exe -addstore -f TrustedPublisher "$localSeed\DokanCA.cer" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "certutil TrustedPublisher failed: $LASTEXITCODE" }

    # 3. Enable TESTSIGNING and pre-arm Driver Verifier for dokan3.sys
    Write-ProvisionLog 'Enabling TESTSIGNING and Driver Verifier for dokan3.sys...'
    & bcdedit.exe /set testsigning on | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "bcdedit testsigning failed: $LASTEXITCODE" }
    $vRes = Invoke-NativeCapture { & verifier.exe /standard /driver dokan3.sys }
    # verifier.exe exit code 2 = EXIT_CODE_REBOOT_REQUIRED (success; changes take effect on next boot)
    Write-ProvisionLog "verifier exit=$($vRes.ExitCode): $($vRes.Output)"
    if ($vRes.ExitCode -ne 0 -and $vRes.ExitCode -ne 2) { throw "verifier config failed: $($vRes.ExitCode)" }

    # 4. Configure Kernel Memory Dump (CrashDumpEnabled=2 -> C:\Windows\MEMORY.DMP) + AutoReboot
    Write-ProvisionLog 'Configuring Kernel Memory Dump (MEMORY.DMP)...'
    $cc = 'HKLM:\SYSTEM\CurrentControlSet\Control\CrashControl'
    Set-ItemProperty -Path $cc -Name 'CrashDumpEnabled' -Type DWord -Value 2
    Set-ItemProperty -Path $cc -Name 'DumpFile' -Type ExpandString -Value '%SystemRoot%\MEMORY.DMP'
    Set-ItemProperty -Path $cc -Name 'Overwrite' -Type DWord -Value 1
    Set-ItemProperty -Path $cc -Name 'AutoReboot' -Type DWord -Value 1
    # Let the CI host turn guest hangs into a kernel dump via QEMU 'nmi' (BugCheck 0x80 NMI_HARDWARE_FAILURE)
    Set-ItemProperty -Path $cc -Name 'NMICrashDump' -Type DWord -Value 1
    # Ensure pagefile is large enough on C: for kernel memory dump, and zero it on bake shutdown so qemu-img drops it
    $mmKey = 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management'
    Set-ItemProperty -Path $mmKey -Name 'PagingFiles' -Type MultiString -Value @('C:\pagefile.sys 2048 4096')
    Set-ItemProperty -Path $mmKey -Name 'ClearPageFileAtShutdown' -Type DWord -Value 1

    # 5. Keep interactive Session 1 AutoLogon enabled on subsequent boots so run_guest_tests.ps1 can
    #    launch tests in WinSta0\Default (required for IFSTest CreateProcessWithLogonW Security tests).
    #    RDP is disabled in autounattend.xml, SSH disables password auth below, and test VMs use restrict=on.
    Write-ProvisionLog 'Enabling console AutoAdminLogon for interactive Session 1...'
    $winlogon = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon'
    Set-ItemProperty -Path $winlogon -Name 'AutoAdminLogon' -Type String -Value '1'
    Set-ItemProperty -Path $winlogon -Name 'DefaultUserName' -Type String -Value 'Administrator'
    Set-ItemProperty -Path $winlogon -Name 'DefaultDomainName' -Type String -Value $env:COMPUTERNAME
    Set-ItemProperty -Path $winlogon -Name 'DefaultPassword' -Type String -Value 'DokanCI!Pass123'
    Remove-ItemProperty -Path $winlogon -Name 'AutoLogonCount' -ErrorAction SilentlyContinue

    # 6. Install Win32-OpenSSH from local seed copy
    Write-ProvisionLog 'Installing Win32-OpenSSH...'
    $sshDest = 'C:\Program Files\OpenSSH'
    New-Item -ItemType Directory -Force -Path $sshDest | Out-Null
    Expand-Archive -Path "$localSeed\OpenSSH-Win64.zip" -DestinationPath 'C:\Windows\Temp\openssh-extract' -Force
    $inner = Get-ChildItem 'C:\Windows\Temp\openssh-extract' -Directory | Select-Object -First 1
    if ($inner) {
        Copy-Item -Path "$($inner.FullName)\*" -Destination $sshDest -Recurse -Force
    } else {
        Copy-Item -Path 'C:\Windows\Temp\openssh-extract\*' -Destination $sshDest -Recurse -Force
    }
    Remove-Item 'C:\Windows\Temp\openssh-extract' -Recurse -Force
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$sshDest\install-sshd.ps1"
    if ($LASTEXITCODE -ne 0) { throw "install-sshd.ps1 failed: $LASTEXITCODE" }
    Set-Service sshd -StartupType Automatic
    Start-Service sshd

    # Wait briefly for sshd to generate C:\ProgramData\ssh\sshd_config on first start.
    $sshData = 'C:\ProgramData\ssh'
    $sshdCfg = "$sshData\sshd_config"
    for ($i = 0; $i -lt 15; $i++) {
        if (Test-Path -LiteralPath $sshdCfg -PathType Leaf) { break }
        Start-Sleep -Seconds 1
    }
    if (-not (Test-Path -LiteralPath $sshdCfg -PathType Leaf)) {
        throw "sshd_config not created at $sshdCfg"
    }

    # Harden sshd_config: disable UseDNS (so restrict=on NAT never stalls banner exchange)
    # and enforce key-only access for Administrator (never accept the public autounattend password over SSH).
    $cfgText = Get-Content -LiteralPath $sshdCfg -Raw
    $directives = @(
        'UseDNS no'
        'PasswordAuthentication no'
        'KbdInteractiveAuthentication no'
        'PubkeyAuthentication yes'
        'AllowUsers Administrator'
    )
    $header = ($directives -join "`r`n") + "`r`n"
    [System.IO.File]::WriteAllText($sshdCfg, $header + $cfgText, (New-Object System.Text.UTF8Encoding($false)))

    # Configure authorized_keys for Administrators (well-known SIDs: BUILTIN\Administrators, NT AUTHORITY\SYSTEM)
    $authKeys = "$sshData\administrators_authorized_keys"
    Copy-Item -LiteralPath "$localSeed\authorized_keys" -Destination $authKeys -Force
    & icacls.exe $authKeys /inheritance:r /grant '*S-1-5-32-544:F' /grant '*S-1-5-18:F' | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "icacls on $authKeys failed: $LASTEXITCODE" }

    # Set PowerShell as default SSH shell and open firewall port 22 on ALL profiles.
    # Use a dedicated rule name so we do not collide with any pre-existing OpenSSH rule scoped only to Domain/Private.
    New-ItemProperty -Path 'HKLM:\SOFTWARE\OpenSSH' -Name 'DefaultShell' -Value 'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe' -PropertyType String -Force | Out-Null
    & netsh.exe advfirewall firewall add rule name='DokanCI-SSH-In' dir=in action=allow protocol=TCP localport=22 profile=any
    if ($LASTEXITCODE -ne 0) { throw "netsh advfirewall add rule failed: $LASTEXITCODE" }

    Restart-Service sshd
    Start-Sleep -Seconds 3
    $sshdSvc = Get-Service sshd
    Write-ProvisionLog "sshd service status: $($sshdSvc.Status)"
    if ($sshdSvc.Status -ne 'Running') { throw "sshd is not running: $($sshdSvc.Status)" }
    $listening = & netstat.exe -ano | Select-String ':22\s'
    Write-ProvisionLog "netstat port 22: $($listening -join ' | ')"
    if (-not $listening) { throw 'sshd is not listening on port 22!' }

    # 7. Create dokan_ifstest user, disable 42-day password expiry (cached images outlive 42 days), & install HLK IFSTest
    Write-ProvisionLog 'Creating dokan_ifstest user and disabling password expiration...'
    & net.exe user dokan_ifstest D0kan_1fstest /ADD
    if ($LASTEXITCODE -ne 0) { throw "net user failed: $LASTEXITCODE" }
    & net.exe accounts /maxpwage:unlimited
    if ($LASTEXITCODE -ne 0) { throw "net accounts /maxpwage:unlimited failed: $LASTEXITCODE" }

    Write-ProvisionLog 'Installing WLK/HLK IFSTest...'
    $env:WLK_INST_CACHE = 'C:\Windows\Temp\wlk_inst'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$localSeed\install_wlk_ifstest.ps1"
    if ($LASTEXITCODE -ne 0) { throw "install_wlk_ifstest.ps1 failed: $LASTEXITCODE" }
    Remove-Item -Path 'C:\Windows\Temp\wlk_inst' -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -Path $localSeed -Recurse -Force -ErrorAction SilentlyContinue

    # 8. Shrink OS component store and TRIM free blocks for sparse hole-punching before qemu-img convert
    Write-ProvisionLog 'Running DISM StartComponentCleanup /ResetBase...'
    $tDism = Get-Date
    $dismRes = Invoke-NativeCapture { & dism.exe /Online /Cleanup-Image /StartComponentCleanup /ResetBase }
    Write-ProvisionLog "DISM exit=$($dismRes.ExitCode) in $([int]((Get-Date) - $tDism).TotalSeconds)s"
    if ($dismRes.ExitCode -ne 0) {
        Write-ProvisionLog "DISM warning output: $($dismRes.Output)"
    }

    Write-ProvisionLog 'Cleaning temp, update cache, and log files...'
    foreach ($cleanDir in @(
        'C:\Windows\Temp\*',
        'C:\Windows\SoftwareDistribution\Download\*',
        'C:\Windows\Logs\CBS\*',
        'C:\Windows\Logs\DISM\*',
        'C:\Windows\Panther\UnattendGC\*'
    )) {
        Remove-Item -Path $cleanDir -Recurse -Force -ErrorAction SilentlyContinue
    }
    # Disable hibernation to eliminate hiberfil.sys
    & powercfg.exe /h off | Out-Null

    # Reclaim deleted blocks directly into qcow2 sparse holes via SATA TRIM (QEMU ide-hd uses
    # discard=unmap + rotation_rate=1). Avoids writing a multi-GB zero file onto the host disk.
    Write-ProvisionLog 'Running Optimize-Volume -DriveLetter C -ReTrim...'
    $tTrim = Get-Date
    try {
        $trimOut = (Optimize-Volume -DriveLetter C -ReTrim -Verbose 4>&1 | ForEach-Object { "$_" }) -join '; '
        Write-ProvisionLog "ReTrim completed in $([int]((Get-Date) - $tTrim).TotalSeconds)s: $trimOut"
    } catch {
        Write-ProvisionLog "WARNING: Optimize-Volume -ReTrim failed: $($_.Exception.Message)"
    }

    Write-ProvisionLog 'PROVISIONING_SUCCEEDED'
    exit 0
} catch {
    Write-ProvisionLog "PROVISIONING_FAILED: $($_.Exception.Message)"
    Write-ProvisionLog ($_.ScriptStackTrace | Out-String)
    exit 1
}
