!include "x64.nsh"
!include MUI2.nsh
!include WinVer.nsh
ManifestDPIAware true

!define MUI_ICON "drivercfg\install.ico"
!define MUI_UNICON "drivercfg\uninstall.ico"

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
!define MUI_COMPONENTSPAGE_SMALLDESC
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
UninstPage Custom un.LockedListShow
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_COMPONENTS
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

; The stuff to install

Section "Core components (required)" instsect1
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
    File output\vstmididrvcfg.exe
    File output\vstbridgeapp32.exe
    File output\vstmidiproxy.exe
    ${If} ${AtLeastWinVista}
      File output\cpltasks64.xml
    ${Endif}
    File output\64\vstbridgeapp64.exe   
!ifndef INNER
    File $%TEMP%\vstmididrvuninstall.exe
!endif
    SetOutPath "$WINDIR\SysNative"
    File output\64\vstmididrv.dll
    SetOutPath "$WINDIR\SysNative\vstmididrv" 
    File output\64\vstmididrvcfg.exe
    File output\vstbridgeapp32.exe
    File output\64\vstbridgeapp64.exe
    
    ummidiplg::SetupRegistry "vstmididrv.dll" "VST MIDI Driver" "Falcosoft" "ROOT\vstmididrv"
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
        "MIDI64" "midi$0"
      WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VST MIDI System Synth\Backup" \
        "MIDIDRV64" "$1"
      SetRegView 32
      WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VST MIDI System Synth\Backup" \
        "MIDI" "midi$0"
      WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VST MIDI System Synth\Backup" \
        "MIDIDRV" "$1"
    ${EndIf}
  
  ${Else}
    ;===========================================================================
    ;installer running on 32bit OS
    ;===========================================================================
    SetOutPath "$WINDIR\System32"
    File output\vstmididrv.dll 
    ${If} ${IsWinNT4}
     File output\vstmididrvcfg.cpl
    ${EndIf}    
    SetOutPath "$WINDIR\System32\vstmididrv\Help"
    File /a /r "Help\*" 
    SetOutPath "$WINDIR\System32\vstmididrv"   
    File output\vstmididrvcfg.exe
    ${If} ${AtLeastWinVista}
      File output\cpltasks32.xml
    ${Endif}
    ${If} ${IsWinXP}
    ${AndIf} ${AtLeastServicePack} 2
    ${OrIf} ${AtLeastWinVista}
      File output\vstmidiproxy.exe
    ${Endif}
    File output\vstbridgeapp32.exe    
!ifndef INNER
    File $%TEMP%\vstmididrvuninstall.exe
!endif
    
    ummidiplg::SetupRegistry "vstmididrv.dll" "VST MIDI Driver" "Falcosoft" "ROOT\vstmididrv"
    pop $2
    pop $0
    pop $1
    ${If} $2 != "OK"
      DetailPrint $2
      SetErrors
      MessageBox MB_OK "Something went wrong with the registry setup. Installation will continue, but it might not work. $2" /SD IDOK
    ${Else}
      WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VST MIDI System Synth\Backup" \
        "MIDI" "midi$0"
      WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VST MIDI System Synth\Backup" \
        "MIDIDRV" "$1"
    ${EndIf}
 ${EndIf}
   
; REGDONE:
  ; Write the uninstall keys
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VST MIDI System Synth" "DisplayName" "VST MIDI System Synth"
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VST MIDI System Synth" "NoModify" 1
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VST MIDI System Synth" "NoRepair" 1  
  CreateDirectory "$SMPROGRAMS\VST MIDI System Synth"
  ${If} ${RunningX64}
    SetRegView 64
    WriteRegStr HKLM "SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\ControlPanel\NameSpace\{E33B77CA-8645-49E7-8CBD-1E39673C8C43}" "" "VST MIDI Driver"
    WriteRegStr HKLM "SOFTWARE\Classes\CLSID\{E33B77CA-8645-49E7-8CBD-1E39673C8C43}" "" "VST MIDI Driver"
    WriteRegStr HKLM "SOFTWARE\Classes\CLSID\{E33B77CA-8645-49E7-8CBD-1E39673C8C43}" "System.ApplicationName" "Falcosoft.VstMidiDriver"
    WriteRegStr HKLM "SOFTWARE\Classes\CLSID\{E33B77CA-8645-49E7-8CBD-1E39673C8C43}" "System.ControlPanel.Category" "2,4"
    WriteRegStr HKLM "SOFTWARE\Classes\CLSID\{E33B77CA-8645-49E7-8CBD-1E39673C8C43}" "InfoTip" "Configure VST MIDI Driver"
    WriteRegDword HKLM "SOFTWARE\Classes\CLSID\{E33B77CA-8645-49E7-8CBD-1E39673C8C43}" "{305CA226-D286-468E-B848-2B2E8E697B74} 2" 0x00000004
    WriteRegStr HKLM "SOFTWARE\Classes\CLSID\{E33B77CA-8645-49E7-8CBD-1E39673C8C43}\DefaultIcon" "" "$WINDIR\SysWow64\vstmididrv\vstmididrvcfg.exe"
    WriteRegStr HKLM "SOFTWARE\Classes\CLSID\{E33B77CA-8645-49E7-8CBD-1E39673C8C43}\Shell\Open\Command" "" "$WINDIR\SysWow64\vstmididrv\vstmididrvcfg.exe"
    ${If} ${AtLeastWinVista}
      WriteRegStr HKLM "SOFTWARE\Classes\CLSID\{E33B77CA-8645-49E7-8CBD-1E39673C8C43}" "System.Software.TasksFileUrl" "$WINDIR\SysWow64\vstmididrv\cpltasks64.xml"	
    ${Endif}
    SetRegView 32
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VST MIDI System Synth" "UninstallString" '"$WINDIR\SysWow64\vstmididrv\vstmididrvuninstall.exe"'
    WriteRegStr HKLM "Software\VST MIDI Driver" "path" "$WINDIR\SysWow64\vstmididrv"
    SetOutPath "$WINDIR\SysWow64\vstmididrv\Help" 
    CreateShortCut "$SMPROGRAMS\VST MIDI System Synth\ReadMe.lnk" "$WINDIR\SysWow64\vstmididrv\Help\Readme.html" "" "$WINDIR\SysWow64\vstmididrv\Help\Readme.html" 0
    SetOutPath "$WINDIR\SysWow64\vstmididrv"
    CreateShortCut "$SMPROGRAMS\VST MIDI System Synth\Uninstall.lnk" "$WINDIR\SysWow64\vstmididrv\vstmididrvuninstall.exe" "" "$WINDIR\SysWow64\vstmididrvuninstall.exe" 0
    CreateShortCut "$SMPROGRAMS\VST MIDI System Synth\Configure VST MIDI Driver.lnk" "$WINDIR\SysWow64\vstmididrv\vstmididrvcfg.exe" "" "$WINDIR\SysWow64\vstmididrv\vstmididrvcfg.exe" 0
    CreateShortCut "$SMPROGRAMS\VST MIDI System Synth\VST MIDI Driver Global Proxy.lnk" "$WINDIR\SysWow64\vstmididrv\vstmidiproxy.exe" "" "$WINDIR\SysWow64\vstmididrv\vstmidiproxy.exe" 0
    SetOutPath "$WINDIR\System32\vstmididrv" 
    CreateShortCut "$SMPROGRAMS\VST MIDI System Synth\Configure VST MIDI Driver (x64).lnk" "$WINDIR\System32\vstmididrv\vstmididrvcfg.exe" "" "$WINDIR\System32\vstmididrv\vstmididrvcfg.exe" 0
    SetOutPath "$WINDIR\SysNative\vstmididrv" 
  ${Else}
    WriteRegStr HKLM "SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\ControlPanel\NameSpace\{E33B77CA-8645-49E7-8CBD-1E39673C8C43}" "" "VST MIDI Driver"
    WriteRegStr HKLM "SOFTWARE\Classes\CLSID\{E33B77CA-8645-49E7-8CBD-1E39673C8C43}" "" "VST MIDI Driver"
    WriteRegStr HKLM "SOFTWARE\Classes\CLSID\{E33B77CA-8645-49E7-8CBD-1E39673C8C43}" "System.ApplicationName" "Falcosoft.VstMidiDriver"
    WriteRegStr HKLM "SOFTWARE\Classes\CLSID\{E33B77CA-8645-49E7-8CBD-1E39673C8C43}" "System.ControlPanel.Category" "2,4"
    WriteRegStr HKLM "SOFTWARE\Classes\CLSID\{E33B77CA-8645-49E7-8CBD-1E39673C8C43}" "InfoTip" "Configure VST MIDI Driver"
    WriteRegDword HKLM "SOFTWARE\Classes\CLSID\{E33B77CA-8645-49E7-8CBD-1E39673C8C43}" "{305CA226-D286-468E-B848-2B2E8E697B74} 2" 0x00000004
    WriteRegStr HKLM "SOFTWARE\Classes\CLSID\{E33B77CA-8645-49E7-8CBD-1E39673C8C43}\DefaultIcon" "" "$WINDIR\System32\vstmididrv\vstmididrvcfg.exe"
    WriteRegStr HKLM "SOFTWARE\Classes\CLSID\{E33B77CA-8645-49E7-8CBD-1E39673C8C43}\Shell\Open\Command" "" "$WINDIR\System32\vstmididrv\vstmididrvcfg.exe"
    ${If} ${AtLeastWinVista}
      WriteRegStr HKLM "SOFTWARE\Classes\CLSID\{E33B77CA-8645-49E7-8CBD-1E39673C8C43}" "System.Software.TasksFileUrl" "$WINDIR\System32\vstmididrv\cpltasks32.xml"	
    ${Endif}
    WriteRegStr HKLM "Software\VST MIDI Driver" "path" "$WINDIR\System32\vstmididrv"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VST MIDI System Synth" "UninstallString" '"$WINDIR\System32\vstmididrv\vstmididrvuninstall.exe"'
    SetOutPath "$WINDIR\System32\vstmididrv\Help"
    CreateShortCut "$SMPROGRAMS\VST MIDI System Synth\ReadMe.lnk" "$WINDIR\System32\vstmididrv\Help\Readme.html" "" "$WINDIR\System32\vstmididrv\Help\Readme.html" 0
    SetOutPath "$WINDIR\System32\vstmididrv"
    ${If} ${IsWinXP}
    ${AndIf} ${AtLeastServicePack} 2
    ${OrIf} ${AtLeastWinVista}
      CreateShortCut "$SMPROGRAMS\VST MIDI System Synth\VST MIDI Driver Global Proxy.lnk" "$WINDIR\System32\vstmididrv\vstmidiproxy.exe" "" "$WINDIR\System32\vstmididrv\vstmidiproxy.exe" 0
    ${Endif}
    CreateShortCut "$SMPROGRAMS\VST MIDI System Synth\Uninstall.lnk" "$WINDIR\System32\vstmididrv\vstmididrvuninstall.exe" "" "$WINDIR\System32\vstmididrv\vstmididrvuninstall.exe" 0
    CreateShortCut "$SMPROGRAMS\VST MIDI System Synth\Configure VST MIDI Driver.lnk" "$WINDIR\System32\vstmididrv\vstmididrvcfg.exe" "" "$WINDIR\System32\vstmididrv\vstmididrvcfg.exe" 0
  ${EndIf}  
  ${If} ${IsWinNT4}
    MessageBox MB_YESNO|MB_ICONQUESTION "Installation complete! Use the driver configuration tool shortcut to configure the driver.$\nYou need to reboot in order for control panel to show the driver!$\nIs it OK to reboot?" /SD IDNO IDNO +2
    Reboot
  ${Else}
    MessageBox MB_OK "Installation complete! Use the driver configuration tool shortcut or the control panel item to configure the driver." /SD IDOK
  ${EndIf}  

SectionEnd

Section "ASIO Output (BassASIO)" instsect2

  ${If} ${RunningX64}
    ;===========================================================================
    ;installer running on 64bit OS
    ;===========================================================================
    SetOutPath "$WINDIR\SysWow64\vstmididrv"   
    File output\bassasio_vstdrv.dll   
        
    SetOutPath "$WINDIR\SysNative\vstmididrv" 
    File output\64\bassasio_vstdrv.dll   
       
  ${Else}
    ;===========================================================================
    ;installer running on 32bit OS
    ;===========================================================================
    SetOutPath "$WINDIR\System32\vstmididrv"   
    File output\bassasio_vstdrv.dll
        
  ${EndIf} 
  
  WriteRegDword HKCU "Software\VSTi Driver" "UsePrivateAsioOnly" 0x00000000   

SectionEnd

Section "ASIO2WASAPI Plugin" instsect3

  ${If} ${RunningX64}
    ;===========================================================================
    ;installer running on 64bit OS
    ;===========================================================================
    SetOutPath "$WINDIR\SysWow64\vstmididrv"   
   	File output\ASIO2WASAPI_vstdrv.dll 
       
    SetOutPath "$WINDIR\SysNative\vstmididrv" 
    File output\64\ASIO2WASAPI_vstdrv.dll 
   
  ${Else}
    ;===========================================================================
    ;installer running on 32bit OS
    ;===========================================================================
    SetOutPath "$WINDIR\System32\vstmididrv"   
   	File output\ASIO2WASAPI_vstdrv.dll  
    
  ${EndIf}

SectionEnd 

Section /o "Use only ASIO2WASAPI as ASIO driver" instsect4
	WriteRegDword HKCU "Software\VSTi Driver" "UsePrivateAsioOnly" 0x00000001 

SectionEnd 

Function .onSelChange
${If} $9 == "1"
	StrCpy $9 0
	${If} ${SectionIsSelected} ${instsect4}
    !insertmacro SelectSection ${instsect3} 
  ${EndIf}  
	${If} ${SectionIsSelected} ${instsect3}
    !insertmacro SelectSection ${instsect2}     
	${EndIf}
${Else}
	StrCpy $9 1
	${IfNot} ${SectionIsSelected} ${instsect2}
    !insertmacro UnSelectSection ${instsect3} 
  ${EndIf}  
  ${IfNot} ${SectionIsSelected} ${instsect3}
    !insertmacro UnSelectSection ${instsect4}      
	${EndIf}
${EndIf}
FunctionEnd

;--------------------------------
Function .onInit

 StrCpy $9 0
 SetShellVarContext All
 ${IfNot} ${IsNT}
  MessageBox MB_OK|MB_ICONSTOP "This driver cannot be installed on Windows 9x systems." /SD IDOK  
  Abort 
 ${EndIf}
 
 ${IfNot} ${AtLeastWinVista}
  !insertmacro UnSelectSection ${instsect3}   
  SectionSetText ${instsect3} ""
  !insertmacro UnSelectSection ${instsect4}   
  SectionSetText ${instsect4} ""
 ${Endif}
 
 ${If} ${IsWinNT4}
  !insertmacro UnSelectSection ${instsect2}   
  SectionSetText ${instsect2} ""
 ${Endif}

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

;--------------------------------

; Uninstaller

!ifdef INNER
Section /o "un.User registry settings" uninstsect1
  SetShellVarContext All
	DeleteRegKey HKCU "Software\VSTi Driver"
SectionEnd

Section "un.Driver files/settings" uninstsect2

  SectionIn RO
  SetShellVarContext All
  ummidiplg::CleanupRegistry "vstmididrv.dll" "VST MIDI Driver" "ROOT\vstmididrv"
  pop $0
  ${If} $0 != "OK"
    DetailPrint $0
    SetErrors
  ${EndIf}
  
  ; Remove registry keys
  
  ; The whole restore process does not make much sense in the current form...
  ; ReadRegStr $0 HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VST MIDI System Synth\Backup" \
  ;   "MIDI"
  ;  ReadRegStr $1 HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VST MIDI System Synth\Backup" \
  ;  "MIDIDRV"
  ;  WriteRegStr HKLM "Software\Microsoft\Windows NT\CurrentVersion\Drivers32" "$0" "$1"
  ${If} ${RunningX64}
    SetRegView 64
  ; ReadRegStr $0 HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VST MIDI System Synth\Backup" \
  ;    "MIDI64"
  ;  ReadRegStr $1 HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VST MIDI System Synth\Backup" \
  ;    "MIDIDRV64"   
  ;  WriteRegStr HKLM "Software\Microsoft\Windows NT\CurrentVersion\Drivers32" "$0" "$1"
    DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VST MIDI System Synth"
    DeleteRegKey HKLM "SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\ControlPanel\NameSpace\{E33B77CA-8645-49E7-8CBD-1E39673C8C43}"
    DeleteRegKey HKLM "SOFTWARE\Classes\CLSID\{E33B77CA-8645-49E7-8CBD-1E39673C8C43}"
    SetRegView 32
  ${EndIf}
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VST MIDI System Synth"
  DeleteRegKey HKLM "Software\VST MIDI Driver"
  DeleteRegKey HKLM "SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\ControlPanel\NameSpace\{E33B77CA-8645-49E7-8CBD-1E39673C8C43}"
  DeleteRegKey HKLM "SOFTWARE\Classes\CLSID\{E33B77CA-8645-49E7-8CBD-1E39673C8C43}"
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
     ${DeleteOnReboot} $WINDIR\SysWow64\vstmididrv\vstmididrvuninstall.exe
     ${DeleteOnReboot} $WINDIR\SysWow64\vstmididrv\vstmididrvcfg.exe
     ${DeleteOnReboot} $WINDIR\SysWow64\vstmididrv\vstmidiproxy.exe
     ${DeleteOnReboot} $WINDIR\SysWow64\vstmididrv\vstbridgeapp32.exe
     ${DeleteOnReboot} $WINDIR\SysWow64\vstmididrv\vstbridgeapp64.exe
     ${DeleteOnReboot} $WINDIR\SysWow64\vstmididrv\bassasio_vstdrv.dll 
     ${DeleteOnReboot} $WINDIR\SysWow64\vstmididrv\ASIO2WASAPI_vstdrv.dll 
     RMDir /r /REBOOTOK $WINDIR\SysWow64\vstmididrv 
     ${DeleteOnReboot} $WINDIR\SysNative\vstmididrv.dll     
     ${DeleteOnReboot} $WINDIR\SysNative\vstmididrv\vstmididrvcfg.exe 
     ${DeleteOnReboot} $WINDIR\SysNative\vstmididrv\vstbridgeapp32.exe
     ${DeleteOnReboot} $WINDIR\SysNative\vstmididrv\vstbridgeapp64.exe
     ${DeleteOnReboot} $WINDIR\SysNative\vstmididrv\bassasio_vstdrv.dll  
     ${DeleteOnReboot} $WINDIR\SysNative\vstmididrv\ASIO2WASAPI_vstdrv.dll 
     RMDir /r /REBOOTOK $WINDIR\SysNative\vstmididrv 
   ${Endif}
 ${Else}
   ${If} ${IsWinNT4}
     ${DeleteOnReboot} $WINDIR\System32\vstmididrvcfg.cpl
   ${EndIf}   
   ${If} ${AtLeastWinVista}
     Delete /REBOOTOK "$WINDIR\System32\vstmididrv.dll"
     RMDir /r /REBOOTOK "$WINDIR\System32\vstmididrv"
   ${Else}
;    MessageBox MB_OK "Note: The uninstaller will reboot your system to remove drivers." /SD IDOK
     ${DeleteOnReboot} $WINDIR\System32\vstmididrv.dll     
     ${DeleteOnReboot} $WINDIR\System32\vstmididrv\vstmididrvuninstall.exe
     ${DeleteOnReboot} $WINDIR\System32\vstmididrv\vstmididrvcfg.exe
     ${DeleteOnReboot} $WINDIR\System32\vstmididrv\vstmidiproxy.exe
     ${DeleteOnReboot} $WINDIR\System32\vstmididrv\vstbridgeapp32.exe
     ${DeleteOnReboot} $WINDIR\System32\vstmididrv\bassasio_vstdrv.dll 
     ${DeleteOnReboot} $WINDIR\System32\vstmididrv\ASIO2WASAPI_vstdrv.dll 
     RMDir /r /REBOOTOK $WINDIR\System32\vstmididrv
   ${Endif}
 ${EndIf}
 IfRebootFlag 0 noreboot
   MessageBox MB_YESNO "A reboot is required to finish the deinstallation. Do you wish to reboot now?" /SD IDNO IDNO noreboot
     Reboot
 noreboot:
 
SectionEnd

LangString DESC_uninstSection1 ${LANG_ENGLISH} "If you want to clear all user settings then select this. Otherwise your settings are preserved."
LangString DESC_uninstSection2 ${LANG_ENGLISH} "Core VSTi driver components. They have to be uninstalled."

!insertmacro MUI_UNFUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${uninstsect1} $(DESC_uninstSection1)
  !insertmacro MUI_DESCRIPTION_TEXT ${uninstsect2} $(DESC_uninstSection2)
!insertmacro MUI_UNFUNCTION_DESCRIPTION_END

!endif

LangString DESC_instSection1 ${LANG_ENGLISH} "Core VSTi driver components. Installation is required."
LangString DESC_instSection2 ${LANG_ENGLISH} "If you only need the default Wave output then you can skip installing BassAsio. CPU with SSE is required."
LangString DESC_instSection3 ${LANG_ENGLISH} "If ASIO is installed you can also install ASIO2WASAPI plugin to use WASAPI output modes."
LangString DESC_instSection4 ${LANG_ENGLISH} "If you have problems with system ASIO drivers you can ignore them and use only private ASIO2WASAPI."

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${instsect1} $(DESC_instSection1)
  !insertmacro MUI_DESCRIPTION_TEXT ${instsect2} $(DESC_instSection2)
  !insertmacro MUI_DESCRIPTION_TEXT ${instsect3} $(DESC_instSection3)
  !insertmacro MUI_DESCRIPTION_TEXT ${instsect4} $(DESC_instSection4)
!insertmacro MUI_FUNCTION_DESCRIPTION_END
