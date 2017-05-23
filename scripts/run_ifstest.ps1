Set-StrictMode -Version 2
#TODO: Get Path via some MSI-CmdLet or Registry instead of hardcoding it
if ([Environment]::OSVersion.Version.Major -eq 10) {
    $hlk_path = "C:\Program Files (x86)\Windows Kits\10\Hardware Lab Kit"
}
else {
    $hlk_path = "C:\Program Files (x86)\Windows Kits\8.1\Hardware Certification Kit"
}

$nttest_path = "${hlk_path}\Tests\${env:PROCESSOR_ARCHITECTURE}\NTTEST"
$ifstest_exe = "${nttest_path}\BASETEST\core_file_services\ifs_test_kit\ifstest.exe"

if (!(Test-Path $ifstest_exe)) {
    throw "$ifstest_exe not found!"
}

& $ifstest_exe $args

if ($LASTEXITCODE -ne 0) {
   Write-Error "Non-zero exit-code: $LASTEXITCODE"
   Exit $LASTEXITCODE
}

