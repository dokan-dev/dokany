$files = Get-ChildItem -path Win32,x64 -recurse -Include *.sys,*.cat,*.dll

signtool sign /v /i "$env:CERTISSUER" /ac "$env:ADDITIONALCERT" /t http://timestamp.verisign.com/scripts/timstamp.dll $files
signtool sign /as /fd SHA256 /v /i "$env:CERTISSUER" /ac "$env:ADDITIONALCERT" /tr http://timestamp.globalsign.com/?signature=sha2 /td SHA256 $files



