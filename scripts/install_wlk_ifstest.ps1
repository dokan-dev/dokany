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
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
# TODO: I have not managed to change the installation path
# $installation_dir = ''
$log_file = "ifstest_install.log"
$temp_dir = '.\ifstest_installer'
if (Get-Item env:WLK_INST_CACHE -ErrorAction SilentlyContinue) {
    $temp_dir = $env:WLK_INST_CACHE
}

$os_version = [Environment]::OSVersion.Version

$hlks = @(
    @{
        # This checks for Windows 10 "Anniversary" 1607
        # See https://technet.microsoft.com/en-us/windows/release-info.aspx
        'suitable_for_os_version' = $os_version -ge (New-Object 'Version' 10, 0, 14393, 0);
        'name' = 'Windows HLK for Windows 10, version 1607';
        'base_url' = 'https://download.microsoft.com/download/7/A/F/7AFE783C-59E6-49F9-80B4-D2F49917FFE6/hlk/Installers';
        'installer_file' = 'HLK Filter.Driver Content-x86_en-us.msi';
        'nttest_path' = "C:\Program Files (x86)\Windows Kits\10\Hardware Lab Kit\Tests\${env:PROCESSOR_ARCHITECTURE}\NTTEST";
        'files' = @{
            'HLK Filter.Driver Content-x86_en-us.msi' = '10DD59CA8B47320C685EA6FBEB64BC4548AFCCE3D7CF7E143CEA68618A679D62';
            '4c5579196433c53cc1ec3d7b40ae5fd2.cab' = '233ED34266101E2D88BB3C6EA032DC6321B83F39A7EDBB8356DF3104B241CCCF';
            '6119459287e24c3503279ff684647c83.cab' = '32CD817A442181325F513DE3D30FAE62D2AFE4A3136CDD3BC57EA365AFE54C69';
            'e54a669f7bfb1c6c6ee7bba08b02a6dc.cab' = 'FFABDD814B114457A084B80BEAC4500B2C64AD7F55007495D9551EA53CE18485';
            'fd0d8d2173424e55667bc3e935e1e376.cab' = 'ADBC46F9064B5DFCC94681B1210ACDCA255646DD434EF3AFDF3FD9BFB303BFA4';
        };
    },
    @{
        # This checks for Windows 10 1809
        'suitable_for_os_version' = $os_version -ge (New-Object 'Version' 10, 0, 17763, 0);
        'name' = 'Windows HLK for Windows 10, version 1809';
        'base_url' = 'https://download.microsoft.com/download/9/D/E/9DE48D44-C2D0-4706-8EBC-552F1A47BB06/hlk/Installers';
        'installer_file' = 'HLK Filter.Driver Content-x86_en-us.msi';
        'nttest_path' = "C:\Program Files (x86)\Windows Kits\10\Hardware Lab Kit\Tests\${env:PROCESSOR_ARCHITECTURE}\NTTEST";
        'files' = @{
            'HLK Filter.Driver Content-x86_en-us.msi' = 'ef7a7b8fc5aa4baced9bd8bcbbd0685198f2e761d1a0746a1de51df0d3baee0f';
            '4c5579196433c53cc1ec3d7b40ae5fd2.cab' = 'd5f69ea11e628c0fb51a6346684780b9add04078cccc7f7b343db66af86d715e';
            '6119459287e24c3503279ff684647c83.cab' = 'c60b003189d6a5516124cc9775c8ab58cc3b3c4952c4641863afaab0e2ad142f';
            'e54a669f7bfb1c6c6ee7bba08b02a6dc.cab' = '79b8897faba62df4aae1b2d5a9bc87da47624f3eb19ed44de8be8960f179fb9b';
            'fd0d8d2173424e55667bc3e935e1e376.cab' = '162df11cae21c22fd4e437e4598fb1b445bf550572c015c60dc2ac9e5677a0cb';
        };
    }
)

$hlk_info = $hlks | Where-Object {$_['suitable_for_os_version']} | Select-Object -Last 1
if ($null -eq $hlk_info) {
    throw "Unsupported Windows version: $os_version. No matching Windows HLK entry found."
}
Write-Host "Using $($hlk_info['name'])"
$base_url = $hlk_info['base_url']
$installer_file = $hlk_info['installer_file']
$dl_files = $hlk_info['files']
$nttest_path = $hlk_info['nttest_path']

New-Item -Type Directory -Force $temp_dir | Out-Null
foreach ($kv in $dl_files.GetEnumerator()) {
    $dl_filename = $kv.Name
    $expected_sha256 = $kv.Value
    $out_file = Join-Path $temp_dir $dl_filename
    $url = "$($base_url.TrimEnd('/'))/$([System.Uri]::EscapeDataString($dl_filename))"
    if ( !(Test-Path $out_file) -Or $(Get-FileHash -Algorithm SHA256 $out_file).Hash -ne $expected_sha256) {
        Write-Output "File $out_file not existing or hash not matching, downloading from $url..."
        $maxAttempts = 4
        $downloaded = $false
        for ($attempt = 1; $attempt -le $maxAttempts; $attempt++) {
            try {
                Remove-Item -Force -ErrorAction SilentlyContinue $out_file
                Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $out_file
                $actual_sha256 = $(Get-FileHash -Algorithm SHA256 $out_file).Hash
                if ($expected_sha256 -ne $actual_sha256) {
                    throw "Hash mismatch: $out_file, expected: $expected_sha256, actual: $actual_sha256"
                }
                $downloaded = $true
                break
            } catch {
                Write-Warning "Attempt $attempt/$maxAttempts failed for ${dl_filename}: $($_.Exception.Message)"
                if ($attempt -eq $maxAttempts) {
                    throw
                }
                Start-Sleep -Seconds (5 * [Math]::Pow(2, $attempt - 1))
            }
        }
        if (!$downloaded) {
            throw "Failed to download $dl_filename"
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
