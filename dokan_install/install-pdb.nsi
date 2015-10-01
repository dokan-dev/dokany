!define VERSION "0.8.0"

!include LogicLib.nsh
!include x64.nsh
!include WinVer.nsh

Name "DokanLibraryInstaller ${VERSION}"
BrandingText http://dokan-dev.github.io
OutFile "DokanInstall_${VERSION}_pdb.exe"

InstallDir $PROGRAMFILES32\Dokan\DokanLibrary\pdb
RequestExecutionLevel admin
LicenseData "licdata.rtf"
ShowUninstDetails show

Page license
Page components
Page instfiles

UninstPage uninstConfirm
UninstPage instfiles


!macro X86Files

  SetOutPath $PROGRAMFILES32\Dokan\DokanLibrary\pdb
 
    File README.url
	File ..\license.gpl.txt
    File ..\license.lgpl.txt
    File ..\license.mit.txt

  SetOutPath $PROGRAMFILES32\Dokan\DokanLibrary\pdb
 
    File ..\Win32\Release\dokanctl.pdb
    File ..\Win32\Release\mounter.pdb
	File ..\Win32\Release\mirror.pdb
	
  SetOutPath $PROGRAMFILES32\Dokan\DokanLibrary\pdb\driver\Win7
 
    File ..\Win32\Win7Release\dokan.pdb
	
  SetOutPath $PROGRAMFILES32\Dokan\DokanLibrary\pdb\driver\Win8
 
    File ..\Win32\Win8Release\dokan.pdb
	
  SetOutPath $PROGRAMFILES32\Dokan\DokanLibrary\pdb\driver\Win8.1
 
    File ..\Win32\Win8.1Release\dokan.pdb

!macroend

!macro X64Files

  SetOutPath $PROGRAMFILES64\Dokan\DokanLibrary\pdb
 
    File README.url
	File ..\license.gpl.txt
    File ..\license.lgpl.txt
    File ..\license.mit.txt

  SetOutPath $PROGRAMFILES64\Dokan\DokanLibrary\pdb
 
    File ..\x64\Release\dokanctl.pdb
    File ..\x64\Release\mounter.pdb
	File ..\x64\Release\mirror.pdb
	
  SetOutPath $PROGRAMFILES64\Dokan\DokanLibrary\pdb\driver\Win7
 
    File ..\x64\Win7Release\dokan.pdb
	
  SetOutPath $PROGRAMFILES64\Dokan\DokanLibrary\pdb\driver\Win8
 
    File ..\x64\Win8Release\dokan.pdb
	
  SetOutPath $PROGRAMFILES64\Dokan\DokanLibrary\pdb\driver\Win8.1
 
    File ..\x64\Win8.1Release\dokan.pdb

!macroend

!macro DokanSetup
  WriteUninstaller $PROGRAMFILES32\Dokan\DokanLibrary\pdb\DokanPDBUninstall.exe

  ; Write the uninstall keys for Windows
  ${If} ${RunningX64}
	SetRegView 64
  ${Else}
    SetRegView 32
  ${EndIf}
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DokanLibraryPDB" "DisplayName" "Dokan Library PDB ${VERSION}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DokanLibraryPDB" "UninstallString" '"$PROGRAMFILES32\Dokan\DokanLibrary\pdb\DokanPDBUninstall.exe"'
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DokanLibraryPDB" "NoModify" 1
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DokanLibraryPDB" "NoRepair" 1

!macroend

Section "Dokan PDB x86" section_x86
  !insertmacro X86Files
  !insertmacro DokanSetup
SectionEnd

Section "Dokan PDB x64" section_x64
  !insertmacro X64Files
  !insertmacro DokanSetup
SectionEnd

Section "Uninstall"

  RMDir /r $PROGRAMFILES32\Dokan\DokanLibrary\pdb
  RMDir $PROGRAMFILES32\Dokan

  ${If} ${RunningX64}
    RMDir /r $PROGRAMFILES64\Dokan\DokanLibrary\pdb
    RMDir $PROGRAMFILES64\Dokan
  ${EndIf}

  ; Remove registry keys
  ${If} ${RunningX64}
	SetRegView 64
  ${Else}
    SetRegView 32
  ${EndIf}
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DokanLibraryPDB"

SectionEnd

Function .onInit
  IntOp $0 ${SF_SELECTED} | ${SF_RO}
  ${If} ${RunningX64}
    SectionSetFlags ${section_x86} $0
    SectionSetFlags ${section_x64} $0
  ${Else}
    SectionSetFlags ${section_x86} $0
    SectionSetFlags ${section_x64} ${SF_RO}  ; disable
  ${EndIf}
FunctionEnd

Function .onInstSuccess
  IfSilent noshellopen
    ExecShell "open" "$PROGRAMFILES32\Dokan\DokanLibrary\pdb"
  noshellopen:
FunctionEnd

