; Invoked by scripts/package-windows.py after deployment and validation.
Unicode true
!include "LogicLib.nsh"
!include "x64.nsh"
!ifndef MANIFEST
 !error "Use scripts/package-windows.py (a validated payload manifest is required)"
!endif
!ifndef VERSION
 !error "Missing VERSION"
!endif
!ifndef ARCH
 !error "Missing ARCH"
!endif
!ifndef OUTPUT
 !error "Missing OUTPUT"
!endif
!include "${MANIFEST}"
!ifdef SIGN_UNINSTALLER
 !uninstfinalize '${SIGN_UNINSTALLER} "%1"'
!endif
Name "OSMClient"
OutFile "${OUTPUT}"
!if "${ARCH}" == "amd64"
 InstallDir "$PROGRAMFILES64\OSMClient"
!else
 InstallDir "$PROGRAMFILES\OSMClient"
!endif
InstallDirRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\OSMClient" "InstallLocation"
RequestExecutionLevel admin
AutoCloseWindow true
SetCompressor /SOLID lzma
VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "OSMClient"
VIAddVersionKey "FileDescription" "OSMClient Setup"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "FIT2CLOUD"
Page directory
Page instfiles
UninstPage uninstConfirm
UninstPage instfiles

Function .onInit
 SetShellVarContext all
 SetRegView 32
 !if "${ARCH}" == "amd64"
  ${IfNot} ${RunningX64}
   MessageBox MB_ICONSTOP "This package requires 64-bit Windows. Use the 386 package for 32-bit Windows."
   Abort
  ${EndIf}
 !endif
 ReadRegStr $0 HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\OSMClientQt" "InstallLocation"
 ${If} $0 != ""
  MessageBox MB_ICONSTOP "Uninstall the previous per-user OSMClient before installing this version."
  Abort
 ${EndIf}
 ReadRegStr $0 HKLM "Software\FIT2CLOUD\OSMClient" "WindowsInstaller"
 ${If} $0 != ""
  MessageBox MB_ICONSTOP "An MSI installation exists. Uninstall it before installing the EXE package."
  Abort
 ${EndIf}
 ReadRegStr $0 HKCU "Software\FIT2CLOUD\OSMClientQt" "WindowsInstaller"
 ${If} $0 != ""
  MessageBox MB_ICONSTOP "An MSI installation exists. Uninstall it from Windows Settings before installing the EXE package."
  Abort
 ${EndIf}
FunctionEnd

Section "Install"
 ; The previous installer removes its own files and old shortcut name on upgrade.
 ; Keep the registry/install identity stable while the display name changes.
 ReadRegStr $0 HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\OSMClient" "InstallLocation"
 ${If} $0 == ""
  ${If} ${FileExists} "$INSTDIR\*.*"
   Abort "The selected installation directory already exists. Choose an empty directory."
  ${EndIf}
 ${EndIf}
 ${If} $0 != ""
  ${If} ${FileExists} "$0\Uninstall.exe"
   ExecWait '$\"$0\Uninstall.exe$\" /S _?=$0' $1
   ${If} $1 != 0
    Abort "The previous version could not be uninstalled. Close the client and retry."
   ${EndIf}
  ${EndIf}
 ${EndIf}
 !insertmacro InstallPayload
 WriteUninstaller "$INSTDIR\Uninstall.exe"
 CreateShortcut "$SMPROGRAMS\OSMClient.lnk" "$INSTDIR\client.exe"
 CreateShortcut "$DESKTOP\OSMClient.lnk" "$INSTDIR\client.exe"
 WriteRegStr HKLM "Software\Classes\jms" "" "URL:JumpServer connection"
 WriteRegStr HKLM "Software\Classes\jms" "URL Protocol" ""
 WriteRegStr HKLM "Software\Classes\jms\DefaultIcon" "" '$\"$INSTDIR\client.exe$\",0'
 WriteRegStr HKLM "Software\Classes\jms\shell\open\command" "" '$\"$INSTDIR\client.exe$\" $\"%1$\"'
 WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\OSMClient" "DisplayName" "OSMClient"
 WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\OSMClient" "DisplayVersion" "${VERSION}"
 WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\OSMClient" "Publisher" "FIT2CLOUD"
 WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\OSMClient" "InstallLocation" "$INSTDIR"
 WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\OSMClient" "DisplayIcon" "$INSTDIR\client.exe"
 WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\OSMClient" "UninstallString" '$\"$INSTDIR\Uninstall.exe$\"'
 WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\OSMClient" "QuietUninstallString" '$\"$INSTDIR\Uninstall.exe$\" /S'
 WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\OSMClient" "NoModify" 1
 WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\OSMClient" "NoRepair" 1
SectionEnd

Section "Uninstall"
 SetShellVarContext all
 SetRegView 32
 ReadRegStr $0 HKLM "Software\Classes\jms\shell\open\command" ""
 ${If} $0 == '$\"$INSTDIR\client.exe$\" $\"%1$\"'
  DeleteRegKey HKLM "Software\Classes\jms"
 ${EndIf}
 !insertmacro RemovePayload
 Delete "$INSTDIR\Uninstall.exe"
 RMDir "$INSTDIR"
 DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\OSMClient"
 Delete "$SMPROGRAMS\OSMClient.lnk"
 Delete "$DESKTOP\OSMClient.lnk"
SectionEnd
