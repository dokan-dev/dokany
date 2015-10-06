!define VERSION "0.8.0"

!include LogicLib.nsh
!include x64.nsh
!include WinVer.nsh

Name "DokanLibraryInstaller ${VERSION}"
BrandingText http://dokan-dev.github.io
!ifdef EMBED_PREREQUISITES
	OutFile "DokanInstall_${VERSION}_redist.exe"
!else
	OutFile "DokanInstall_${VERSION}.exe"
!endif

InstallDir $PROGRAMFILES32\Dokan\DokanLibrary
RequestExecutionLevel admin
LicenseData "licdata.rtf"
ShowUninstDetails show

Page license
Page components
Page instfiles

UninstPage uninstConfirm
UninstPage instfiles

!macro Files arch folder

  SetOutPath ${folder}\Dokan\DokanLibrary

    File README.url
    File ..\license.gpl.txt
    File ..\license.lgpl.txt
    File ..\license.mit.txt
    File ..\${arch}\Release\dokanctl.exe
    File ..\${arch}\Release\mounter.exe
	
  SetOutPath ${folder}\Dokan\DokanLibrary\include\dokan
	
	File ..\dokan\dokan.h
	File ..\dokan\fileinfo.h
	
  SetOutPath ${folder}\Dokan\DokanLibrary\lib
  
	File ..\${arch}\Release\dokan.lib
    File ..\${arch}\Release\dokanfuse.lib

  SetOutPath ${folder}\Dokan\DokanLibrary\sample\mirror

    File ..\dokan_mirror\dokan_mirror.vcxproj
    File ..\dokan_mirror\mirror.c
    File ..\${arch}\Release\mirror.exe

  SetOutPath ${folder}\Dokan\DokanLibrary\include\fuse

    File /r "..\dokan_fuse\include\"
	
  ${If} ${arch} == "x64"
	${DisableX64FSRedirection}
  ${EndIf}

  SetOutPath $SYSDIR

    File ..\${arch}\Release\dokan.dll
    File ..\${arch}\Release\dokannp.dll

  ${If} ${arch} == "x64"
	${EnableX64FSRedirection}
  ${EndIf}

!macroend

!macro PDBFiles os arch folder

  SetOutPath ${folder}\Dokan\DokanLibrary\pdb
 
    File ..\${arch}\Release\dokanctl.pdb
    File ..\${arch}\Release\mounter.pdb
	
  SetOutPath ${folder}\Dokan\DokanLibrary\pdb\mirror
  
	File ..\${arch}\Release\mirror.pdb
	
  SetOutPath ${folder}\Dokan\DokanLibrary\pdb\driver
 
    File ..\${arch}\${os}Release\dokan.pdb
	
!macroend

!macro DokanSetup
  ExecWait '"$PROGRAMFILES32\Dokan\DokanLibrary\dokanctl.exe" /i a' $0
  ExecWait '"$PROGRAMFILES32\Dokan\DokanLibrary\dokanctl.exe" /i n' $0
  DetailPrint "dokanctl returned $0"
  WriteUninstaller $PROGRAMFILES32\Dokan\DokanLibrary\DokanUninstall.exe

  ; Write the uninstall keys for Windows
  ${If} ${RunningX64}
	SetRegView 64
  ${Else}
    SetRegView 32
  ${EndIf}
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DokanLibrary" "DisplayName" "Dokan Library ${VERSION}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DokanLibrary" "UninstallString" '"$PROGRAMFILES32\Dokan\DokanLibrary\DokanUninstall.exe"'
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DokanLibrary" "NoModify" 1
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DokanLibrary" "NoRepair" 1

!macroend

!macro Driver os arch
  ${If} ${arch} == "x64"
	${DisableX64FSRedirection}
  ${EndIf}

  SetOutPath $SYSDIR\drivers

    File ..\${arch}\${os}Release\dokan.sys

  ${If} ${arch} == "x64"
    ${EnableX64FSRedirection}
  ${EndIf}
!macroend

Section -Prerequisites
  ; Check VC++ 2013 is installed on the system
  
  SetOutPath "$INSTDIR"
  
  IfSilent endVCRedist
  ${If} ${RunningX64}
	SetRegView 32
	ReadRegStr $0 HKLM "SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{13A4EE12-23EA-3371-91EE-EFB36DDFFF3E}" "Version"
	${If} $0 == ""
		Goto beginVCRedist_x86
	${EndIf}
	SetRegView 64
	ReadRegStr $0 HKLM "SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{A749D8E6-B613-3BE3-8F5F-045C84EBA29B}" "Version"
	${If} $0 == ""
		Goto beginVCRedist_x64
	${EndIf}
  ${Else}
	ReadRegStr $0 HKLM "SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{13A4EE12-23EA-3371-91EE-EFB36DDFFF3E}" "Version"
	${If} $0 == ""
		Goto beginVCRedist_x86
	${EndIf}
  ${EndIf}
  Goto endVCRedist
  
  beginVCRedist_x86:
  !ifdef EMBED_PREREQUISITES
	  File "vcredist_x86.exe"
	  ExecWait '"$INSTDIR\vcredist_x86.exe"  /passive /norestart'
	  ${If} ${RunningX64}
		beginVCRedist_x64:
		File "vcredist_x64.exe"
		ExecWait '"$INSTDIR\vcredist_x64.exe"  /passive /norestart'
	  ${EndIf}
  !else
	beginVCRedist_x64:
	MessageBox MB_YESNO "Your system does not appear to have Microsoft Visual C++ 2013 Runtime installed.$\n$\nWould you like to download it?" IDNO endVCRedist
	ExecShell "open" "https://www.microsoft.com/en-US/download/details.aspx?id=40784"
	Abort
  !endif
  endVCRedist:
SectionEnd


Section "Dokan Library x86" section_x86
  !insertmacro Files "Win32" $PROGRAMFILES32
SectionEnd

Section "Dokan Library x64" section_x64
  !insertmacro Files "x64" $PROGRAMFILES64
SectionEnd

Section "Dokan PDB x86" section_x86_pdb
  ${If} ${IsWin7}
    !insertmacro PDBFiles "Win7" "Win32" $PROGRAMFILES32
  ${ElseIf} ${IsWin2008R2}
    !insertmacro PDBFiles "Win7" "Win32" $PROGRAMFILES32 
  ${ElseIf} ${IsWin8}
    !insertmacro PDBFiles "Win8" "Win32" $PROGRAMFILES32
  ${ElseIf} ${IsWin2012}
    !insertmacro PDBFiles "Win8" "Win32" $PROGRAMFILES32
  ${ElseIf} ${IsWin8.1}
    !insertmacro PDBFiles "Win8.1" "Win32" $PROGRAMFILES32
  ${ElseIf} ${IsWin2012R2}
    !insertmacro PDBFiles "Win8.1" "Win32" $PROGRAMFILES32
  ${EndIf}
SectionEnd

Section "Dokan PDB x64" section_x64_pdb
  ${If} ${IsWin7}
    !insertmacro PDBFiles "Win7" "x64" $PROGRAMFILES64
  ${ElseIf} ${IsWin2008R2}
    !insertmacro PDBFiles "Win7" "x64" $PROGRAMFILES64
  ${ElseIf} ${IsWin8}
    !insertmacro PDBFiles "Win8" "x64" $PROGRAMFILES64
  ${ElseIf} ${IsWin2012}
    !insertmacro PDBFiles "Win8" "x64" $PROGRAMFILES64
  ${ElseIf} ${IsWin8.1}
    !insertmacro PDBFiles "Win8.1" "x64" $PROGRAMFILES64
  ${ElseIf} ${IsWin2012R2}
    !insertmacro PDBFiles "Win8.1" "x64" $PROGRAMFILES64
  ${EndIf}
SectionEnd

Section "Dokan Driver x86" section_x86_driver
  ${If} ${IsWin7}
    !insertmacro Driver "Win7" "Win32"
  ${ElseIf} ${IsWin2008R2}
    !insertmacro Driver "Win7" "Win32"
  ${ElseIf} ${IsWin8}
    !insertmacro Driver "Win8" "Win32"
  ${ElseIf} ${IsWin2012}
    !insertmacro Driver "Win8" "Win32"
  ${ElseIf} ${IsWin8.1}
    !insertmacro Driver "Win8.1" "Win32"
  ${ElseIf} ${IsWin2012R2}
    !insertmacro Driver "Win8.1" "Win32"
  ${EndIf}
  !insertmacro DokanSetup
SectionEnd

Section "Dokan Driver x64" section_x64_driver
  ${If} ${IsWin7}
    !insertmacro Driver "Win7" "x64"
  ${ElseIf} ${IsWin2008R2}
    !insertmacro Driver "Win7" "x64"
  ${ElseIf} ${IsWin8}
    !insertmacro Driver "Win8" "x64"
  ${ElseIf} ${IsWin2012}
    !insertmacro Driver "Win8" "x64"
  ${ElseIf} ${IsWin8.1}
    !insertmacro Driver "Win8.1" "x64"
  ${ElseIf} ${IsWin2012R2}
    !insertmacro Driver "Win8.1" "x64"
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
      Delete /REBOOTOK $SYSDIR\drivers\dokan.sys
      Delete $SYSDIR\dokan.dll
      Delete $SYSDIR\dokannp.dll
    ${EnableX64FSRedirection}
  ${Else}
    Delete /REBOOTOK $SYSDIR\drivers\dokan.sys
  ${EndIf}

  ; Remove registry keys
  ${If} ${RunningX64}
	SetRegView 64
  ${Else}
    SetRegView 32
  ${EndIf}
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DokanLibrary"

  IfSilent noreboot
    MessageBox MB_YESNO "A reboot is required to finish the uninstallation. Do you wish to reboot now?" IDNO noreboot
    Reboot
  noreboot:


SectionEnd

Function .onInit
  IntOp $0 ${SF_SELECTED} | ${SF_RO}
  SectionSetFlags ${section_x86} $0
  ${If} ${RunningX64}
    SectionSetFlags ${section_x64} $0
    SectionSetFlags ${section_x86_driver} ${SF_RO}  ; disable
    SectionSetFlags ${section_x64_driver} $0
  ${Else}
    SectionSetFlags ${section_x64} ${SF_RO}  ; disable
    SectionSetFlags ${section_x86_driver} $0
    SectionSetFlags ${section_x64_driver} ${SF_RO}  ; disable
  ${EndIf}
  
  IntOp $0 ${SF_USELECTED} | !${SF_RO}
  SectionSetFlags ${section_x86_pdb} $0
  ${If} ${RunningX64}
	SectionSetFlags ${section_x64_pdb} $0
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

