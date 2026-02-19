; POSE64 NSIS Installer Script
; Nullsoft Scriptable Install System — just like Winamp used to make

!include "MUI2.nsh"

; ---- General ----
Name "POSE64"
OutFile "..\build-win\pose64-0.9.0-setup.exe"
InstallDir "$PROGRAMFILES64\POSE64"
InstallDirRegKey HKLM "Software\POSE64" "InstallDir"
RequestExecutionLevel admin

; ---- Icon ----
!define MUI_ICON "..\data\pose64.ico"
!define MUI_UNICON "..\data\pose64.ico"

; ---- Branding ----
BrandingText "POSE64 — Palm OS Emulator"

; ---- Interface Settings ----
!define MUI_ABORTWARNING

; ---- Pages ----
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "..\LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

; ---- Uninstaller pages ----
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

; ---- Language ----
!insertmacro MUI_LANGUAGE "English"

; ---- Installer Section ----
Section "POSE64 (required)" SecMain
    SectionIn RO

    SetOutPath "$INSTDIR"

    ; Main executable
    File "..\build-win\pose64.exe"

    ; Qt and MinGW runtime DLLs
    File "..\build-win\Qt6Core.dll"
    File "..\build-win\Qt6Gui.dll"
    File "..\build-win\Qt6Network.dll"
    File "..\build-win\Qt6Svg.dll"
    File "..\build-win\Qt6Widgets.dll"
    File "..\build-win\opengl32sw.dll"
    File "..\build-win\D3Dcompiler_47.dll"
    File "..\build-win\libgcc_s_seh-1.dll"
    File "..\build-win\libstdc++-6.dll"
    File "..\build-win\libwinpthread-1.dll"

    ; Qt plugins — platforms
    SetOutPath "$INSTDIR\platforms"
    File "..\build-win\platforms\qwindows.dll"

    ; Qt plugins — styles
    SetOutPath "$INSTDIR\styles"
    File "..\build-win\styles\qmodernwindowsstyle.dll"

    ; Qt plugins — imageformats
    SetOutPath "$INSTDIR\imageformats"
    File "..\build-win\imageformats\qgif.dll"
    File "..\build-win\imageformats\qico.dll"
    File "..\build-win\imageformats\qjpeg.dll"
    File "..\build-win\imageformats\qsvg.dll"

    ; Qt plugins — iconengines
    SetOutPath "$INSTDIR\iconengines"
    File "..\build-win\iconengines\qsvgicon.dll"

    ; Qt plugins — generic
    SetOutPath "$INSTDIR\generic"
    File "..\build-win\generic\qtuiotouchplugin.dll"

    ; Qt plugins — tls
    SetOutPath "$INSTDIR\tls"
    File "..\build-win\tls\qcertonlybackend.dll"
    File "..\build-win\tls\qschannelbackend.dll"

    ; Qt plugins — networkinformation
    SetOutPath "$INSTDIR\networkinformation"
    File "..\build-win\networkinformation\qnetworklistmanager.dll"

    ; Skins
    SetOutPath "$INSTDIR\Skins"
    File /r "..\build-win\Skins\*.*"

    ; Back to INSTDIR for shortcuts
    SetOutPath "$INSTDIR"

    ; Registry keys for uninstall
    WriteRegStr HKLM "Software\POSE64" "InstallDir" "$INSTDIR"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\POSE64" \
        "DisplayName" "POSE64 — Palm OS Emulator"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\POSE64" \
        "UninstallString" '"$INSTDIR\uninstall.exe"'
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\POSE64" \
        "DisplayIcon" '"$INSTDIR\pose64.exe"'
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\POSE64" \
        "Publisher" "VibeKoder"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\POSE64" \
        "DisplayVersion" "0.9.0"
    WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\POSE64" \
        "NoModify" 1
    WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\POSE64" \
        "NoRepair" 1

    ; Create uninstaller
    WriteUninstaller "$INSTDIR\uninstall.exe"

    ; Start menu shortcuts
    CreateDirectory "$SMPROGRAMS\POSE64"
    CreateShortcut "$SMPROGRAMS\POSE64\POSE64.lnk" "$INSTDIR\pose64.exe" "" "$INSTDIR\pose64.exe" 0
    CreateShortcut "$SMPROGRAMS\POSE64\Uninstall POSE64.lnk" "$INSTDIR\uninstall.exe"

    ; Desktop shortcut
    CreateShortcut "$DESKTOP\POSE64.lnk" "$INSTDIR\pose64.exe" "" "$INSTDIR\pose64.exe" 0

SectionEnd

; ---- Uninstaller Section ----
Section "Uninstall"

    ; Remove files
    Delete "$INSTDIR\pose64.exe"
    Delete "$INSTDIR\Qt6Core.dll"
    Delete "$INSTDIR\Qt6Gui.dll"
    Delete "$INSTDIR\Qt6Network.dll"
    Delete "$INSTDIR\Qt6Svg.dll"
    Delete "$INSTDIR\Qt6Widgets.dll"
    Delete "$INSTDIR\opengl32sw.dll"
    Delete "$INSTDIR\D3Dcompiler_47.dll"
    Delete "$INSTDIR\libgcc_s_seh-1.dll"
    Delete "$INSTDIR\libstdc++-6.dll"
    Delete "$INSTDIR\libwinpthread-1.dll"
    Delete "$INSTDIR\uninstall.exe"

    ; Remove Qt plugin dirs
    RMDir /r "$INSTDIR\platforms"
    RMDir /r "$INSTDIR\styles"
    RMDir /r "$INSTDIR\imageformats"
    RMDir /r "$INSTDIR\iconengines"
    RMDir /r "$INSTDIR\generic"
    RMDir /r "$INSTDIR\tls"
    RMDir /r "$INSTDIR\networkinformation"

    ; Remove Skins
    RMDir /r "$INSTDIR\Skins"

    ; Remove install directory (only if empty)
    RMDir "$INSTDIR"

    ; Remove shortcuts
    Delete "$SMPROGRAMS\POSE64\POSE64.lnk"
    Delete "$SMPROGRAMS\POSE64\Uninstall POSE64.lnk"
    RMDir "$SMPROGRAMS\POSE64"
    Delete "$DESKTOP\POSE64.lnk"

    ; Remove registry keys
    DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\POSE64"
    DeleteRegKey HKLM "Software\POSE64"

SectionEnd
