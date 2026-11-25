@echo off
rem Runs provision_guest.ps1 from the seed CD, mirrors its output to COM1 (QEMU serial.log), then powers off.
echo [run_provisioning.cmd] Starting provision_guest.ps1 from %1 > COM1:
powershell.exe -NoProfile -ExecutionPolicy Bypass -File %1\provision_guest.ps1 %1 > COM1: 2>&1
echo [run_provisioning.cmd] provision_guest.ps1 exited with code %ERRORLEVEL% > COM1:
shutdown.exe /s /t 2
