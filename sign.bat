REM Internal script to sign binaries before publishing a release on github
REM This will *NOT* work on your computer without the excepted driver signing certificate!

REM Because of various code signing certificates on our build machines, this script use explicitly a code signing certificate issued by GoDaddy (/i Daddy).

REM Sign dll/exe files
signtool sign /fd SHA256 /v /i Daddy /ac "cert\Go Daddy Root Certification Authority - G2.cer" /tr http://timestamp.globalsign.com/?signature=sha2 /td SHA256 Win32\Release\*.dll
signtool sign /fd SHA256 /v /i Daddy /ac "cert\Go Daddy Root Certification Authority - G2.cer" /tr http://timestamp.globalsign.com/?signature=sha2 /td SHA256 Win32\Release\*.exe
signtool sign /fd SHA256 /v /i Daddy /ac "cert\Go Daddy Root Certification Authority - G2.cer" /tr http://timestamp.globalsign.com/?signature=sha2 /td SHA256 x64\Release\*.dll
signtool sign /fd SHA256 /v /i Daddy /ac "cert\Go Daddy Root Certification Authority - G2.cer" /tr http://timestamp.globalsign.com/?signature=sha2 /td SHA256 x64\Release\*.exe

REM Sign driver files...
signtool sign /fd SHA256 /v /i Daddy /ac "cert\Go Daddy Root Certification Authority - G2.cer" /tr http://timestamp.globalsign.com/?signature=sha2 /td SHA256 Win32\Win7Release\*.sys
signtool sign /fd SHA256 /v /i Daddy /ac "cert\Go Daddy Root Certification Authority - G2.cer" /tr http://timestamp.globalsign.com/?signature=sha2 /td SHA256 Win32\Win8Release\*.sys
signtool sign /fd SHA256 /v /i Daddy /ac "cert\Go Daddy Root Certification Authority - G2.cer" /tr http://timestamp.globalsign.com/?signature=sha2 /td SHA256 Win32\Win8.1Release\*.sys
signtool sign /fd SHA256 /v /i Daddy /ac "cert\Go Daddy Root Certification Authority - G2.cer" /tr http://timestamp.globalsign.com/?signature=sha2 /td SHA256 Win32\Win10Release\*.sys
signtool sign /fd SHA256 /v /i Daddy /ac "cert\Go Daddy Root Certification Authority - G2.cer" /tr http://timestamp.globalsign.com/?signature=sha2 /td SHA256 x64\Win7Release\*.sys
signtool sign /fd SHA256 /v /i Daddy /ac "cert\Go Daddy Root Certification Authority - G2.cer" /tr http://timestamp.globalsign.com/?signature=sha2 /td SHA256 x64\Win8Release\*.sys
signtool sign /fd SHA256 /v /i Daddy /ac "cert\Go Daddy Root Certification Authority - G2.cer" /tr http://timestamp.globalsign.com/?signature=sha2 /td SHA256 x64\Win8.1Release\*.sys
signtool sign /fd SHA256 /v /i Daddy /ac "cert\Go Daddy Root Certification Authority - G2.cer" /tr http://timestamp.globalsign.com/?signature=sha2 /td SHA256 x64\Win10Release\*.sys