!include "x64.nsh"
!include MUI2.nsh
!include WinVer.nsh
ManifestDPIAware true
; The name of the installer
!getdllversion "output\vstmididrvcfg.exe" expv_
Name "VST MIDI System Synth (Falcomod) ${expv_1}.${expv_2}.${expv_3}"

!define PRODUCT_NAME "VST MIDI System Synth (Falcomod)"

VIProductVersion "${expv_1}.${expv_2}.${expv_3}.${expv_4}"
VIFileVersion "${expv_1}.${expv_2}.${expv_3}.${expv_4}"
VIAddVersionKey "FileVersion" "${expv_1}.${expv_2}.${expv_3}.${expv_4}"
VIAddVersionKey "ProductVersion" "${expv_1}.${expv_2}.${expv_3}"
VIAddVersionKey "FileDescription" "${PRODUCT_NAME}"
VIAddVersionKey "ProductName" "${PRODUCT_NAME}"
VIAddVersionKey "LegalCopyright" "kode54, Arakula, DB50XG, Falcosoft"

;Directory of User-Mode MIDI Registration PlugIn
!ifdef NSIS_UNICODE
  ; necessary for NSIS >= 3.07 which defaults to Unicode
  !addplugindir ReleaseUnicode
!else
  !addplugindir Release
!endif

!ifdef INNER
  !echo "Inner invocation"
  OutFile "$%temp%\tempinstaller.exe"
  SetCompress off
!else
  !echo "Outer invocation"

  !system "$\"${NSISDIR}\makensis$\" /DINNER vstmididrv.nsi" = 0

  !system "$%TEMP%\tempinstaller.exe" = 2

  ;!system "m:\signit.bat $%TEMP%\vstmididrvuninstall.exe" = 0

  ; The file to write
  OutFile "vstmididrv.exe"

  ; Request application privileges for Windows Vista
  RequestExecutionLevel admin 
  SetCompressor /solid lzma 
!endif

;--------------------------------
; Pages
!insertmacro MUI_PAGE_WELCOME
Page Custom LockedListShow
!insertmacro MUI_PAGE_INSTFILES
UninstPage Custom un.LockedListShow
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

!macro DeleteOnReboot Path
  IfFileExists `${Path}` 0 +3
    SetFileAttributes `${Path}` NORMAL
    Delete /rebootok `${Path}`
!macroend
!define DeleteOnReboot `!insertmacro DeleteOnReboot`

Function LockedListShow
 ${If} ${AtLeastWinVista}
  !insertmacro MUI_HEADER_TEXT `File in use check` `Drive use check`
  LockedList::AddModule \vstmididrv.dll
  LockedList::Dialog  /autonext   
  Pop $R0
  ${EndIf}
FunctionEnd
Function un.LockedListShow
 ${If} ${AtLeastWinVista}
  !insertmacro MUI_HEADER_TEXT `File in use check` `Drive use check`
  LockedList::AddModule \vstmididrv.dll
  LockedList::Dialog  /autonext   
  Pop $R0
 ${EndIf}
FunctionEnd
;--------------------------------
Function .onInit

 SetShellVarContext All
 ${IfNot} ${IsNT}
  MessageBox MB_OK|MB_ICONSTOP "This driver cannot be installed on Windows 9x systems." /SD IDOK  
  Abort 
 ${EndIf}

!ifdef INNER
  WriteUninstaller "$%TEMP%\vstmididrvuninstall.exe"
  Quit
!endif

ReadRegStr $R0 HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VST MIDI System Synth" "UninstallString"
  StrCmp $R0 "" done
  MessageBox MB_OKCANCEL|MB_ICONEXCLAMATION \
  "The MIDI driver is already installed. $\n$\nClick `OK` to remove the \
  previous version or `Cancel` to cancel this upgrade." \
  /SD IDOK IDOK uninst
  Abort
;Run the uninstaller
uninst:
  ClearErrors
  Exec $R0
  Abort
done:
   MessageBox MB_YESNO "This will install the VST MIDI System Synth. Continue?" /SD IDYES IDYES NoAbort
     Abort ; causes installer to quit.
   NoAbort:
 FunctionEnd
; The stuff to install
Section "Needed (required)"
  SectionIn RO
  ; Copy files according to whether its x64 or not.
   DetailPrint "Copying driver and synth..."
   
  ${If} ${RunningX64}
    ;===========================================================================
    ;installer running on 64bit OS
    ;===========================================================================
    SetOutPath "$WINDIR\SysWow64"
    File output\vstmididrv.dll 
    SetOutPath "$WINDIR\SysWow64\vstmididrv\Help"
    File /a /r "Help\*" 
    SetOutPath "$WINDIR\SysWow64\vstmididrv"   
    File output\bassasio.dll   
    File output\vstmididrvcfg.exe
    File output\vsthost32.exe
    File output\64\vsthost64.exe   
!ifndef INNER
    File $%TEMP%\vstmididrvuninstall.exe
!endif
    SetOutPath "$WINDIR\SysNative"
    File output\64\vstmididrv.dll
    SetOutPath "$WINDIR\SysNative\vstmididrv" 
    File output\64\bassasio.dll   
    File output\64\vstmididrvcfg.exe
    File output\vsthost32.exe
    File output\64\vsthost64.exe
    
    ummidiplg::SetupRegistry "vstmididrv.dll" "VST MIDI Driver" "falcosoft" "ROOT\vstmididrv"
    pop $2
    pop $0
    pop $1
    ${If} $2 != "OK"
      DetailPrint $2
      SetErrors
      MessageBox MB_OK "Something went wrong with the registry setup. Installation will continue, but it might not work. $2" /SD IDOK
    ${Else}
      SetRegView 64
      WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VST MIDI System Synth\Backup" \
        "MIDI" "midi$1"
      WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VST MIDI System Synth\Backup" \
        "MIDIDRV" "$0"
      SetRegView 32
      WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VST MIDI System Synth\Backup" \
        "MIDI64" "midi$1"
      WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VST MIDI System Synth\Backup" \
        "MIDIDRV64" "$0"
    ${EndIf}
  
  ${Else}
    ;===========================================================================
    ;installer running on 32bit OS
    ;===========================================================================
    SetOutPath "$WINDIR\System32"
    File output\vstmididrv.dll 
    SetOutPath "$WINDIR\System32\vstmididrv\Help"
    File /a /r "Help\*" 
    SetOutPath "$WINDIR\System32\vstmididrv"   
    File output\bassasio.dll   
    File output\vstmididrvcfg.exe
    File output\vsthost32.exe    
!ifndef INNER
    File $%TEMP%\vstmididrvuninstall.exe
!endif
    
    ummidiplg::SetupRegistry "vstmididrv.dll" "VST MIDI Driver" "falcosoft" "ROOT\vstmididrv"
    pop $2
    pop $0
    pop $1
    ${If} $2 != "OK"
      DetailPrint $2
      SetErrors
      MessageBox MB_OK "Something went wrong with the registry setup. Installation will continue, but it might not work. $2" /SD IDOK
    ${Else}
      WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VST MIDI System Synth\Backup" \
        "MIDI" "midi$1"
      WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VST MIDI System Synth\Backup" \
        "MIDIDRV" "$0"
    ${EndIf}
 ${EndIf}
   
REGDONE:
  ; Write the uninstall keys
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VST MIDI System Synth" "DisplayName" "VST MIDI System Synth"
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VST MIDI System Synth" "NoModify" 1
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VST MIDI System Synth" "NoRepair" 1  
  CreateDirectory "$SMPROGRAMS\VST MIDI System Synth"
  ${If} ${RunningX64}
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VST MIDI System Synth" "UninstallString" '"$WINDIR\SysWow64\vstmididrv\vstmididrvuninstall.exe"'
    WriteRegStr HKLM "Software\VST MIDI Driver" "path" "$WINDIR\SysWow64\vstmididrv"
    SetOutPath "$WINDIR\SysWow64\vstmididrv\Help" 
    CreateShortCut "$SMPROGRAMS\VST MIDI System Synth\ReadMe.lnk" "$WINDIR\SysWow64\vstmididrv\Help\Readme.html" "" "$WINDIR\SysWow64\vstmididrv\Help\Readme.html" 0
    SetOutPath "$WINDIR\SysWow64\vstmididrv"
    CreateShortCut "$SMPROGRAMS\VST MIDI System Synth\Uninstall.lnk" "$WINDIR\SysWow64\vstmididrv\vstmididrvuninstall.exe" "" "$WINDIR\SysWow64\vstmididrvuninstall.exe" 0
    CreateShortCut "$SMPROGRAMS\VST MIDI System Synth\Configure VST MIDI Driver.lnk" "$WINDIR\SysWow64\vstmididrv\vstmididrvcfg.exe" "" "$WINDIR\SysWow64\vstmididrv\vstmididrvcfg.exe" 0
    SetOutPath "$WINDIR\System32\vstmididrv" 
    CreateShortCut "$SMPROGRAMS\VST MIDI System Synth\Configure VST MIDI Driver (x64).lnk" "$WINDIR\System32\vstmididrv\vstmididrvcfg.exe" "" "$WINDIR\System32\vstmididrv\vstmididrvcfg.exe" 0
    SetOutPath "$WINDIR\SysNative\vstmididrv" 
  ${Else}
    WriteRegStr HKLM "Software\VST MIDI Driver" "path" "$WINDIR\System32\vstmididrv"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VST MIDI System Synth" "UninstallString" '"$WINDIR\System32\vstmididrv\vstmididrvuninstall.exe"'
    SetOutPath "$WINDIR\System32\vstmididrv\Help"
    CreateShortCut "$SMPROGRAMS\VST MIDI System Synth\ReadMe.lnk" "$WINDIR\System32\vstmididrv\Help\Readme.html" "" "$WINDIR\System32\vstmididrv\Help\Readme.html" 0
    SetOutPath "$WINDIR\System32\vstmididrv"
    CreateShortCut "$SMPROGRAMS\VST MIDI System Synth\Uninstall.lnk" "$WINDIR\System32\vstmididrv\vstmididrvuninstall.exe" "" "$WINDIR\System32\vstmididrv\vstmididrvuninstall.exe" 0
    CreateShortCut "$SMPROGRAMS\VST MIDI System Synth\Configure VST MIDI Driver.lnk" "$WINDIR\System32\vstmididrv\vstmididrvcfg.exe" "" "$WINDIR\System32\vstmididrv\vstmididrvcfg.exe" 0
  ${EndIf}  
  ${If} ${IsWinNT4}
	MessageBox MB_YESNO|MB_ICONQUESTION "Installation complete! Use the driver configuration tool which is in the 'VST MIDI System Synth' program shortcut directory to configure the driver.$\nYou need to reboot in order for control panel to show the driver!$\nIs it OK to reboot?" /SD IDNO IDNO +2
	Reboot
  ${Else}
	MessageBox MB_OK "Installation complete! Use the driver configuration tool which is in the 'VST MIDI System Synth' program shortcut directory to configure the driver." /SD IDOK
  ${EndIf}
  

SectionEnd
;--------------------------------

; Uninstaller

!ifdef INNER
Section "Uninstall"

  SetShellVarContext All
  ummidiplg::CleanupRegistry "vstmididrv.dll" "VST MIDI Driver" "ROOT\vstmididrv"
  pop $0
  ${If} $0 != "OK"
    DetailPrint $0
    SetErrors
  ${EndIf}
  
  ; Remove registry keys
   ReadRegStr $0 HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VST MIDI System Synth\Backup" \
     "MIDI"
  ReadRegStr $1 HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VST MIDI System Synth\Backup" \
    "MIDIDRV"
  WriteRegStr HKLM "Software\Microsoft\Windows NT\CurrentVersion\Drivers32" "$0" "$1"
  ${If} ${RunningX64}
    ReadRegStr $0 HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VST MIDI System Synth\Backup" \
      "MIDI64"
    ReadRegStr $1 HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VST MIDI System Synth\Backup" \
      "MIDIDRV64"
    SetRegView 64
    WriteRegStr HKLM "Software\Microsoft\Windows NT\CurrentVersion\Drivers32" "$0" "$1"
    SetRegView 32
  ${EndIf}
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VST MIDI System Synth"
  DeleteRegKey HKLM "Software\VST MIDI Driver"
  RMDir /r "$SMPROGRAMS\VST MIDI System Synth"
  
 ${If} ${RunningX64}
   ${If} ${AtLeastWinVista}
     Delete /REBOOTOK "$WINDIR\SysWow64\vstmididrv.dll"
     RMDir /r /REBOOTOK "$WINDIR\SysWow64\vstmididrv"
     Delete /REBOOTOK "$WINDIR\SysNative\vstmididrv.dll"
     RMDir /r /REBOOTOK "$WINDIR\SysNative\vstmididrv"
   ${Else}
;    MessageBox MB_OK "Note: The uninstaller will reboot your system to remove drivers." /SD IDOK
     ${DeleteOnReboot} $WINDIR\SysWow64\vstmididrv.dll  
     ${DeleteOnReboot} $WINDIR\SysWow64\vstmididrv\bassasio.dll  
     ${DeleteOnReboot} $WINDIR\SysWow64\vstmididrv\vstmididrvuninstall.exe
     ${DeleteOnReboot} $WINDIR\SysWow64\vstmididrv\vstmididrvcfg.exe
     ${DeleteOnReboot} $WINDIR\SysWow64\vstmididrv\vsthost32.exe
     ${DeleteOnReboot} $WINDIR\SysWow64\vstmididrv\vsthost64.exe
     RMDir /r /REBOOTOK $WINDIR\SysWow64\vstmididrv     
     ${DeleteOnReboot} $WINDIR\SysNative\vstmididrv.dll 
     ${DeleteOnReboot} $WINDIR\SysNative\vstmididrv\bassasio.dll  
     ${DeleteOnReboot} $WINDIR\SysNative\vstmididrv\vstmididrvcfg.exe   
     ${DeleteOnReboot} $WINDIR\SysNative\vstmididrv\vsthost32.exe
     ${DeleteOnReboot} $WINDIR\SysNative\vstmididrv\vsthost64.exe
     RMDir /r /REBOOTOK $WINDIR\SysNative\vstmididrv 
   ${Endif}
 ${Else}
   ${If} ${AtLeastWinVista}
     Delete /REBOOTOK "$WINDIR\System32\vstmididrv.dll"
     RMDir /r /REBOOTOK "$WINDIR\System32\vstmididrv"
   ${Else}
;    MessageBox MB_OK "Note: The uninstaller will reboot your system to remove drivers." /SD IDOK
     ${DeleteOnReboot} $WINDIR\System32\vstmididrv.dll 
     ${DeleteOnReboot} $WINDIR\System32\vstmididrv\bassasio.dll    
     ${DeleteOnReboot} $WINDIR\System32\vstmididrv\vstmididrvuninstall.exe
     ${DeleteOnReboot} $WINDIR\System32\vstmididrv\vstmididrvcfg.exe
     ${DeleteOnReboot} $WINDIR\System32\vstmididrv\vsthost32.exe
     RMDir /r /REBOOTOK $WINDIR\System32\vstmididrv
   ${Endif}
 ${EndIf}
 IfRebootFlag 0 noreboot
   MessageBox MB_YESNO "A reboot is required to finish the deinstallation. Do you wish to reboot now?" /SD IDNO IDNO noreboot
     Reboot
 noreboot:
 
SectionEnd
!endif
