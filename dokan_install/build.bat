"C:\Program Files (x86)\NSIS\makensis.exe" install.nsi
"C:\Program Files (x86)\NSIS\makensis.exe" install-redist.nsi
call sign.bat
"C:\Program Files\7-Zip\7z.exe" a -tzip dokan.zip ../Win32 ../x64