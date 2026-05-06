; -----------------------------------------------------------------------------
; sapicli SAPI HTTP Server -- NSIS installer
;
; Build:
;   makensis -DVERSION=0.8 -DDISTDIR=..\dist deploy\sapisrv.nsi
;   (assumes dist\ contains sapicli.exe, sapisrv.exe, gen-keys.ps1, www\ ...)
;
; Behaviour:
;   - per-machine install (admin elevation requested)
;   - default install dir C:\Program Files\sapisrv\, user-overridable
;   - registers "sapisrv" as a Windows service running as NetworkService
;   - reserves the http://+:8080/ URL ACL for that account
;   - generates %ProgramData%\sapicli\keys.json with a random public-trial
;     key on first install (gen-keys.ps1 is idempotent on re-runs)
;   - "Open the sapisrv web UI in my browser" checkbox on the Finish page
;   - uninstaller stops + removes the service, removes the URL ACL,
;     deletes program files; keeps %ProgramData%\sapicli\ (logs, keys.json)
; -----------------------------------------------------------------------------

!include "MUI2.nsh"
!include "LogicLib.nsh"

!ifndef VERSION
  !define VERSION "0.0.0"
!endif

!ifndef DISTDIR
  !define DISTDIR "..\dist"
!endif

; OUTFILE may be an absolute path; defaults to a relative file next to
; the .nsi (i.e. inside deploy\). CI passes an absolute path so the
; finished installer ends up at the project root for upload-artifact.
!ifndef OUTFILE
  !define OUTFILE "sapisrv-setup-${VERSION}.exe"
!endif

Name "sapicli SAPI HTTP Server ${VERSION}"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES\sapisrv"
RequestExecutionLevel admin
Unicode true
ShowInstDetails show
ShowUninstDetails show

; VIProductVersion needs strict X.X.X.X (four-part) numeric. CI computes
; the padded form and passes it via /DVIVERSION; non-CI builds get a
; benign placeholder.
!ifndef VIVERSION
  !define VIVERSION "0.0.0.0"
!endif
VIProductVersion "${VIVERSION}"
VIAddVersionKey "ProductName"     "sapicli SAPI HTTP Server"
VIAddVersionKey "FileVersion"     "${VERSION}"
VIAddVersionKey "ProductVersion"  "${VERSION}"
VIAddVersionKey "CompanyName"     "vampi.tech"
VIAddVersionKey "FileDescription" "sapicli SAPI HTTP Server installer"

; --- UI ---
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_TEXT "Open the sapisrv web UI in my browser"
!define MUI_FINISHPAGE_RUN_FUNCTION "OpenWebUI"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Function OpenWebUI
  ExecShell "open" "http://localhost:8080/"
FunctionEnd

; --- Install ---
Section "Install" SEC_INSTALL
  SetOutPath "$INSTDIR"
  File "${DISTDIR}\sapicli.exe"
  File "${DISTDIR}\sapisrv.exe"
  File "${DISTDIR}\gen-keys.ps1"
  File /r "${DISTDIR}\www"

  DetailPrint "Stopping any existing sapisrv service..."
  nsExec::ExecToLog '"$SYSDIR\sc.exe" stop sapisrv'
  nsExec::ExecToLog '"$SYSDIR\sc.exe" delete sapisrv'
  Sleep 500

  DetailPrint "Registering sapisrv service..."
  nsExec::ExecToLog '"$INSTDIR\sapisrv.exe" install'
  Pop $0
  ${If} $0 != "0"
    MessageBox MB_OK|MB_ICONSTOP "sapisrv.exe install failed (exit code $0). The installer will continue, but you'll need to register the service manually."
  ${EndIf}

  DetailPrint "Reserving URL ACL http://+:8080/ ..."
  nsExec::ExecToLog '"$SYSDIR\netsh.exe" http delete urlacl url=http://+:8080/'
  nsExec::ExecToLog '"$SYSDIR\netsh.exe" http add urlacl url=http://+:8080/ user="NT AUTHORITY\NetworkService"'

  DetailPrint "Generating keys.json (no-op if it already exists)..."
  ; %ProgramData% expands to C:\ProgramData on modern Windows. NSIS
  ; doesn't ship a $PROGRAMDATA / $COMMONAPPDATA built-in, so read the
  ; env var directly. gen-keys.ps1 itself uses $env:ProgramData and
  ; creates the dir if it doesn't exist; we mkdir here just to keep the
  ; intent visible.
  ReadEnvStr $R0 "ProgramData"
  CreateDirectory "$R0\sapicli"
  nsExec::ExecToLog 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$INSTDIR\gen-keys.ps1"'

  DetailPrint "Starting sapisrv service..."
  nsExec::ExecToLog '"$SYSDIR\sc.exe" start sapisrv'

  WriteUninstaller "$INSTDIR\uninstall.exe"

  ; Add/Remove Programs entry
  WriteRegStr   HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\sapisrv" "DisplayName"     "sapicli SAPI HTTP Server"
  WriteRegStr   HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\sapisrv" "DisplayVersion"  "${VERSION}"
  WriteRegStr   HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\sapisrv" "Publisher"       "vampi.tech"
  WriteRegStr   HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\sapisrv" "InstallLocation" "$INSTDIR"
  WriteRegStr   HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\sapisrv" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\sapisrv" "NoModify" 1
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\sapisrv" "NoRepair" 1
SectionEnd

; --- Uninstall ---
Section "Uninstall"
  DetailPrint "Stopping + removing sapisrv service..."
  nsExec::ExecToLog '"$SYSDIR\sc.exe" stop sapisrv'
  nsExec::ExecToLog '"$SYSDIR\sc.exe" delete sapisrv'

  DetailPrint "Releasing URL ACL..."
  nsExec::ExecToLog '"$SYSDIR\netsh.exe" http delete urlacl url=http://+:8080/'

  RMDir /r "$INSTDIR\www"
  Delete   "$INSTDIR\sapicli.exe"
  Delete   "$INSTDIR\sapisrv.exe"
  Delete   "$INSTDIR\gen-keys.ps1"
  Delete   "$INSTDIR\uninstall.exe"
  RMDir    "$INSTDIR"

  ; %ProgramData%\sapicli\ intentionally preserved (logs, keys.json).

  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\sapisrv"
SectionEnd
