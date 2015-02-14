@ECHO OFF
if "%INCLUDE%" EQU "" GOTO ERROR
@cl /Fevstmididrv.dll /Od /Zi /MTd /EHsc /DUNICODE /D_UNICODE /LDd /I "..\external_packages" VSTDriver.cpp MidiSynth.cpp winmm_drv.cpp kernel32.lib user32.lib Shlwapi.lib advapi32.lib winmm.lib Ole32.lib uuid.lib vstmididrv.def
goto END
:ERROR
echo.
echo This scripts needs to be run from a Visual studio command line 
echo (Check Visual Studio tools, Visual studio comand line in the programs menu)
:END
