; OrcaSlicer installer, built by makensis over a tree produced by
; "cmake --install".
;
; One installer covers both install scopes. An install that recorded itself in
; the HKLM uninstall key - which is every install made by the CPack installer
; this script replaces - is machine-wide: it is replaced under Program Files,
; which needs an administrator token, and this program never asks for one
; itself. Anything else installs under the user's LOCALAPPDATA at the user's
; own level, so an in-app update never raises a UAC prompt.
;
; Supplied with -D by the caller:
;   STAGE          install tree to package (required)
;   OUTFILE        installer to write (required)
;   APP_VERSION    version shown in Apps & features (required)
;   ORCA_SIGN_CMD  signs one file, "%1" is the file (optional)

Unicode true

!include "MUI2.nsh"

!define APP_NAME     "OrcaSlicer"
!define APP_EXE      "orca-slicer.exe"
; current_install_flavour() in GUI_Utils.cpp calls a directory that holds this
; file beside the executable a Windows installer install.
!define UNINST_EXE   "uninstall.exe"
!define UNINST_KEY   "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}"
!define PER_USER_DIR "$LOCALAPPDATA\Programs\${APP_NAME}"
!define MACHINE_DIR  "$PROGRAMFILES64\${APP_NAME}"

!ifndef STAGE
  !error "STAGE is not set: pass -DSTAGE=<install tree>."
!endif
!ifndef OUTFILE
  !error "OUTFILE is not set: pass -DOUTFILE=<installer to write>."
!endif
!ifndef APP_VERSION
  !error "APP_VERSION is not set: pass -DAPP_VERSION=<version>."
!endif

Name "${APP_NAME}"
OutFile "${OUTFILE}"
SetCompressor lzma

; As invoker. A per-user install must not prompt, and the machine-wide path
; refuses to run without a token it was started with rather than asking.
RequestExecutionLevel user

; The per-user tree is the default; .onInit moves a machine-wide install to
; MACHINE_DIR, unless /D= already named the tree.
InstallDir "${PER_USER_DIR}"

!define MUI_ICON   "${__FILEDIR__}\..\..\resources\images\OrcaSlicer.ico"
!define MUI_UNICON "${MUI_ICON}"
!define MUI_ABORTWARNING

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${__FILEDIR__}\..\..\LICENSE.txt"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

; The payload is signed before it is compressed, the uninstaller before it is
; written, and the installer last. Callers that build unsigned pass no command.
!ifdef ORCA_SIGN_CMD
  !system '${ORCA_SIGN_CMD} "${STAGE}"' = 0
  !uninstfinalize '${ORCA_SIGN_CMD} "%1"' = 0
  !finalize '${ORCA_SIGN_CMD} "%1"' = 0
!endif

Var AllUsers

; $0 is 1 when this process holds an administrator token. $WINDIR is writable
; by administrators only, so a write there is the answer.
Function IsElevated
  StrCpy $0 0
  ClearErrors
  FileOpen $9 "$WINDIR\${APP_NAME}-elevation-test" w
  IfErrors is_elevated_done
  FileClose $9
  Delete "$WINDIR\${APP_NAME}-elevation-test"
  StrCpy $0 1
is_elevated_done:
FunctionEnd

Function .onInit
  StrCpy $AllUsers 0

  ; The machine key is asked for in both views: installers built before this
  ; script were 32-bit and recorded themselves under WOW6432Node.
  SetRegView 64
  ReadRegStr $0 HKLM "${UNINST_KEY}" "UninstallString"
  StrCmp $0 "" oninit_32bit machine
oninit_32bit:
  SetRegView 32
  ReadRegStr $0 HKLM "${UNINST_KEY}" "UninstallString"
  SetRegView 64
  StrCmp $0 "" per_user machine

machine:
  StrCpy $AllUsers 1
  ; Without /D= the tree is still the per-user default, so take the machine one.
  StrCmp $INSTDIR "${PER_USER_DIR}" machine_has_dir
  StrCpy $INSTDIR "${MACHINE_DIR}"
machine_has_dir:
  Call IsElevated
  StrCmp $0 "1" scope_done
  MessageBox MB_OK|MB_ICONSTOP \
    "OrcaSlicer is installed for all users in ${MACHINE_DIR}, and replacing it needs administrator rights.$\r$\n$\r$\nRun this installer as an administrator, or start the update from OrcaSlicer." \
    /SD IDOK
  ; A silent update reads the exit code: without this the helper would take the
  ; abort for a successful install.
  SetErrorLevel 1
  Abort

per_user:
scope_done:
  SetShellVarContext current
  StrCmp $AllUsers "0" context_done
  SetShellVarContext all
context_done:
FunctionEnd

Section "!${APP_NAME}"
  ; Any earlier install is uninstalled first so that no file from the old
  ; version survives. _?= runs that uninstaller in place instead of copying it
  ; to $TEMP, and the updater's helper has already waited for the running
  ; application to exit.
  IfFileExists "$INSTDIR\${UNINST_EXE}" old_install nothing_to_remove

old_install:
  ExecWait '"$INSTDIR\${UNINST_EXE}" /S _?=$INSTDIR' $0
  StrCmp $0 "0" nothing_to_remove
  MessageBox MB_OK|MB_ICONSTOP \
    "The installed version could not be removed (exit code $0), so nothing was changed." \
    /SD IDOK
  SetErrorLevel $0
  Abort

nothing_to_remove:
  SetOutPath "$INSTDIR"
  File /r "${STAGE}\*.*"
  WriteUninstaller "$INSTDIR\${UNINST_EXE}"

  CreateDirectory "$SMPROGRAMS\${APP_NAME}"
  CreateShortCut "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}"
  CreateShortCut "$SMPROGRAMS\${APP_NAME}\Uninstall ${APP_NAME}.lnk" "$INSTDIR\${UNINST_EXE}"
  CreateShortCut "$DESKTOP\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}"

  ; Apps & features entry, in the hive of the scope: HKCU for a per-user
  ; install, HKLM for a machine-wide one. The key name is what makes an
  ; upgrade replace the entry instead of adding a second one.
  WriteRegStr   SHCTX "${UNINST_KEY}" "DisplayName"         "${APP_NAME}"
  WriteRegStr   SHCTX "${UNINST_KEY}" "DisplayVersion"      "${APP_VERSION}"
  WriteRegStr   SHCTX "${UNINST_KEY}" "DisplayIcon"         "$INSTDIR\${APP_EXE}"
  WriteRegStr   SHCTX "${UNINST_KEY}" "Publisher"           "SoftFever"
  WriteRegStr   SHCTX "${UNINST_KEY}" "HelpLink"            "https://github.com/OrcaSlicer/OrcaSlicer"
  WriteRegStr   SHCTX "${UNINST_KEY}" "URLInfoAbout"        "https://github.com/OrcaSlicer/OrcaSlicer"
  WriteRegStr   SHCTX "${UNINST_KEY}" "InstallLocation"     "$INSTDIR"
  WriteRegStr   SHCTX "${UNINST_KEY}" "UninstallString"     '"$INSTDIR\${UNINST_EXE}"'
  WriteRegStr   SHCTX "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\${UNINST_EXE}" /S'
  WriteRegDWORD SHCTX "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD SHCTX "${UNINST_KEY}" "NoRepair" 1
SectionEnd

; IsElevated for uninstaller code, which cannot call installer functions.
Function un.IsElevated
  StrCpy $0 0
  ClearErrors
  FileOpen $9 "$WINDIR\${APP_NAME}-uninstall-elevation-test" w
  IfErrors un_is_elevated_done
  FileClose $9
  Delete "$WINDIR\${APP_NAME}-uninstall-elevation-test"
  StrCpy $0 1
un_is_elevated_done:
FunctionEnd

Function un.onInit
  ; $INSTDIR in an uninstaller is the directory the uninstaller sits in, so the
  ; tree is known; the ARP entry says which scope the shortcuts are in.
  SetRegView 64
  SetShellVarContext current
  ClearErrors
  ReadRegStr $0 HKLM "${UNINST_KEY}" "UninstallString"
  IfErrors un_scope_done
  SetShellVarContext all

  ; An all-users tree needs administrator rights and this program carries the
  ; installer's as-invoker manifest, so an unelevated run hands over to an
  ; elevated copy of itself. InstallLocation names the tree to hand over.
  Call un.IsElevated
  StrCmp $0 "1" un_scope_done
  ReadRegStr $1 HKLM "${UNINST_KEY}" "InstallLocation"
  StrCmp $1 "" un_needs_admin
  IfSilent un_needs_admin
  ClearErrors
  ExecShell "runas" "$1\${UNINST_EXE}" "" SW_SHOWNORMAL
  IfErrors un_needs_admin
  Quit

un_needs_admin:
  SetErrorLevel 1
  MessageBox MB_OK|MB_ICONSTOP \
    "Uninstalling the all-users install needs administrator rights.$\r$\n$\r$\nRun uninstall.exe in the OrcaSlicer folder as an administrator." \
    /SD IDOK
  Quit

un_scope_done:
FunctionEnd

Section "Uninstall"
  Delete "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk"
  Delete "$SMPROGRAMS\${APP_NAME}\Uninstall ${APP_NAME}.lnk"
  RMDir  "$SMPROGRAMS\${APP_NAME}"
  Delete "$DESKTOP\${APP_NAME}.lnk"

  ; The tree holds nothing but what this installer wrote.
  RMDir /r "$INSTDIR"

  DeleteRegKey SHCTX "${UNINST_KEY}"
  ; Older installers were 32-bit and left their key in the other view.
  SetRegView 32
  DeleteRegKey HKLM "${UNINST_KEY}"
SectionEnd
