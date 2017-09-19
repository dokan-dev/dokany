# This powershell script need CERTISSUER, ADDITIONALCERT and EV_CERTISSUER env variable set
# CERTISSUER - Certificat issuer name
# ADDITIONALCERT - Your certificat path
# EV_CERTISSUER - Certificat issuer name for EV sign

New-Item -ItemType Directory -Force -Path Win32,x64,ARM | Out-Null
$files = Get-ChildItem -path Win32,x64,ARM -recurse -Include *.sys,*.cat,*.dll,*.exe
signtool sign /v /i "$env:CERTISSUER" /ac "$env:ADDITIONALCERT" /t http://timestamp.verisign.com/scripts/timstamp.dll $files
signtool sign /as /fd SHA256 /v /i "$env:CERTISSUER" /ac "$env:ADDITIONALCERT" /tr http://timestamp.globalsign.com/?signature=sha2 /td SHA256 $files

if (-not ([string]::IsNullOrEmpty($env:EV_CERTISSUER)))
{
	New-Item -ItemType Directory -Force -Path Win32\Win10Release,x64\Win10Release,Win32\Win10Debug,x64\Win10Debug | Out-Null
	$files = Get-ChildItem -path Win32\Win10Release,x64\Win10Release,Win32\Win10Debug,x64\Win10Debug -recurse -Include *.sys,*.cat,*.dll
	signtool sign /as /fd sha256 /tr http://timestamp.digicert.com /td sha256 /n "$env:EV_CERTISSUER" $files
}
