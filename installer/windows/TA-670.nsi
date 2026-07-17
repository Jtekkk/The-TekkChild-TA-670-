; TA-670 — Windows installer (NSIS).
; Installs the CLAP plugin to the standard system-wide CLAP directory
; (C:\Program Files\Common Files\CLAP\Tekkchild) with an uninstaller and an
; Add/Remove Programs entry.
;
; Build:  makensis /DVERSION=x.y.z /DPLUGIN_FILE=<abs path to TA-670.clap> TA-670.nsi

!ifndef VERSION
  !define VERSION "0.1.0"
!endif
!ifndef PLUGIN_FILE
  !define PLUGIN_FILE "..\..\build\Release\TA-670.clap"
!endif

!define PRODUCT "Tekkchild Model 670"
!define REGKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\TekkchildTA670"

Unicode true
Name "${PRODUCT} ${VERSION}"
OutFile "TA-670-Setup-${VERSION}-win64.exe"
RequestExecutionLevel admin
InstallDir "$COMMONFILES64\CLAP\Tekkchild"
SetCompressor /SOLID lzma

Page directory
Page instfiles
UninstPage uninstConfirm
UninstPage instfiles

Section "TA-670 CLAP plugin (required)"
  SectionIn RO
  SetOutPath "$INSTDIR"
  File "${PLUGIN_FILE}"
  WriteUninstaller "$INSTDIR\Uninstall-TA-670.exe"

  WriteRegStr HKLM "${REGKEY}" "DisplayName" "${PRODUCT} (CLAP)"
  WriteRegStr HKLM "${REGKEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${REGKEY}" "Publisher" "Tekkchild"
  WriteRegStr HKLM "${REGKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${REGKEY}" "UninstallString" '"$INSTDIR\Uninstall-TA-670.exe"'
  WriteRegDWORD HKLM "${REGKEY}" "NoModify" 1
  WriteRegDWORD HKLM "${REGKEY}" "NoRepair" 1
SectionEnd

Section "Uninstall"
  Delete "$INSTDIR\TA-670.clap"
  Delete "$INSTDIR\Uninstall-TA-670.exe"
  RMDir "$INSTDIR"
  DeleteRegKey HKLM "${REGKEY}"
SectionEnd
