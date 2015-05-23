REM Internal script to sign binaries before publishing a release on github
REM This will *NOT* work on your computer without the excepted driver signing certificate!

REM Because of various code signing certificates on our build machines, this script use explicitly a code signing certificate issued by GoDaddy (/i Daddy).

REM Sign dll/exe files
signtool sign /v /i Daddy /ac "cert\Go Daddy Class 2 Certification Authority.cer" /t http://timestamp.verisign.com/scripts/timstamp.dll Win32\Release\*.dll
signtool sign /v /i Daddy /ac "cert\Go Daddy Class 2 Certification Authority.cer" /t http://timestamp.verisign.com/scripts/timstamp.dll Win32\Release\*.exe
signtool sign /v /i Daddy /ac "cert\Go Daddy Class 2 Certification Authority.cer" /t http://timestamp.verisign.com/scripts/timstamp.dll x64\Release\*.dll
signtool sign /v /i Daddy /ac "cert\Go Daddy Class 2 Certification Authority.cer" /t http://timestamp.verisign.com/scripts/timstamp.dll x64\Release\*.exe

REM Sign driver files...
signtool sign /v /i Daddy /ac "cert\Go Daddy Class 2 Certification Authority.cer" /t http://timestamp.verisign.com/scripts/timstamp.dll Win32\Win7Release\dokan.sys
signtool sign /v /i Daddy /ac "cert\Go Daddy Class 2 Certification Authority.cer" /t http://timestamp.verisign.com/scripts/timstamp.dll Win32\Win8Release\dokan.sys
signtool sign /v /i Daddy /ac "cert\Go Daddy Class 2 Certification Authority.cer" /t http://timestamp.verisign.com/scripts/timstamp.dll Win32\Win8.1Release\dokan.sys
signtool sign /v /i Daddy /ac "cert\Go Daddy Class 2 Certification Authority.cer" /t http://timestamp.verisign.com/scripts/timstamp.dll x64\Win7Release\dokan.sys
signtool sign /v /i Daddy /ac "cert\Go Daddy Class 2 Certification Authority.cer" /t http://timestamp.verisign.com/scripts/timstamp.dll x64\Win8Release\dokan.sys
signtool sign /v /i Daddy /ac "cert\Go Daddy Class 2 Certification Authority.cer" /t http://timestamp.verisign.com/scripts/timstamp.dll x64\Win8.1Release\dokan.sys