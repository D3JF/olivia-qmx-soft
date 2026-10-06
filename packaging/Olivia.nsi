!define APP_NAME "Olivia QMX+"
!define APP_VERSION "0.1.1"
!define APP_DIR "..\olivia-gui-windows-x64"

Name "${APP_NAME}"
OutFile "..\Olivia-QMX+-${APP_VERSION}-win64-setup.exe"
Icon "..\resources\olivia.ico"
InstallDir "$PROGRAMFILES64\${APP_NAME}"
RequestExecutionLevel admin

Page directory
Page instfiles
UninstPage uninstConfirm
UninstPage instfiles

Section
  SetOutPath "$INSTDIR"
  File /r "${APP_DIR}\*.*"
  WriteUninstaller "$INSTDIR\uninstall.exe"
  CreateDirectory "$SMPROGRAMS\${APP_NAME}"
  CreateShortCut "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk" \
    "$INSTDIR\olivia_gui.exe" "" "$INSTDIR\olivia_gui.exe" 0
  CreateShortCut "$DESKTOP\${APP_NAME}.lnk" \
    "$INSTDIR\olivia_gui.exe" "" "$INSTDIR\olivia_gui.exe" 0
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}" \
    "DisplayName" "${APP_NAME}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}" \
    "UninstallString" "$INSTDIR\uninstall.exe"
SectionEnd

Section "Uninstall"
  Delete "$DESKTOP\${APP_NAME}.lnk"
  Delete "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk"
  RMDir "$SMPROGRAMS\${APP_NAME}"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}"
  RMDir /r "$INSTDIR"
SectionEnd
