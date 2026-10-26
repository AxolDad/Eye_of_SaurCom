; NSIS installer for Eye of ZorCom. Built by tools/package/make_windows.sh: makensis -DVERSION=... -DSRC=<folder> -DOUTFILE=<setup.exe>
!ifndef SEP
  !define SEP "/"
!endif
!include "MUI2.nsh"
!include "x64.nsh"
Name "Eye of ZorCom"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\ZorCom"
InstallDirRegKey HKLM "Software\ZorCom" "Install_Dir"
RequestExecutionLevel admin
!define UNINST "Software\Microsoft\Windows\CurrentVersion\Uninstall\ZorCom"
!define MUI_ICON "${ICON}"
!define MUI_UNICON "${ICON}"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${SRC}${SEP}LICENSE.txt"
!define MUI_PAGE_CUSTOMFUNCTION_LEAVE DirLeave
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_FUNCTION StartZorCom
!define MUI_FINISHPAGE_RUN_TEXT "Start Eye of ZorCom"
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_OK|MB_ICONSTOP "Eye of ZorCom needs 64-bit Windows 10 or 11."
    Abort
  ${EndIf}
FunctionEnd

; The uninstaller removes the whole install folder: a folder that is not Eye of ZorCom's own (C:\Program Files, C:\Tools ...) would lose everything in it.
Function DirLeave
  StrLen $0 "$INSTDIR"
  IntOp $0 $0 - 6
  ${If} $0 < 0
    StrCpy $INSTDIR "$INSTDIR\ZorCom"
  ${Else}
    StrCpy $1 "$INSTDIR" 6 $0
    StrCmp $1 "ZorCom" +2 0   ; (not case sensitive)
    StrCpy $INSTDIR "$INSTDIR\ZorCom"
  ${EndIf}
FunctionEnd

; Started through explorer.exe: the program then runs with the rights of the user, not as the administrator that the installer is (the same way the updater starts it)
Function StartZorCom
  Exec '"$WINDIR\explorer.exe" "$INSTDIR\ZorCom.exe"'
FunctionEnd

Section "Eye of ZorCom"
  SetShellVarContext current   ; remove the per-user shortcuts that version 0.1.0's first installer created
  Delete "$SMPROGRAMS\ZorCom\ZorCom.lnk"
  Delete "$SMPROGRAMS\ZorCom\Uninstall.lnk"
  RMDir "$SMPROGRAMS\ZorCom"
  Delete "$DESKTOP\ZorCom.lnk"
  SetShellVarContext all   ; the program is installed for all users: so are its shortcuts
  SetOutPath "$INSTDIR"
  ; an update over an older Eye of ZorCom: its libraries go first, so that none of a former version stays (an old SoapySDR module next to new libraries, ...)
  IfFileExists "$INSTDIR\ZorCom.exe" 0 +4
    Delete "$INSTDIR\*.dll"
    RMDir /r "$INSTDIR\lib"
    RMDir /r "$INSTDIR\licenses"
  File /r "${SRC}${SEP}*.*"
  WriteRegStr HKLM "Software\ZorCom" "Install_Dir" "$INSTDIR"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ZorCom" "DisplayName" "Eye of ZorCom"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ZorCom" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ZorCom" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKLM "${UNINST}" "Publisher" "Eye of ZorCom project"
  WriteRegStr HKLM "${UNINST}" "DisplayIcon" "$INSTDIR\ZorCom.exe"
  WriteRegStr HKLM "${UNINST}" "URLInfoAbout" "https://github.com/AxolDad/OnAir-SDR-Suite"
  WriteRegDWORD HKLM "${UNINST}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST}" "NoRepair" 1
  SectionGetSize 0 $0
  WriteRegDWORD HKLM "${UNINST}" "EstimatedSize" $0
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  CreateDirectory "$SMPROGRAMS\ZorCom"
  CreateShortcut "$SMPROGRAMS\ZorCom\ZorCom.lnk" "$INSTDIR\ZorCom.exe"
  CreateShortcut "$SMPROGRAMS\ZorCom\Uninstall.lnk" "$INSTDIR\Uninstall.exe"
  CreateShortcut "$DESKTOP\ZorCom.lnk" "$INSTDIR\ZorCom.exe"
SectionEnd

Section "Uninstall"
  SetShellVarContext current   ; per-user shortcuts left by an earlier installer
  Delete "$SMPROGRAMS\ZorCom\ZorCom.lnk"
  Delete "$SMPROGRAMS\ZorCom\Uninstall.lnk"
  RMDir "$SMPROGRAMS\ZorCom"
  Delete "$DESKTOP\ZorCom.lnk"
  SetShellVarContext all
  Delete "$DESKTOP\ZorCom.lnk"
  RMDir /r "$SMPROGRAMS\ZorCom"
  RMDir /r "$INSTDIR"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ZorCom"
  DeleteRegKey HKLM "Software\ZorCom"
SectionEnd
