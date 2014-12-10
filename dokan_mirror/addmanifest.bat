
SET MT_PATH="C:\Program Files (x86)\Microsoft SDKs\Windows\v7.0A\bin\mt.exe"
%MT_PATH% -manifest "mirror.exe.manifest"  -outputresource:"objchk_wlh_x86\i386\mirror.exe"
%MT_PATH% -manifest "mirror.exe.manifest"  -outputresource:"objchk_wnet_x86\i386\mirror.exe"
%MT_PATH% -manifest "mirror.exe.manifest"  -outputresource:"objchk_win7_x86\i386\mirror.exe"
%MT_PATH% -manifest "mirror.exe.manifest"  -outputresource:"objchk_wxp_x86\i386\mirror.exe"
