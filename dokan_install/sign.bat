REM Internal script to sign installer binary before publishing a release on github
REM This will *NOT* work on your computer without the excepted driver signing certificate!

REM Because of various code signing certificates on our build machines, this script use explicitly a code signing certificate issued by GoDaddy (/i Daddy).

REM Sign exe files
signtool sign /v /i Daddy /ac "..\cert\Go Daddy Class 2 Certification Authority.cer" /t http://timestamp.verisign.com/scripts/timstamp.dll *.exe