@ECHO OFF
SETLOCAL
REM Ensure the environment is set up
if not defined %INCLUDE% GOTO ERROR
REM Workaround to detect x64 or x86 environment
cl 1> NUL 2> tmp.txt
find /C "x64" < tmp.txt > tmp2.txt
SET /p IS64BIT= < tmp2.txt
del tmp.txt
del tmp2.txt
cl /O1 /MT /EHsc /DUNICODE /D_UNICODE /LD /I "..\external_packages" /MP%NUMBER_OF_PROCESSORS% vstmididrv.cpp ..\driver\src\VSTDriver.cpp dsound.cpp sound_out_dsound.cpp sound_out_xaudio2.cpp kernel32.lib user32.lib Shlwapi.lib advapi32.lib winmm.lib Ole32.lib uuid.lib vstmididrv.def
if ERRORLEVEL 1 goto END
REM Move files to the output dir
if "%IS64BIT%" EQU "1" (
  move vstmididrv.dll ..\output\64
) else (
  move vstmididrv.dll ..\output\
)
goto END
:ERROR
echo.
echo This scripts needs to be run from a Visual studio command line 
echo (Check Visual Studio tools, Visual studio comand line in the programs menu)
:END
ENDLOCAL