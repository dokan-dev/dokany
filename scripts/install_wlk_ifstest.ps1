# How to prevent downloading gigabytes of stuff (I am sure there are easier ways)
# 1. Get the URL of HLKSetup.exe
# 2. Get the MSI for the file system tests: HLK Filter.Driver Content-x86_en-us.msi
#    Starting with Windows Server 2022 (build 20348), ntlog.dll and ntlogger.ini moved to
#    HLK OnecoreUAP.Filter.Driver Content-x86_en-us.msi, so both MSIs are required.
#    The files that the setup downloads are located under the directory Installers relative to HLKSetup.exe
#    For instance, the FS-tests installer for https://download.microsoft.com/download/foo/bar/HLKSetup.exe
#                 is located at https://download.microsoft.com/download/foo/bar/Installers/HLK Filter.Driver Content-x86_en-us.msi
# 3. Install lessmsi: choco install lessmsi
# 4. Try extracting each MSI file with lessmsi. If it complains about missing cabinet files, add them to the list of
#    files to be downloaded.
#
Set-StrictMode -Version 2
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
# TODO: I have not managed to change the installation path
# $installation_dir = ''
$temp_dir = '.\ifstest_installer'
if (Get-Item env:WLK_INST_CACHE -ErrorAction SilentlyContinue) {
    $temp_dir = $env:WLK_INST_CACHE
}

$os_version = [Version](Get-CimInstance Win32_OperatingSystem).Version
$nttest_path = "C:\Program Files (x86)\Windows Kits\10\Hardware Lab Kit\Tests\${env:PROCESSOR_ARCHITECTURE}\NTTEST"

$hlks = @(
    @{
        # Covers Windows 10 1507 (build 10240) through Windows 10 1803 and Windows Server 2016 (14393)
        'suitable_for_os_version' = $os_version -ge [Version]'10.0.10240';
        'name' = 'Windows HLK for Windows Server 2016 / Windows 10, version 1607 (10.1.14393.8069)';
        'base_url' = 'https://download.microsoft.com/download/f0cf2813-b675-4446-a8f2-f5345c1648d8/hlk/Installers';
        'installer_files' = @(
            'HLK Filter.Driver Content-x86_en-us.msi'
        );
        'files' = @{
            'HLK Filter.Driver Content-x86_en-us.msi' = '4979F845A18C2339D7CAD14DFA352E396E684CA47AAEA15317FB43D54F4C4899';
            '6119459287e24c3503279ff684647c83.cab' = '87782A15E0BF581CB0BE1F0B8137FA617D09A7FFF3DAA483FA5E4A7919E8EAFF';
            'e54a669f7bfb1c6c6ee7bba08b02a6dc.cab' = '0B038B145B93CD1C90597B0D0EB6A1386B3156F5B44EDB18AF05D325AAE59D95';
        };
    },
    @{
        # Windows Server 2019 / Windows 10 1809-1909
        'suitable_for_os_version' = $os_version -ge [Version]'10.0.17763';
        'name' = 'Windows HLK for Windows Server 2019 / Windows 10, version 1809 (10.1.17763.7250)';
        'base_url' = 'https://download.microsoft.com/download/24a7be4f-469e-46dc-ad7b-a65a233187be/hlk/Installers';
        'installer_files' = @(
            'HLK Filter.Driver Content-x86_en-us.msi'
        );
        'files' = @{
            'HLK Filter.Driver Content-x86_en-us.msi' = 'A26F1ED8A0DCF56CA7FFA329617B8D997BDF3E92DCF45F49A31EEF30F40201EC';
            '6119459287e24c3503279ff684647c83.cab' = '812E30AA97A6D3707932F9AB854594F9E9AC507394D7FE50F81255C3FC916ABB';
            'e54a669f7bfb1c6c6ee7bba08b02a6dc.cab' = '88D8887BA5054FBD2502B46F06CC06687B4602FCF74C2160BBA9729BC9807515';
            'fd0d8d2173424e55667bc3e935e1e376.cab' = '8DA88FB2742BF3E6133EFF9B32B577B11D1ADFB1D54B0ADC90C3FC496B45C569';
        };
    },
    @{
        # Windows 10 2004-22H2
        'suitable_for_os_version' = $os_version -ge [Version]'10.0.19041';
        'name' = 'Windows HLK for Windows 10, versions 2004-22H2 (10.1.19041.5738)';
        'base_url' = 'https://download.microsoft.com/download/06db4bc9-e6a0-4e1c-8348-d3e4df9517cf/hlk/Installers';
        'installer_files' = @(
            'HLK Filter.Driver Content-x86_en-us.msi'
        );
        'files' = @{
            'HLK Filter.Driver Content-x86_en-us.msi' = '1FD554D265FF015625D88FCE577D9DFDC7B4D4C088FA5F65F4E067CE4C2EAED3';
            '4c5579196433c53cc1ec3d7b40ae5fd2.cab' = 'E0D863754ADAEC66722B749085E195E418247957C06B9FFD077C38EC9CBA22FA';
            '6119459287e24c3503279ff684647c83.cab' = 'DFFF8123798A0B1CAE770746925831D69D93EC0C8D831C55E4A612344133437C';
            'e54a669f7bfb1c6c6ee7bba08b02a6dc.cab' = '3233C9F5ECB60053BA5B4A38EA032B89E2F42DA8A2D92D277551FB68A9CC384E';
            'fd0d8d2173424e55667bc3e935e1e376.cab' = '57437CE133B7D92D8494F6A25E1E94F97894B3BC19D394C4D08C063B0210D97D';
        };
    },
    @{
        # Windows Server 2022
        'suitable_for_os_version' = $os_version -ge [Version]'10.0.20348';
        'name' = 'Windows HLK for Windows Server 2022 (10.1.20348.3567)';
        'base_url' = 'https://download.microsoft.com/download/20ede2e4-2368-4b6d-a05f-409b432ec553/hlk/Installers';
        'installer_files' = @(
            'HLK Filter.Driver Content-x86_en-us.msi',
            'HLK OnecoreUAP.Filter.Driver Content-x86_en-us.msi'
        );
        'files' = @{
            'HLK Filter.Driver Content-x86_en-us.msi' = '4D1A7E7BA7085484F11C08EAC35BC7C7E246B99A8A0D5F20ACC987D0EE58A9CF';
            '6119459287e24c3503279ff684647c83.cab' = 'CE197354E2ACA7E40D3A22624893A87331B7D8381C037210B67676377DA28A69';
            'e54a669f7bfb1c6c6ee7bba08b02a6dc.cab' = 'D352BD90617BAE7792763452E153D12D4A9980FCED2CEC78B5243EF0CB5254B4';
            'HLK OnecoreUAP.Filter.Driver Content-x86_en-us.msi' = 'B0091C8EBC8588D1869E693395C044F944A71638EB0D724D4CC4C5CA0EEA286D';
            '85d6bdfb1ace899a13cd5a9b6497fb45.cab' = '331B822480953B7253D6AA4F2EB464FBEA8EB5E94A268998BA41605C6D9D4EF4';
            'c181bddcd2a99df11781ff5fd869ab4f.cab' = 'C4FC705486AADA717650A8C4348B1AF76062C58B350CF8B98479A2C08287EB12';
        };
    },
    @{
        # Windows 11 22H2-23H2
        'suitable_for_os_version' = $os_version -ge [Version]'10.0.22621';
        'name' = 'Windows HLK for Windows 11, versions 22H2-23H2 (10.1.22621.5413)';
        'base_url' = 'https://download.microsoft.com/download/92a1cfff-4d49-48ce-bbce-2b3173229647/hlk/Installers';
        'installer_files' = @(
            'HLK Filter.Driver Content-x86_en-us.msi',
            'HLK OnecoreUAP.Filter.Driver Content-x86_en-us.msi'
        );
        'files' = @{
            'HLK Filter.Driver Content-x86_en-us.msi' = '6DDC552E827C6E4370DE34747377D943F8F175FAF393860A3196420E7770BC08';
            '1717ea82f4ea959810c3561c3404ccc3.cab' = '70434618DD2065CB83CD8904668417014E7C46418ADA1D7F5C21072407A3729E';
            'a71d6782d8573a0654145c06c27cc3b3.cab' = 'A67EFA583E6DC8DC3791D7A7495F184B0D35AB349E120653C35DAEEB12BFB52E';
            'ba00d40293767a06f41411d7a914cca2.cab' = '1C9CAF4F1842F1C5B00BF16226DB942C5568D5AD1295FF7ADFD0D9F1BE6F79D7';
            'ba6c54205789145a9ab50b14a0024a97.cab' = 'A74977A175D1A3AB9AD3FB34A17569E578A6CBDADB6E0981C0A3A1D99D2C1105';
            'HLK OnecoreUAP.Filter.Driver Content-x86_en-us.msi' = '7089AAA8703F7BE6E8E8265408A6372559F3F1D103B5D5CAE8916342C895AA4B';
            '5747e78692ed428f11b4b2b25ad77830.cab' = '3D9C01F575DF3056FA31C7C3B946BAE1C398C24FBD1A65C73AF406DC44FEC8F7';
            '85d6bdfb1ace899a13cd5a9b6497fb45.cab' = '73160CC9E8C1DB1A379B9FD9C56C81B81358C08351810E7ED019DF0DCD430F58';
            'c181bddcd2a99df11781ff5fd869ab4f.cab' = 'F6581C77B0961E05A5C56624F7A8E90A1106CA18A7D8EAB11E342C91E368E3C3';
            'f40c598f451b43f7521a73400ed7bf54.cab' = '1EC67027B6988E0FAA2478A379D839B70C64976E1EC1370E0A74517004CD0E64';
        };
    },
    @{
        # Windows 11 24H2-25H2 / Windows Server 2025
        'suitable_for_os_version' = $os_version -ge [Version]'10.0.26100';
        'name' = 'Windows HLK for Windows 11, versions 24H2-25H2 / Windows Server 2025 (10.1.26100.9818)';
        'base_url' = 'https://download.microsoft.com/download/037e2449-e232-48fa-8d14-2c0929d6134e/KIT_BUNDLE_HLK_MEDIACREATION/Installers';
        'installer_files' = @(
            'HLK Filter.Driver Content-x86_en-us.msi',
            'HLK OnecoreUAP.Filter.Driver Content-x86_en-us.msi'
        );
        'files' = @{
            'HLK Filter.Driver Content-x86_en-us.msi' = 'DC154993E41D5934548FBFE31057D7729E41047A29B99F21FBC726BEA51A89FD';
            'a71d6782d8573a0654145c06c27cc3b3.cab' = 'BAB8E5E8AAF6353BCDAA2856AFD6DE4D4E6459C8FF688D88F2C70BCE1B750E1F';
            'ba00d40293767a06f41411d7a914cca2.cab' = '85CD3484A6DAEE3D0C16328D1D027965DBD900B385A72E7367EFA7C529B59382';
            'ba6c54205789145a9ab50b14a0024a97.cab' = '1F63BFAAC6B7762719C247DD68DAC89D4BBB740A63B225874192E746D75AFF7E';
            'HLK OnecoreUAP.Filter.Driver Content-x86_en-us.msi' = '8B9C4A88215C34602DD9642DB7A36EC6885A180D1BBE4FF8441883EF372ADDB0';
            '85d6bdfb1ace899a13cd5a9b6497fb45.cab' = 'BF76BED4A0A3582E6C35785F66138DD55892BBADA8CAB6B158CC5487172D578F';
            'c181bddcd2a99df11781ff5fd869ab4f.cab' = 'EB0ABD654FD60068B294DFE082E7F6775DFBC89B806A9ECE66069E180D109D5B';
            'f40c598f451b43f7521a73400ed7bf54.cab' = '4FA059BD8A14DC285CB27354B5777DBBC3EE2C00ED3522BFECD7D354E50A2809';
        };
    }
)

$hlk_info = $hlks | Where-Object {$_['suitable_for_os_version']} | Select-Object -Last 1
if ($null -eq $hlk_info) {
    throw "Unsupported Windows version: $os_version. Windows 10 (build 10240) or newer is required."
}
Write-Host "Using $($hlk_info['name'])"
$base_url = $hlk_info['base_url']
$installer_files = $hlk_info['installer_files']
$dl_files = $hlk_info['files']

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

Push-Location $temp_dir
try {
    foreach ($installer_file in $installer_files) {
        $safe_name = ([System.IO.Path]::GetFileNameWithoutExtension($installer_file) -replace '[^A-Za-z0-9._-]', '_')
        $log_file = Join-Path $env:TEMP "ifstest_install_${safe_name}.log"
        Write-Output "Installing MSI: $installer_file"
        $msi_proc = Start-Process -PassThru -Wait -FilePath msiexec.exe -ArgumentList @("/qn", "/norestart", "/lv*", "`"$log_file`"", "/i", "`"$installer_file`"")
        if ($msi_proc.ExitCode -ne 0) {
            Write-Error "IFSTest-Installation failed for $installer_file (exit code $($msi_proc.ExitCode)). Log below this line:"
            if (Test-Path $log_file) {
                Get-Content $log_file
            }
            throw "IFSTest-Installation failed for $installer_file (exit code $($msi_proc.ExitCode)). Log above this line"
        }
    }
} finally {
    Pop-Location
}

# We copy some files into the same dir as the exe.
# Setting the PATH is cumbersome, because IFSTest launches itself under a different user for some tests!
$ifstest_dir = "${nttest_path}\BASETEST\core_file_services\ifs_test_kit\"
$needed_files = @(
    "${nttest_path}\BASETEST\core_file_services\shared_libs\fbslog\FbsLog.dll",
    "${nttest_path}\commontest\ntlog\ntlogger.ini",
    "${nttest_path}\commontest\ntlog\ntlog.dll"
)
Copy-Item -Path $needed_files -Destination $ifstest_dir -Force -ErrorAction Stop

Write-Host "Installation successful."
