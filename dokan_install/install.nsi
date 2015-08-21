!define VERSION "0.7.4"

!include LogicLib.nsh
!include x64.nsh
!include WinVer.nsh

Name "DokanLibraryInstaller ${VERSION}"
BrandingText http://dokan-dev.github.io
OutFile "DokanInstall_${VERSION}.exe"

InstallDir $PROGRAMFILES32\Dokan\DokanLibrary
RequestExecutionLevel admin
LicenseData "licdata.rtf"
ShowUninstDetails show

Page license
Page components
Page instfiles

UninstPage uninstConfirm
UninstPage instfiles


!macro X86Files os

  SetOutPath $PROGRAMFILES32\Dokan\DokanLibrary
 
    File README.url
    File ..\dokan\dokan.h
    File ..\license.gpl.txt
    File ..\license.lgpl.txt
    File ..\license.mit.txt
    File ..\Win32\Release\dokan.lib
    File ..\Win32\Release\dokanfuse.lib
    File ..\Win32\Release\dokanctl.exe
    File ..\Win32\Release\mounter.exe

  SetOutPath $PROGRAMFILES32\Dokan\DokanLibrary\sample\mirror

    File ..\dokan_mirror\dokan_mirror.vcxproj
	File ..\dokan_mirror\mirror.c
    File ..\Win32\Release\mirror.exe
	
  SetOutPath $PROGRAMFILES32\Dokan\DokanLibrary\include\fuse

    File /r "..\dokan_fuse\include\"

  SetOutPath $SYSDIR

    File ..\Win32\Release\dokan.dll
    File ..\Win32\Release\dokannp.dll

!macroend

!macro X64Files os

  SetOutPath $PROGRAMFILES64\Dokan\DokanLibrary

    File README.url
    File ..\dokan\dokan.h
    File ..\license.gpl.txt
    File ..\license.lgpl.txt
    File ..\license.mit.txt
    File ..\x64\Release\dokan.lib
    File ..\x64\Release\dokanfuse.lib
    File ..\x64\Release\dokanctl.exe
    File ..\x64\Release\mounter.exe

  SetOutPath $PROGRAMFILES64\Dokan\DokanLibrary\sample\mirror

    File ..\dokan_mirror\dokan_mirror.vcxproj
    File ..\dokan_mirror\mirror.c
    File ..\x64\Release\mirror.exe

  SetOutPath $PROGRAMFILES64\Dokan\DokanLibrary\include\fuse

    File /r "..\dokan_fuse\include\"
	
  ${DisableX64FSRedirection}

  SetOutPath $SYSDIR

    File ..\x64\Release\dokan.dll
    File ..\x64\Release\dokannp.dll

  ${EnableX64FSRedirection}

!macroend

!macro DokanSetup
  ExecWait '"$PROGRAMFILES32\Dokan\DokanLibrary\dokanctl.exe" /i a' $0
  ExecWait '"$PROGRAMFILES32\Dokan\DokanLibrary\dokanctl.exe" /i n' $0
  DetailPrint "dokanctl returned $0"
  WriteUninstaller $PROGRAMFILES32\Dokan\DokanLibrary\DokanUninstall.exe

  ; Write the uninstall keys for Windows
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DokanLibrary" "DisplayName" "Dokan Library ${VERSION}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DokanLibrary" "UninstallString" '"$PROGRAMFILES32\Dokan\DokanLibrary\DokanUninstall.exe"'
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DokanLibrary" "NoModify" 1
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DokanLibrary" "NoRepair" 1

!macroend

!macro X86Driver os
  SetOutPath $SYSDIR\drivers
    File ..\Win32\${os}Release\dokan.sys
!macroend

!macro X64Driver os
  ${DisableX64FSRedirection}

  SetOutPath $SYSDIR\drivers

    File ..\x64\${os}Release\dokan.sys

  ${EnableX64FSRedirection}
!macroend

Section -Prerequisites
  ; Check VC++ 2013 is installed on the system
  
  IfSilent endVCRedist
  ${If} ${RunningX64}
	SetRegView 32
	ReadRegStr $0 HKLM "SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{13A4EE12-23EA-3371-91EE-EFB36DDFFF3E}" "Version"
	${If} $0 == ""
		Goto beginVCRedist
	${EndIf}
	SetRegView 64
	ReadRegStr $0 HKLM "SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{A749D8E6-B613-3BE3-8F5F-045C84EBA29B}" "Version"
	${If} $0 == ""
		Goto beginVCRedist
	${EndIf}
  ${Else}
	ReadRegStr $0 HKLM "SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{13A4EE12-23EA-3371-91EE-EFB36DDFFF3E}" "Version"
	${If} $0 == ""
		Goto beginVCRedist
	${EndIf}
  ${EndIf}
  Goto endVCRedist
  
  beginVCRedist:
  MessageBox MB_YESNO "Your system does not appear to have Microsoft Visual C++ 2013 Runtime installed.$\n$\nWould you like to download it?" IDNO endVCRedist
  ExecShell "open" "https://www.microsoft.com/en-US/download/details.aspx?id=40784"
  Abort
  endVCRedist:
SectionEnd


Section "Dokan Library x86" section_x86
  ${If} ${IsWin7}
    !insertmacro X86Files "Win7"
  ${ElseIf} ${IsWin2008R2}
    !insertmacro X86Files "Win7"
  ${ElseIf} ${IsWin8}
    !insertmacro X86Files "Win8"
  ${ElseIf} ${IsWin2012}
    !insertmacro X86Files "Win8"
  ${ElseIf} ${IsWin8.1}
    !insertmacro X86Files "Win8.1"
  ${ElseIf} ${IsWin2012R2}
    !insertmacro X86Files "Win8.1"
  ${EndIf}
SectionEnd

Section "Dokan Library x64" section_x64
  ${If} ${IsWin7}
    !insertmacro X64Files "Win7"
  ${ElseIf} ${IsWin2008R2}
    !insertmacro X64Files "Win7"
  ${ElseIf} ${IsWin8}
    !insertmacro X64Files "Win8"
  ${ElseIf} ${IsWin2012}
    !insertmacro X64Files "Win8"
  ${ElseIf} ${IsWin8.1}
    !insertmacro X64Files "Win8.1"
  ${ElseIf} ${IsWin2012R2}
    !insertmacro X64Files "Win8.1"
  ${EndIf}
SectionEnd

Section "Dokan Driver x86" section_x86_driver
  ${If} ${IsWin7}
    !insertmacro X86Driver "Win7"
  ${ElseIf} ${IsWin2008R2}
    !insertmacro X86Driver "Win7"
  ${ElseIf} ${IsWin8}
    !insertmacro X86Driver "Win8"
  ${ElseIf} ${IsWin2012}
    !insertmacro X86Driver "Win8"
  ${ElseIf} ${IsWin8.1}
    !insertmacro X86Driver "Win8.1"
  ${ElseIf} ${IsWin2012R2}
    !insertmacro X86Driver "Win8.1"
  ${EndIf}
  !insertmacro DokanSetup
SectionEnd

Section "Dokan Driver x64" section_x64_driver
  ${If} ${IsWin7}
    !insertmacro X64Driver "Win7"
  ${ElseIf} ${IsWin2008R2}
    !insertmacro X64Driver "Win7"
  ${ElseIf} ${IsWin8}
    !insertmacro X64Driver "Win8"
  ${ElseIf} ${IsWin2012}
    !insertmacro X64Driver "Win8"
  ${ElseIf} ${IsWin8.1}
    !insertmacro X64Driver "Win8.1"
  ${ElseIf} ${IsWin2012R2}
    !insertmacro X64Driver "Win8.1"
  ${EndIf}
  !insertmacro DokanSetup
SectionEnd

Section "Uninstall"
  ExecWait '"$PROGRAMFILES32\Dokan\DokanLibrary\dokanctl.exe" /r n' $0
  ExecWait '"$PROGRAMFILES32\Dokan\DokanLibrary\dokanctl.exe" /r a' $0
  DetailPrint "dokanctl.exe returned $0"

  RMDir /r $PROGRAMFILES32\Dokan\DokanLibrary
  RMDir $PROGRAMFILES32\Dokan
  Delete $SYSDIR\dokan.dll
  Delete $SYSDIR\dokannp.dll

  ${If} ${RunningX64}
    RMDir /r $PROGRAMFILES64\Dokan\DokanLibrary
    RMDir $PROGRAMFILES64\Dokan
    ${DisableX64FSRedirection}
      Delete $SYSDIR\drivers\dokan.sys
      Delete $SYSDIR\dokan.dll
      Delete $SYSDIR\dokannp.dll
    ${EnableX64FSRedirection}
  ${Else}
    Delete $SYSDIR\drivers\dokan.sys
  ${EndIf}

  ; Remove registry keys
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DokanLibrary"

  IfSilent noreboot
    MessageBox MB_YESNO "A reboot is required to finish the uninstallation. Do you wish to reboot now?" IDNO noreboot
    Reboot
  noreboot:


SectionEnd

Function .onInit
  IntOp $0 ${SF_SELECTED} | ${SF_RO}
  ${If} ${RunningX64}
    SectionSetFlags ${section_x86} $0
    SectionSetFlags ${section_x64} $0
    SectionSetFlags ${section_x86_driver} ${SF_RO}  ; disable
    SectionSetFlags ${section_x64_driver} $0
  ${Else}
    SectionSetFlags ${section_x86} $0
    SectionSetFlags ${section_x64} ${SF_RO}  ; disable
    SectionSetFlags ${section_x86_driver} $0
    SectionSetFlags ${section_x64_driver} ${SF_RO}  ; disable
  ${EndIf}

  ; Windows Version check

  ${If} ${RunningX64}
    ${If} ${IsWin2008R2}
    ${ElseIf} ${IsWin7}
	${ElseIf} ${IsWin2012}
	${ElseIf} ${IsWin8}
	${ElseIf} ${IsWin2012R2}
	${ElseIf} ${IsWin8.1}
    ${Else}
      MessageBox MB_OK "Your OS is not supported. Dokan library supports Windows 2008R2, 7, 2012, 8, 2012R2, 8.1 for x64."
      Abort
    ${EndIf}
  ${Else}
    ${If} ${IsWin2008R2}
    ${ElseIf} ${IsWin7}
	${ElseIf} ${IsWin2012}
	${ElseIf} ${IsWin8}
	${ElseIf} ${IsWin2012R2}
	${ElseIf} ${IsWin8.1}
    ${Else}
      MessageBox MB_OK "Your OS is not supported. Dokan library supports Windows 2008R2, 7, 2012, 8, 2012R2, 8.1 for x86."
      Abort
    ${EndIf}
  ${EndIf}

  ; Previous version
  ${If} ${RunningX64}
    ${DisableX64FSRedirection}
      IfFileExists $SYSDIR\drivers\dokan.sys HasPreviousVersionX64 NoPreviousVersionX64
      ; To make EnableX64FSRedirection called in both cases, needs duplicated MessageBox code. How can I avoid this?
      HasPreviousVersionX64:
        MessageBox MB_OK "Please unstall the previous version and restart your computer before running this installer."
        Abort
      NoPreviousVersionX64:
    ${EnableX64FSRedirection}
  ${Else}
    IfFileExists $SYSDIR\drivers\dokan.sys HasPreviousVersion NoPreviousVersion
    HasPreviousVersion:
      MessageBox MB_OK "Please unstall the previous version and restart your computer before running this installer."
      Abort
    NoPreviousVersion:
  ${EndIf}


FunctionEnd

Function .onInstSuccess
  IfSilent noshellopen
    ExecShell "open" "$PROGRAMFILES32\Dokan\DokanLibrary"
  noshellopen:
FunctionEnd

