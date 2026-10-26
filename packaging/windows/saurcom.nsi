; NSIS installer for Eye of SaurCom. Built by tools/package/make_windows.sh: makensis -DVERSION=... -DSRC=<folder> -DOUTFILE=<setup.exe>
!ifndef SEP
  !define SEP "/"
!endif
!include "MUI2.nsh"
!include "x64.nsh"
Name "Eye of SaurCom"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\SaurCom"
InstallDirRegKey HKLM "Software\SaurCom" "Install_Dir"
RequestExecutionLevel admin
!define UNINST "Software\Microsoft\Windows\CurrentVersion\Uninstall\SaurCom"
!define MUI_ICON "${ICON}"
!define MUI_UNICON "${ICON}"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${SRC}${SEP}LICENSE.txt"
!define MUI_PAGE_CUSTOMFUNCTION_LEAVE DirLeave
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_FUNCTION StartSaurCom
!define MUI_FINISHPAGE_RUN_TEXT "Start Eye of SaurCom"
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_OK|MB_ICONSTOP "Eye of SaurCom needs 64-bit Windows 10 or 11."
    Abort
  ${EndIf}
FunctionEnd

; The uninstaller removes the whole install folder: a folder that is not Eye of SaurCom's own (C:\Program Files, C:\Tools ...) would lose everything in it.
Function DirLeave
  StrLen $0 "$INSTDIR"
  IntOp $0 $0 - 6
  ${If} $0 < 0
    StrCpy $INSTDIR "$INSTDIR\SaurCom"
  ${Else}
    StrCpy $1 "$INSTDIR" 6 $0
    StrCmp $1 "SaurCom" +2 0   ; (not case sensitive)
    StrCpy $INSTDIR "$INSTDIR\SaurCom"
  ${EndIf}
FunctionEnd

; Started through explorer.exe: the program then runs with the rights of the user, not as the administrator that the installer is (the same way the updater starts it)
Function StartSaurCom
  Exec '"$WINDIR\explorer.exe" "$INSTDIR\SaurCom.exe"'
FunctionEnd

Section "Eye of SaurCom"
  SetShellVarContext current   ; remove the per-user shortcuts that version 0.1.0's first installer created
  Delete "$SMPROGRAMS\SaurCom\SaurCom.lnk"
  Delete "$SMPROGRAMS\SaurCom\Uninstall.lnk"
  RMDir "$SMPROGRAMS\SaurCom"
  Delete "$DESKTOP\SaurCom.lnk"
  SetShellVarContext all   ; the program is installed for all users: so are its shortcuts
  SetOutPath "$INSTDIR"
  ; an update over an older Eye of SaurCom: its libraries go first, so that none of a former version stays (an old SoapySDR module next to new libraries, ...)
  IfFileExists "$INSTDIR\SaurCom.exe" 0 +4
    Delete "$INSTDIR\*.dll"
    RMDir /r "$INSTDIR\lib"
    RMDir /r "$INSTDIR\licenses"
  File /r "${SRC}${SEP}*.*"
  WriteRegStr HKLM "Software\SaurCom" "Install_Dir" "$INSTDIR"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\SaurCom" "DisplayName" "Eye of SaurCom"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\SaurCom" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\SaurCom" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKLM "${UNINST}" "Publisher" "Eye of SaurCom project"
  WriteRegStr HKLM "${UNINST}" "DisplayIcon" "$INSTDIR\SaurCom.exe"
  WriteRegStr HKLM "${UNINST}" "URLInfoAbout" "https://github.com/AxolDad/Eye_of_ZorCom"
  WriteRegDWORD HKLM "${UNINST}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST}" "NoRepair" 1
  SectionGetSize 0 $0
  WriteRegDWORD HKLM "${UNINST}" "EstimatedSize" $0
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  CreateDirectory "$SMPROGRAMS\SaurCom"
  CreateShortcut "$SMPROGRAMS\SaurCom\SaurCom.lnk" "$INSTDIR\SaurCom.exe"
  CreateShortcut "$SMPROGRAMS\SaurCom\Uninstall.lnk" "$INSTDIR\Uninstall.exe"
  CreateShortcut "$DESKTOP\SaurCom.lnk" "$INSTDIR\SaurCom.exe"
SectionEnd

Section "Uninstall"
  SetShellVarContext current   ; per-user shortcuts left by an earlier installer
  Delete "$SMPROGRAMS\SaurCom\SaurCom.lnk"
  Delete "$SMPROGRAMS\SaurCom\Uninstall.lnk"
  RMDir "$SMPROGRAMS\SaurCom"
  Delete "$DESKTOP\SaurCom.lnk"
  SetShellVarContext all
  Delete "$DESKTOP\SaurCom.lnk"
  RMDir /r "$SMPROGRAMS\SaurCom"
  RMDir /r "$INSTDIR"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\SaurCom"
  DeleteRegKey HKLM "Software\SaurCom"
SectionEnd
