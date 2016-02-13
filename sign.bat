REM Sign Release dll/exe files
REM SHA1
signtool sign /v /i "%CERTISSUER%" /ac "%ADDITIONALCERT%" /t http://timestamp.verisign.com/scripts/timstamp.dll Win32\Release\*.dll
signtool sign /v /i "%CERTISSUER%" /ac "%ADDITIONALCERT%" /t http://timestamp.verisign.com/scripts/timstamp.dll Win32\Release\*.exe
signtool sign /v /i "%CERTISSUER%" /ac "%ADDITIONALCERT%" /t http://timestamp.verisign.com/scripts/timstamp.dll x64\Release\*.dll
signtool sign /v /i "%CERTISSUER%" /ac "%ADDITIONALCERT%" /t http://timestamp.verisign.com/scripts/timstamp.dll x64\Release\*.exe
REM SHA2
signtool sign /as /fd SHA256 /v /i "%CERTISSUER%" /ac "%ADDITIONALCERT%" /tr http://timestamp.globalsign.com/?signature=sha2 /td SHA256 Win32\Release\*.dll
signtool sign /as /fd SHA256 /v /i "%CERTISSUER%" /ac "%ADDITIONALCERT%" /tr http://timestamp.globalsign.com/?signature=sha2 /td SHA256 Win32\Release\*.exe
signtool sign /as /fd SHA256 /v /i "%CERTISSUER%" /ac "%ADDITIONALCERT%" /tr http://timestamp.globalsign.com/?signature=sha2 /td SHA256 x64\Release\*.dll
signtool sign /as /fd SHA256 /v /i "%CERTISSUER%" /ac "%ADDITIONALCERT%" /tr http://timestamp.globalsign.com/?signature=sha2 /td SHA256 x64\Release\*.exe


REM Sign Release driver files...
REM SHA1
signtool sign /v /i "%CERTISSUER%" /ac "%ADDITIONALCERT%" /t http://timestamp.verisign.com/scripts/timstamp.dll Win32\Win7Release\*.sys
signtool sign /v /i "%CERTISSUER%" /ac "%ADDITIONALCERT%" /t http://timestamp.verisign.com/scripts/timstamp.dll Win32\Win8Release\*.sys
signtool sign /v /i "%CERTISSUER%" /ac "%ADDITIONALCERT%" /t http://timestamp.verisign.com/scripts/timstamp.dll Win32\Win8.1Release\*.sys
signtool sign /v /i "%CERTISSUER%" /ac "%ADDITIONALCERT%" /t http://timestamp.verisign.com/scripts/timstamp.dll Win32\Win10Release\*.sys
signtool sign /v /i "%CERTISSUER%" /ac "%ADDITIONALCERT%" /t http://timestamp.verisign.com/scripts/timstamp.dll x64\Win7Release\*.sys
signtool sign /v /i "%CERTISSUER%" /ac "%ADDITIONALCERT%" /t http://timestamp.verisign.com/scripts/timstamp.dll x64\Win8Release\*.sys
signtool sign /v /i "%CERTISSUER%" /ac "%ADDITIONALCERT%" /t http://timestamp.verisign.com/scripts/timstamp.dll x64\Win8.1Release\*.sys
signtool sign /v /i "%CERTISSUER%" /ac "%ADDITIONALCERT%" /t http://timestamp.verisign.com/scripts/timstamp.dll x64\Win10Release\*.sys
REM SHA2
signtool sign /as /fd SHA256 /v /i "%CERTISSUER%" /ac "%ADDITIONALCERT%" /tr http://timestamp.globalsign.com/?signature=sha2 /td SHA256 Win32\Win7Release\*.sys
signtool sign /as /fd SHA256 /v /i "%CERTISSUER%" /ac "%ADDITIONALCERT%" /tr http://timestamp.globalsign.com/?signature=sha2 /td SHA256 Win32\Win8Release\*.sys
signtool sign /as /fd SHA256 /v /i "%CERTISSUER%" /ac "%ADDITIONALCERT%" /tr http://timestamp.globalsign.com/?signature=sha2 /td SHA256 Win32\Win8.1Release\*.sys
signtool sign /as /fd SHA256 /v /i "%CERTISSUER%" /ac "%ADDITIONALCERT%" /tr http://timestamp.globalsign.com/?signature=sha2 /td SHA256 Win32\Win10Release\*.sys
signtool sign /as /fd SHA256 /v /i "%CERTISSUER%" /ac "%ADDITIONALCERT%" /tr http://timestamp.globalsign.com/?signature=sha2 /td SHA256 x64\Win7Release\*.sys
signtool sign /as /fd SHA256 /v /i "%CERTISSUER%" /ac "%ADDITIONALCERT%" /tr http://timestamp.globalsign.com/?signature=sha2 /td SHA256 x64\Win8Release\*.sys
signtool sign /as /fd SHA256 /v /i "%CERTISSUER%" /ac "%ADDITIONALCERT%" /tr http://timestamp.globalsign.com/?signature=sha2 /td SHA256 x64\Win8.1Release\*.sys
signtool sign /as /fd SHA256 /v /i "%CERTISSUER%" /ac "%ADDITIONALCERT%" /tr http://timestamp.globalsign.com/?signature=sha2 /td SHA256 x64\Win10Release\*.sys

REM Sign Debug dll/exe files
REM SHA1
signtool sign /v /i "%CERTISSUER%" /ac "%ADDITIONALCERT%" /t http://timestamp.verisign.com/scripts/timstamp.dll Win32\Debug\*.dll
signtool sign /v /i "%CERTISSUER%" /ac "%ADDITIONALCERT%" /t http://timestamp.verisign.com/scripts/timstamp.dll Win32\Debug\*.exe
signtool sign /v /i "%CERTISSUER%" /ac "%ADDITIONALCERT%" /t http://timestamp.verisign.com/scripts/timstamp.dll x64\Debug\*.dll
signtool sign /v /i "%CERTISSUER%" /ac "%ADDITIONALCERT%" /t http://timestamp.verisign.com/scripts/timstamp.dll x64\Debug\*.exe
REM SHA2
signtool sign /as /fd SHA256 /v /i "%CERTISSUER%" /ac "%ADDITIONALCERT%" /tr http://timestamp.globalsign.com/?signature=sha2 /td SHA256 Win32\Debug\*.dll
signtool sign /as /fd SHA256 /v /i "%CERTISSUER%" /ac "%ADDITIONALCERT%" /tr http://timestamp.globalsign.com/?signature=sha2 /td SHA256 Win32\Debug\*.exe
signtool sign /as /fd SHA256 /v /i "%CERTISSUER%" /ac "%ADDITIONALCERT%" /tr http://timestamp.globalsign.com/?signature=sha2 /td SHA256 x64\Debug\*.dll
signtool sign /as /fd SHA256 /v /i "%CERTISSUER%" /ac "%ADDITIONALCERT%" /tr http://timestamp.globalsign.com/?signature=sha2 /td SHA256 x64\Debug\*.exe


REM Sign Debug driver files...
REM SHA1
signtool sign /v /i "%CERTISSUER%" /ac "%ADDITIONALCERT%" /t http://timestamp.verisign.com/scripts/timstamp.dll Win32\Win7Debug\*.sys
signtool sign /v /i "%CERTISSUER%" /ac "%ADDITIONALCERT%" /t http://timestamp.verisign.com/scripts/timstamp.dll Win32\Win8Debug\*.sys
signtool sign /v /i "%CERTISSUER%" /ac "%ADDITIONALCERT%" /t http://timestamp.verisign.com/scripts/timstamp.dll Win32\Win8.1Debug\*.sys
signtool sign /v /i "%CERTISSUER%" /ac "%ADDITIONALCERT%" /t http://timestamp.verisign.com/scripts/timstamp.dll Win32\Win10Debug\*.sys
signtool sign /v /i "%CERTISSUER%" /ac "%ADDITIONALCERT%" /t http://timestamp.verisign.com/scripts/timstamp.dll x64\Win7Debug\*.sys
signtool sign /v /i "%CERTISSUER%" /ac "%ADDITIONALCERT%" /t http://timestamp.verisign.com/scripts/timstamp.dll x64\Win8Debug\*.sys
signtool sign /v /i "%CERTISSUER%" /ac "%ADDITIONALCERT%" /t http://timestamp.verisign.com/scripts/timstamp.dll x64\Win8.1Debug\*.sys
signtool sign /v /i "%CERTISSUER%" /ac "%ADDITIONALCERT%" /t http://timestamp.verisign.com/scripts/timstamp.dll x64\Win10Debug\*.sys
REM SHA2
signtool sign /as /fd SHA256 /v /i "%CERTISSUER%" /ac "%ADDITIONALCERT%" /tr http://timestamp.globalsign.com/?signature=sha2 /td SHA256 Win32\Win7Debug\*.sys
signtool sign /as /fd SHA256 /v /i "%CERTISSUER%" /ac "%ADDITIONALCERT%" /tr http://timestamp.globalsign.com/?signature=sha2 /td SHA256 Win32\Win8Debug\*.sys
signtool sign /as /fd SHA256 /v /i "%CERTISSUER%" /ac "%ADDITIONALCERT%" /tr http://timestamp.globalsign.com/?signature=sha2 /td SHA256 Win32\Win8.1Debug\*.sys
signtool sign /as /fd SHA256 /v /i "%CERTISSUER%" /ac "%ADDITIONALCERT%" /tr http://timestamp.globalsign.com/?signature=sha2 /td SHA256 Win32\Win10Debug\*.sys
signtool sign /as /fd SHA256 /v /i "%CERTISSUER%" /ac "%ADDITIONALCERT%" /tr http://timestamp.globalsign.com/?signature=sha2 /td SHA256 x64\Win7Debug\*.sys
signtool sign /as /fd SHA256 /v /i "%CERTISSUER%" /ac "%ADDITIONALCERT%" /tr http://timestamp.globalsign.com/?signature=sha2 /td SHA256 x64\Win8Debug\*.sys
signtool sign /as /fd SHA256 /v /i "%CERTISSUER%" /ac "%ADDITIONALCERT%" /tr http://timestamp.globalsign.com/?signature=sha2 /td SHA256 x64\Win8.1Debug\*.sys
signtool sign /as /fd SHA256 /v /i "%CERTISSUER%" /ac "%ADDITIONALCERT%" /tr http://timestamp.globalsign.com/?signature=sha2 /td SHA256 x64\Win10Debug\*.sys