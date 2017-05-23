# How to prevent downloading gigabytes of stuff (I am sure there are easier ways)
# 1. Get the URL of HLKSetup.exe
# 2. Get the MSI for the file system tests: HLK Filter.Driver Content-x86_en-us.msi
#    The files that the setup downloads are located under the directory Installers relative to HLKSetup.exe
#    For instance, the FS-tests installer for http://download.microsoft.com/download/foo/bar/HLKSetup.exe
#                 is located at http://download.microsoft.com/download/foo/bar/Installers/HLK Filter.Driver Content-x86_en-us.msi
# 3. Install lessmsi: choco install lessmsi
# 4. Try extracting the MSI-file with lessmsi. If it complains about missing cabinet files, add it to the list of
#    files to be downloaded.
#
Set-StrictMode -Version 2
# TODO: I have not managed to change the installation path
# $installation_dir = ''
$log_file = "ifstest_install.log"
$temp_dir = '.\ifstest_installer'
if (Get-Item env:WLK_INST_CACHE -ErrorAction SilentlyContinue) {
    $temp_dir = $env:WLK_INST_CACHE
}

$hlks = @(
    @{
        # Anything older than Win10 (8.1, 8, 7). Only tested with 8.1. We might need an older one for Win7
        'suitable_for_os_version' = [Environment]::OSVersion.Version -lt (New-Object 'Version' 10, 0, 0, 0);
        'name' = 'Windows HCK for Windows 8.1 QFE Update 014 (Build ID: 8.100.27024)';
        'base_url' = 'http://download.microsoft.com/download/6/6/B/66B3BE98-DD33-4860-A404-8F2F77EDF622/HCK/Installers/';
        'installer_file' = 'HCK Filter.Driver Content-x86_en-us.msi';
        'nttest_path' = "C:\Program Files (x86)\Windows Kits\8.1\Hardware Certification Kit\Tests\${env:PROCESSOR_ARCHITECTURE}\NTTEST";
        'files' = @{
            'HCK Filter.Driver Content-x86_en-us.msi' = '6E5987DE75ABE1258C775A6B6C60062F5BEE2DE7FDF38F94F1E4B23EEFF50730';
            '6119459287e24c3503279ff684647c83.cab' = 'F40471EA4F6E90A6D6B698C910E3B817BD672A288ED45C52A1DB57B734A10810';
            '9ddb34978b4f0879d46fa6380d941d00.cab' = '9FD8C0435CF17726A6D4A31475412B526653624330D9046B678155E5A73E9CD5';
            'dc7239f1f797bdd32e0262f2556b253f.cab' = 'E279B34736495772CE464F3E25C6CD49A2BA97C4AD69D2A449F175B70323D6F0';
            'e54a669f7bfb1c6c6ee7bba08b02a6dc.cab' = 'D7139ADC54303EB6A6B7F4F14CE875A307F268669FFCD54C7BFEAA75F00705D2';
            'f1e419e00f0b2f836bdd5f3d26ae111d.cab' = 'C67F07F6533B4C64E69C5E2C60850490AF92459907F8CE10A108A2AD2B2184BC';
            'fd0d8d2173424e55667bc3e935e1e376.cab' = 'FA63584DCCA98976544EF9DC5E66979B06F67A99187FEC9275599F7F258F8213';
        };
    },
    #TODO: Need to find information about whether WLK is backward/forward-compatible within Win10-releases
    @{
        # This checks for Windows 10 "Anniversary" 1607
        # See https://technet.microsoft.com/en-us/windows/release-info.aspx
        'suitable_for_os_version' = [Environment]::OSVersion.Version -ge (New-Object 'Version' 10, 0, 14393, 0);
        'name' = 'Windows HLK for Windows 10, version 1607';
        'base_url' = 'http://download.microsoft.com/download/7/A/F/7AFE783C-59E6-49F9-80B4-D2F49917FFE6/hlk/Installers/';
        'installer_file' = 'HLK Filter.Driver Content-x86_en-us.msi';
        'nttest_path' = "C:\Program Files (x86)\Windows Kits\10\Hardware Lab Kit\Tests\${env:PROCESSOR_ARCHITECTURE}\NTTEST";
        'files' = @{
            'HLK Filter.Driver Content-x86_en-us.msi' = '10DD59CA8B47320C685EA6FBEB64BC4548AFCCE3D7CF7E143CEA68618A679D62';
            '4c5579196433c53cc1ec3d7b40ae5fd2.cab' = '233ED34266101E2D88BB3C6EA032DC6321B83F39A7EDBB8356DF3104B241CCCF';
            '6119459287e24c3503279ff684647c83.cab' = '32CD817A442181325F513DE3D30FAE62D2AFE4A3136CDD3BC57EA365AFE54C69';
            'e54a669f7bfb1c6c6ee7bba08b02a6dc.cab' = 'FFABDD814B114457A084B80BEAC4500B2C64AD7F55007495D9551EA53CE18485';
            'fd0d8d2173424e55667bc3e935e1e376.cab' = 'ADBC46F9064B5DFCC94681B1210ACDCA255646DD434EF3AFDF3FD9BFB303BFA4';
        };
    }
)

$hlk_info = $hlks | Where-Object {$_['suitable_for_os_version']}
Write-Host "Using $($hlk_info['name'])"
$base_url = $hlk_info['base_url']
$installer_file = $hlk_info['installer_file']
$dl_files = $hlk_info['files']
$nttest_path = $hlk_info['nttest_path']

New-Item -Type Directory -Force $temp_dir | Out-Null
foreach ($kv in $dl_files.GetEnumerator()) {
    $dl_filename = $kv.Name
    $expected_sha256 = $kv.Value
    $out_file = "${temp_dir}/${dl_filename}"
    $url = "${base_url}/${dl_filename}"
    if ( !(Test-Path $out_file) -Or $(Get-FileHash -Algorithm SHA256 $out_file).Hash -ne $expected_sha256) {
        Write-Host "File $out_file not existing or hash not matching, downloading..."
        Invoke-WebRequest $url -OutFile $out_file
        $actual_sha256 = $(Get-FileHash -Algorithm SHA256 $out_file).Hash
        if ($expected_sha256 -ne $actual_sha256) {
            throw "Hash mismatch: $out_file, expected: $expected_sha256, actual: $actual_sha256"
        }
    }
    else {
        Write-Host "Skipping already downloaded file $out_file"
    }
}

Write-Host Installing MSI
Push-Location $temp_dir
$log_file = "./install.log"
$msi_proc = Start-Process -PassThru -Wait -FilePath msiexec.exe -ArgumentList @("/qn", "/norestart", "/lv*", $log_file, "/i", "`"$installer_file`"")
Pop-Location
if ($msi_proc.ExitCode -ne 0) {
    Write-Error "IFSTest-Installation failed. Log below this line:"
    Get-Content $log_file
    throw "IFSTest-Installation failed. Log above this line"
}

# We copy some files into the same dir as the exe.
# Setting the PATH is cumbersome, because IFSTest launches itself under a different user for some tests!
$ifstest_dir = "${nttest_path}\BASETEST\core_file_services\ifs_test_kit\"
$needed_files = @(
    "${nttest_path}\BASETEST\core_file_services\shared_libs\fbslog\FbsLog.dll",
    "${nttest_path}\commontest\ntlog\ntlogger.ini"
    "${nttest_path}\commontest\ntlog\ntlog.dll"
)
Copy-Item $needed_files $ifstest_dir

Write-Host "Installation successful."
