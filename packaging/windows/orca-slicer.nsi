; OrcaSlicer installer, built by makensis over a tree produced by
; "cmake --install".
;
; One installer covers both install scopes. A tree that a record in the machine
; hive names - which is every install made by the CPack installer this script
; replaces, and every install this script makes for all users - is machine-wide:
; it is replaced in place, and its entry needs an administrator token that this
; program never asks for itself. A tree no machine record names installs under
; the user's LOCALAPPDATA at the user's own level, so an in-app update never
; raises a UAC prompt.
;
; Both scopes stage the new tree beside the target and rename it into place, so
; the old tree stays usable until a complete new one exists. The exit codes the
; updater's helper reads:
;   1  the tree needs an administrator and this run has no token
;   2  nothing was written: the tree is in use, the stage failed, or the old
;      uninstaller could not be run
;   3  the old tree was removed and the new one could not be put in place
;
; Supplied with -D by the caller:
;   STAGE             install tree to package (required)
;   OUTFILE           installer to write (required)
;   APP_VERSION       version shown in Apps & features (required)
;   ORCA_SIGN_CMD     signs one file, "%1" is the file (optional)
;   ORCA_SIGN_SHELL   shell that runs the signing script (optional, with ORCA_SIGN_SCRIPT)
;   ORCA_SIGN_SCRIPT  signing script the shell runs (optional, with ORCA_SIGN_SHELL)

Unicode true

!include "MUI2.nsh"

!define APP_NAME     "OrcaSlicer"
!define APP_EXE      "orca-slicer.exe"
; current_install_flavour() in GUI_Utils.cpp calls a directory that holds this
; file beside the executable a Windows installer install.
!define UNINST_EXE   "uninstall.exe"
!define UNINST_KEY   "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}"
; The CPack-era installer recorded its tree only in this key's default value.
!define LEGACY_KEY   "Software\SoftFever\OrcaSlicer"
!define PER_USER_DIR "$LOCALAPPDATA\Programs\${APP_NAME}"

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

; The per-user tree is the default; .onInit moves a machine-wide install to the
; tree its record names, unless /D= already named the tree.
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
; A caller whose paths may contain spaces names the shell and the script
; separately: quoting a whole command inside the define would break at the
; first space in the path.
!ifndef ORCA_SIGN_CMD
  !ifdef ORCA_SIGN_SCRIPT
    !ifdef ORCA_SIGN_SHELL
      !define ORCA_SIGN_CMD '${ORCA_SIGN_SHELL} -NoProfile -ExecutionPolicy Bypass -File "${ORCA_SIGN_SCRIPT}" -Path'
    !endif
  !endif
!endif
!ifdef ORCA_SIGN_CMD
  !system '${ORCA_SIGN_CMD} "${STAGE}"' = 0
  !uninstfinalize '${ORCA_SIGN_CMD} "%1"' = 0
  !finalize '${ORCA_SIGN_CMD} "%1"' = 0
!endif

; 1 when the chosen tree is an all-users tree: the entry is written to HKLM and
; the shortcuts to the all-users profile.
Var AllUsers

; A record names the tree its uninstaller sits in: $MachineRoot is the root the machine
; hive names, $MachineRoot32 the one the 32-bit view names on its own, $UserRoot the one
; the user hive names, and "" means there is no record. NSIS has no function locals, so
; the strings these helpers need live here.
Var MachineRoot
Var MachineRoot32
Var UserRoot
Var RegRoot
Var RegChar
Var RegLen

; Separators at the end are not part of the tree: "C:\Apps\OrcaSlicer\" and
; "C:\Apps\OrcaSlicer" name one directory. A drive root keeps its own one.
; Tag keeps the labels apart per insertion.
!macro TrimRoot Root Tag
trim_root_${Tag}:
  StrCpy $RegChar ${Root} 1 -1
  StrCmp $RegChar "\" 0 trim_root_done_${Tag}
  StrLen $RegLen ${Root}
  IntCmp $RegLen 3 trim_root_done_${Tag} trim_root_done_${Tag}
  IntOp $RegLen $RegLen - 1
  StrCpy ${Root} ${Root} $RegLen
  Goto trim_root_${Tag}
trim_root_done_${Tag}:
!macroend

; The tree one uninstall record names, read in the current registry view: the
; InstallLocation value, else the default value the CPack-era installer wrote,
; else UninstallString with the quotes and the trailing \uninstall.exe removed.
; Only a whole directory is ever compared with another, never a prefix.
!macro ReadRecordRootView Hive Out Tag
  ReadRegStr $RegRoot ${Hive} "${UNINST_KEY}" "InstallLocation"
  StrCmp $RegRoot "" 0 read_root_trim_${Tag}
  ReadRegStr $RegRoot ${Hive} "${LEGACY_KEY}" ""
  StrCmp $RegRoot "" 0 read_root_trim_${Tag}
  ReadRegStr $RegRoot ${Hive} "${UNINST_KEY}" "UninstallString"
  StrCmp $RegRoot "" read_root_view_done_${Tag}

  ; UninstallString names the uninstaller, quoted by this installer.
  StrCpy $RegChar $RegRoot 1
  StrCmp $RegChar '"' 0 read_root_suffix_${Tag}
  StrCpy $RegChar $RegRoot 1 -1
  StrCmp $RegChar '"' 0 read_root_suffix_${Tag}
  StrLen $RegLen $RegRoot
  IntOp $RegLen $RegLen - 2
  StrCpy $RegRoot $RegRoot $RegLen 1

read_root_suffix_${Tag}:
  StrLen $RegLen $RegRoot
  IntCmp $RegLen 14 read_root_trim_${Tag} read_root_trim_${Tag}
  StrCpy $RegChar $RegRoot 14 -14
  StrCmp $RegChar "\uninstall.exe" 0 read_root_trim_${Tag}
  IntOp $RegLen $RegLen - 14
  StrCpy $RegRoot $RegRoot $RegLen

read_root_trim_${Tag}:
  !insertmacro TrimRoot $RegRoot ${Tag}t
  StrCmp $RegRoot "" read_root_view_done_${Tag}
  StrCpy ${Out} $RegRoot
read_root_view_done_${Tag}:
!macroend

; The record is asked for in both views, because installers built before this
; script were 32-bit and wrote under WOW6432Node. The view is left at 64, which
; is where this script writes. Tag keeps the labels apart per insertion.
!macro ReadRecordRoot Hive Out Tag
  StrCpy ${Out} ""
  SetRegView 64
  !insertmacro ReadRecordRootView ${Hive} ${Out} ${Tag}64
  StrCmp ${Out} "" 0 read_root_have_${Tag}
  SetRegView 32
  !insertmacro ReadRecordRootView ${Hive} ${Out} ${Tag}32
read_root_have_${Tag}:
  SetRegView 64
!macroend

; $MachineRoot is the tree a machine-wide record names, "" when there is none.
Function GetMachineRoot
  !insertmacro ReadRecordRoot HKLM $MachineRoot machine
FunctionEnd

; A CPack-era install recorded its tree only in LEGACY_KEY's default value,
; which every later install reads as a machine record and would retarget to.
; The value is this tree's when it names $INSTDIR exactly; one that names
; another tree belongs to that install and is left alone. Called once per
; registry view, because that installer was 32-bit and wrote under WOW6432Node.
!macro DeleteLegacyRoot Hive Tag
  ReadRegStr $RegRoot ${Hive} "${LEGACY_KEY}" ""
  StrCmp $RegRoot "" delete_legacy_done_${Tag}
  !insertmacro TrimRoot $RegRoot ${Tag}t
  StrCmp $RegRoot $INSTDIR 0 delete_legacy_done_${Tag}
  DeleteRegValue ${Hive} "${LEGACY_KEY}" ""
delete_legacy_done_${Tag}:
!macroend

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

  ; A tree that a record in the machine hive names is machine-wide. The record
  ; does not say the tree is protected: an administrator can install into a
  ; folder this user can write, and that tree is replaced in place.
  Call GetMachineRoot
  !insertmacro TrimRoot $INSTDIR init
  StrCmp $MachineRoot "" oninit_per_user
  ; Without /D= the tree is still the per-user default, so take the recorded
  ; one. A tree /D= named is used as given.
  StrCmp $INSTDIR "${PER_USER_DIR}" 0 oninit_scope_known
  StrCpy $INSTDIR $MachineRoot
oninit_scope_known:
  StrCmp $INSTDIR $MachineRoot oninit_machine oninit_per_user

oninit_machine:
  StrCpy $AllUsers 1
  Call IsElevated
  StrCmp $0 "1" oninit_scope_done
  MessageBox MB_OK|MB_ICONSTOP \
    "OrcaSlicer is installed for all users in $INSTDIR, and replacing it needs administrator rights.$\r$\n$\r$\nRun this installer as an administrator, or start the update from OrcaSlicer." \
    /SD IDOK
  ; A silent update reads the exit code: without this the helper would take the
  ; abort for a successful install.
  SetErrorLevel 1
  Abort

oninit_per_user:
oninit_scope_done:
  Call ApplyScopeContext
FunctionEnd

; $SMPROGRAMS and $DESKTOP resolve per shell context, so the context follows the
; scope: the user's own for a per-user tree, the all-users profile for a
; machine-wide one.
Function ApplyScopeContext
  SetShellVarContext current
  StrCmp $AllUsers "0" apply_scope_done
  SetShellVarContext all
apply_scope_done:
FunctionEnd

; The directory page changes $INSTDIR after .onInit resolved the scope, so the
; scope is decided again for whatever tree is chosen there. A folder no machine
; record names is a per-user install: its entry belongs in HKCU, and rewriting
; the recorded tree's entry from here would put one tree's record on another.
; The record is read here as well, because this callback can run before .onInit.
Function .onVerifyInstDir
  StrCpy $AllUsers 0
  Call GetMachineRoot
  !insertmacro TrimRoot $INSTDIR verify
  StrCmp $MachineRoot "" verify_scope_done
  StrCmp $INSTDIR $MachineRoot 0 verify_scope_done
  StrCpy $AllUsers 1
verify_scope_done:
  Call ApplyScopeContext
FunctionEnd

Section "!${APP_NAME}"
  ; An installer that starts while OrcaSlicer runs from $INSTDIR would remove
  ; the tree under a live process, and the old uninstaller cannot delete a
  ; mapped image, so the tree would be left half removed. Windows refuses to
  ; rename a mapped image, so the probe is a rename aside and back. It closes
  ; the window between the helper's own process check and the UAC consent.
  IfFileExists "$INSTDIR\${APP_EXE}" 0 probe_done
  ClearErrors
  Rename "$INSTDIR\${APP_EXE}" "$INSTDIR\${APP_EXE}.running-probe"
  IfErrors app_running
  ClearErrors
  Rename "$INSTDIR\${APP_EXE}.running-probe" "$INSTDIR\${APP_EXE}"
  IfErrors probe_stuck
probe_done:

  ; The new tree is staged beside the target and renamed into place, so the old
  ; one stays as it is until a complete new one exists. A stage left behind by
  ; an earlier failed run is removed first, and one that cannot be cleared stops
  ; the install here rather than merging with this payload.
  RMDir /r "$INSTDIR.new"
  IfFileExists "$INSTDIR.new\*.*" install_failed
  SetOutPath "$INSTDIR.new"
  ClearErrors
  File /r "${STAGE}\*.*"
  IfErrors install_failed

  ; Written into the stage rather than into the final tree: a failure here
  ; happens while the old tree is intact, so it is a "nothing was written"
  ; failure (exit code 2) rather than one that lands after the swap.
  WriteUninstaller "$INSTDIR.new\${UNINST_EXE}"
  IfErrors install_failed
  IfFileExists "$INSTDIR.new\${APP_EXE}" 0 install_failed

  IfFileExists "$INSTDIR\${UNINST_EXE}" upgrade fresh_install

fresh_install:
  ; Nothing is installed here, so there is nothing to remove. RMDir only fails
  ; when the chosen folder already holds something, and the rename then fails
  ; too, which leaves the folder as it was and the stage unused.
  RMDir "$INSTDIR"
  ; SetOutPath made the stage the working directory, and Windows refuses to
  ; rename a directory the process sits in. Leave it before either rename.
  SetOutPath "$TEMP"
  ; The rename itself is the test: a folder that already held an executable
  ; would otherwise satisfy the check below and report a successful install
  ; while the stage was never moved.
  ClearErrors
  Rename "$INSTDIR.new" "$INSTDIR"
  IfErrors install_failed
  IfFileExists "$INSTDIR.new\*.*" install_failed
  IfFileExists "$INSTDIR\${APP_EXE}" 0 install_failed
  Goto installed

upgrade:
  ; The old uninstaller removes the tree it sits in. _?= runs it in place
  ; instead of copying it to $TEMP, which also means it cannot delete its own
  ; file: that one is removed here, or the directory survives and the rename
  ; below always fails.
  ClearErrors
  ExecWait '"$INSTDIR\${UNINST_EXE}" /S _?=$INSTDIR' $0
  ; A process that cannot be launched leaves the error flag set and $0 with no
  ; exit code, and SetErrorLevel would take that empty $0 for success.
  IfErrors old_unlaunchable
  StrCmp $0 "0" 0 old_failed
  Delete "$INSTDIR\${UNINST_EXE}"
  ; Anything still there would make the rename fail, and says so instead.
  RMDir /r "$INSTDIR"
  IfFileExists "$INSTDIR\*.*" swap_failed

swap:
  SetOutPath "$TEMP"
  Rename "$INSTDIR.new" "$INSTDIR"
  IfFileExists "$INSTDIR\${APP_EXE}" 0 swap_failed

installed:
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

  ; This install has just replaced the tree it owns, so the CPack-era value
  ; that named it is this tree's record too. Removing it keeps the next
  ; uninstall and the next double-click from reading it as a machine record.
  SetRegView 64
  !insertmacro DeleteLegacyRoot SHCTX install64
  SetRegView 32
  !insertmacro DeleteLegacyRoot SHCTX install32
  SetRegView 64
  Goto section_done

app_running:
  ; Abort on its own reports 1, which reads as a missing token; 2 says nothing
  ; was written. A silent run shows no message, so the exit code is the report.
  SetErrorLevel 2
  MessageBox MB_OK|MB_ICONSTOP \
    "OrcaSlicer is running from $INSTDIR.$\r$\n$\r$\nClose OrcaSlicer, then run this installer again." \
    /SD IDOK
  Abort

probe_stuck:
  ; The probe could not put the executable back, so the tree is not in the
  ; state the failure messages claim. Name the file that has to be renamed.
  SetErrorLevel 2
  MessageBox MB_OK|MB_ICONSTOP \
    "OrcaSlicer could not be put back after the running check.$\r$\n$\r$\nRename $INSTDIR\${APP_EXE}.running-probe back to ${APP_EXE}, then run this installer again." \
    /SD IDOK
  Abort

old_failed:
  MessageBox MB_OK|MB_ICONSTOP \
    "The installed version could not be removed (exit code $0), so OrcaSlicer was not updated." \
    /SD IDOK
  SetErrorLevel $0
  Abort

old_unlaunchable:
  ; 2 is the error level of a failed write; 1 is what Abort would report.
  SetErrorLevel 2
  MessageBox MB_OK|MB_ICONSTOP \
    "The installed version could not be removed, so nothing was changed." \
    /SD IDOK
  Abort

install_failed:
  ; The stage never became the install, so the tree in $INSTDIR, if there is
  ; one, was not touched.
  SetErrorLevel 2
  MessageBox MB_OK|MB_ICONSTOP \
    "OrcaSlicer could not be written to $INSTDIR.$\r$\n$\r$\nNothing was changed; an installed version, if any, is still there." \
    /SD IDOK
  Abort

swap_failed:
  ; The old tree is gone by now: this is the only failure that needs the user
  ; to install from the release page.
  SetErrorLevel 3
  MessageBox MB_OK|MB_ICONSTOP \
    "The previous version was removed, but OrcaSlicer could not be put in place at $INSTDIR.$\r$\n$\r$\nInstall OrcaSlicer again from the release page." \
    /SD IDOK
  Abort

section_done:
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

; The same roots for uninstaller code, which cannot call installer functions.
Function un.GetMachineRoot
  !insertmacro ReadRecordRoot HKLM $MachineRoot un_machine
FunctionEnd

; The tree the user hive records, so the per-user entry is deleted only when it belongs
; to the tree being removed.
Function un.GetUserRoot
  !insertmacro ReadRecordRoot HKCU $UserRoot un_user
FunctionEnd

; The tree the 32-bit view records on its own, so the uninstaller deletes that view's
; leftover key only when it belongs to the tree being removed.
Function un.GetMachineRoot32
  StrCpy $MachineRoot32 ""
  SetRegView 32
  !insertmacro ReadRecordRootView HKLM $MachineRoot32 un_machine32
  SetRegView 64
FunctionEnd

Function un.onInit
  ; $INSTDIR in an uninstaller is the directory the uninstaller sits in, so the
  ; tree is known; the records say which scope wrote it.
  SetShellVarContext current
  StrCpy $AllUsers 0
  !insertmacro TrimRoot $INSTDIR un
  Call un.GetMachineRoot
  Call un.GetMachineRoot32
  Call un.GetUserRoot

  ; Only a record that names this tree makes it all-users. A record naming another tree
  ; says nothing about this one: taking it for all-users would delete that install's
  ; shortcuts and Apps & features entry while removing this tree.
  StrCmp $MachineRoot $INSTDIR un_scope_all
  Goto un_scope_done

un_scope_all:
  StrCpy $AllUsers 1
  SetShellVarContext all

  ; An all-users tree needs administrator rights and this program carries the
  ; installer's as-invoker manifest, so an unelevated run hands over to an
  ; elevated copy of itself. $INSTDIR is the tree this copy sits in.
  Call un.IsElevated
  StrCmp $0 "1" un_scope_done
  IfSilent un_needs_admin
  ClearErrors
  ExecShell "runas" "$INSTDIR\${UNINST_EXE}" "" SW_SHOWNORMAL
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
  ; The tree holds nothing but what this installer wrote.
  RMDir /r "$INSTDIR"

  ; A record this tree does not own says nothing about it: deleting that
  ; install's entry, its shortcuts or its Start-menu folder while removing a
  ; copied or unrecorded tree would strip a live install. Only the branch whose
  ; record names $INSTDIR removes anything outside the tree, and only the set
  ; belonging to the scope that record was read in.
  StrCmp $AllUsers "0" uninstall_user

  DeleteRegKey HKLM "${UNINST_KEY}"
  ; The CPack-era installer wrote its tree only into the legacy value, in the
  ; 32-bit view; a value that names this tree is this tree's record.
  SetRegView 64
  !insertmacro DeleteLegacyRoot HKLM un_all64
  ; Older installers also wrote this tree's record into the 32-bit view; remove
  ; that key only when it names this tree.
  SetRegView 32
  StrCmp $MachineRoot32 $INSTDIR 0 un_all_view_done
  DeleteRegKey HKLM "${UNINST_KEY}"
un_all_view_done:
  !insertmacro DeleteLegacyRoot HKLM un_all32
  SetRegView 64

  Delete "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk"
  Delete "$SMPROGRAMS\${APP_NAME}\Uninstall ${APP_NAME}.lnk"
  RMDir  "$SMPROGRAMS\${APP_NAME}"
  Delete "$DESKTOP\${APP_NAME}.lnk"
  Goto uninstall_done

uninstall_user:
  ; Same rule for the user entry, so a copied or unrecorded tree cannot strip a
  ; live per-user install's entry or shortcuts.
  StrCmp $UserRoot $INSTDIR 0 uninstall_done
  DeleteRegKey HKCU "${UNINST_KEY}"
  ; HKCU\Software is not redirected, so one read covers both registry views.
  !insertmacro DeleteLegacyRoot HKCU un_user_rec

  Delete "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk"
  Delete "$SMPROGRAMS\${APP_NAME}\Uninstall ${APP_NAME}.lnk"
  RMDir  "$SMPROGRAMS\${APP_NAME}"
  Delete "$DESKTOP\${APP_NAME}.lnk"
uninstall_done:
SectionEnd
