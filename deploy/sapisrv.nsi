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
;     (binPath derived from INSTDIR at install time, so the install can
;     live anywhere)
;   - reserves the http://+:8080/ URL ACL for that account
;   - generates %ProgramData%\sapicli\keys.json with a random public-trial
;     key on first install (gen-keys.ps1 is idempotent on re-runs)
;   - optional: append INSTDIR to system PATH (default on, opt-out via
;     the Components page) so `sapicli` is callable from any cmd
;   - "Open the sapisrv web UI in my browser" checkbox on the Finish page
;   - uninstaller stops + removes the service, removes the URL ACL,
;     deletes program files, and undoes the PATH addition;
;     %ProgramData%\sapicli\ (logs, keys.json) is preserved
; -----------------------------------------------------------------------------

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "WordFunc.nsh"
!include "WinMessages.nsh"

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

!define HKLM_ENV "SYSTEM\CurrentControlSet\Control\Session Manager\Environment"

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
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Function OpenWebUI
  ExecShell "open" "http://localhost:8080/"
FunctionEnd

; --- Sections ---

; The core install. Mandatory (read-only in the Components UI), since
; everything else (service, ACL, keys.json) depends on the files being
; on disk.
Section "sapicli + sapisrv (required)" SEC_INSTALL
  SectionIn RO
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

; Optional: add INSTDIR to the system-wide PATH. Default on. Idempotent
; (won't add a duplicate if it's already there).
Section "Add to system PATH" SEC_PATH
  DetailPrint "Adding $INSTDIR to system PATH..."
  ReadRegStr $0 HKLM "${HKLM_ENV}" "Path"
  ; Wrap in delimiters so a substring match doesn't false-positive
  ; on dirs that happen to share a suffix with $INSTDIR.
  ${WordFind} ";$0;" ";$INSTDIR;" "E+1{" $1
  ${If} $1 != ""
    DetailPrint "  already present; skipping."
  ${Else}
    ${If} $0 == ""
      StrCpy $0 "$INSTDIR"
    ${Else}
      StrCpy $0 "$0;$INSTDIR"
    ${EndIf}
    WriteRegExpandStr HKLM "${HKLM_ENV}" "Path" "$0"
    ; Tell already-running processes (Explorer, etc.) so new shells they
    ; spawn pick up the change without a reboot. Existing cmd/PS shells
    ; still need to be reopened.
    SendMessage ${HWND_BROADCAST} ${WM_WININICHANGE} 0 "STR:Environment" /TIMEOUT=5000
  ${EndIf}
SectionEnd

LangString DESC_SEC_INSTALL ${LANG_ENGLISH} "Installs sapicli.exe + the sapisrv Windows service. Required."
LangString DESC_SEC_PATH    ${LANG_ENGLISH} "Append the install folder to the system PATH so `sapicli` is callable from any command prompt."

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_INSTALL} $(DESC_SEC_INSTALL)
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_PATH}    $(DESC_SEC_PATH)
!insertmacro MUI_FUNCTION_DESCRIPTION_END

; --- Uninstall ---
Section "Uninstall"
  DetailPrint "Stopping + removing sapisrv service..."
  nsExec::ExecToLog '"$SYSDIR\sc.exe" stop sapisrv'
  nsExec::ExecToLog '"$SYSDIR\sc.exe" delete sapisrv'

  DetailPrint "Releasing URL ACL..."
  nsExec::ExecToLog '"$SYSDIR\netsh.exe" http delete urlacl url=http://+:8080/'

  ; Remove INSTDIR from PATH (whether or not the user opted in at
  ; install time -- always safe; it's a no-op if not present).
  DetailPrint "Removing $INSTDIR from system PATH (if present)..."
  ReadRegStr $0 HKLM "${HKLM_ENV}" "Path"
  ${WordReplace} "$0" ";$INSTDIR" "" "+" $1
  ${If} $1 == "$0"
    ${WordReplace} "$0" "$INSTDIR;" "" "+" $1
  ${EndIf}
  ${If} $1 == "$0"
    ${WordReplace} "$0" "$INSTDIR" "" "+" $1
  ${EndIf}
  ${If} $1 != "$0"
    WriteRegExpandStr HKLM "${HKLM_ENV}" "Path" "$1"
    SendMessage ${HWND_BROADCAST} ${WM_WININICHANGE} 0 "STR:Environment" /TIMEOUT=5000
  ${EndIf}

  RMDir /r "$INSTDIR\www"
  Delete   "$INSTDIR\sapicli.exe"
  Delete   "$INSTDIR\sapisrv.exe"
  Delete   "$INSTDIR\gen-keys.ps1"
  Delete   "$INSTDIR\uninstall.exe"
  RMDir    "$INSTDIR"

  ; %ProgramData%\sapicli\ intentionally preserved (logs, keys.json).

  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\sapisrv"
SectionEnd
