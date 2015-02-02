REM Internal script to sign binaries before publishing a release on github
REM This will *NOT* work on your computer without the excepted driver signing certificate!

REM Because of various code signing certificates on our build machines, this script use explicitly a code signing certificate issued by GoDaddy (/i Daddy).

REM Sign dll/exe files
signtool sign /v /i Daddy /ac "cert\Go Daddy Class 2 Certification Authority.cer" /t http://timestamp.verisign.com/scripts/timstamp.dll Release\*.dll
signtool sign /v /i Daddy /ac "cert\Go Daddy Class 2 Certification Authority.cer" /t http://timestamp.verisign.com/scripts/timstamp.dll Release\*.exe

REM Sign driver files...
signtool sign /v /i Daddy /ac "cert\Go Daddy Class 2 Certification Authority.cer" /t http://timestamp.verisign.com/scripts/timstamp.dll Win7Release\dokan.sys
signtool sign /v /i Daddy /ac "cert\Go Daddy Class 2 Certification Authority.cer" /t http://timestamp.verisign.com/scripts/timstamp.dll Win8Release\dokan.sys
signtool sign /v /i Daddy /ac "cert\Go Daddy Class 2 Certification Authority.cer" /t http://timestamp.verisign.com/scripts/timstamp.dll Win8.1Release\dokan.sys
signtool sign /v /i Daddy /ac "cert\Go Daddy Class 2 Certification Authority.cer" /t http://timestamp.verisign.com/scripts/timstamp.dll x64\Win7Release\dokan.sys
signtool sign /v /i Daddy /ac "cert\Go Daddy Class 2 Certification Authority.cer" /t http://timestamp.verisign.com/scripts/timstamp.dll x64\Win8Release\dokan.sys
signtool sign /v /i Daddy /ac "cert\Go Daddy Class 2 Certification Authority.cer" /t http://timestamp.verisign.com/scripts/timstamp.dll x64\Win8.1Release\dokan.sys