#------------------------------------------------------------------------------
# Makefile for the VST Midi Driver Installer
#------------------------------------------------------------------------------

TARGET = output\vstmididrv.exe

#------------------------------------------------------------------------------
# Macros for NSIS
#------------------------------------------------------------------------------

# Microsoft did me a BIG favor with the 64bit environment variable
# "ProgramFiles(x86)", which simply can't be used in a makefile. Idjits.
# As a circumvention, it's necessary to pass it in as a command line
# parameter, like in nmake PF86="%ProgramFiles(x86)%" /f blabla
# Oh well...
PF=$(PROGRAMFILES)
!IF EXIST("$(PF86)\nsis\makensis.exe")
MAKENSIS = "$(PF86)\nsis\makensis.exe"
!ELSE IF EXIST("$(PF)\nsis\makensis.exe")
MAKENSIS = "$(PF)\nsis\makensis.exe"
!ENDIF

!IFNDEF MAKENSIS
#------------------------------------------------------------------------------
# NSIS not found - display download instructions
#------------------------------------------------------------------------------

all:    $(TARGET)
        @echo NSIS not found!
        @echo Please download and install V3.0a1 or higher from
        @echo http://nsis.sourceforge.net/Download
        @echo with the locked-list plug-in from
        @echo https://nsis.sourceforge.io/LockedList_plug-in
        @echo to build the VST Midi Driver!
        
!ELSE
#------------------------------------------------------------------------------
# NSIS found, so go building
#------------------------------------------------------------------------------

# additional NSIS command line parameters could be given here
NSISPARMS = 
MKNSIS = $(MAKENSIS) $(NSISPARMS)

#------------------------------------------------------------------------------
# Main symbolic target
#------------------------------------------------------------------------------

all:    $(TARGET)
        @echo Installer generated

#------------------------------------------------------------------------------
# clean symbolic target
#------------------------------------------------------------------------------

clean:
        @if exist $(TARGET) del $(TARGET)
        @echo Cleaned up

#------------------------------------------------------------------------------
# vstmididrv.exe generation
#------------------------------------------------------------------------------

$(TARGET): vstmididrv.mak \
        vstmididrv.nsi \
        ReleaseUnicode\ummidiplg.dll \
        output\vstmididrv.dll \
        output\vstbridgeapp32.exe \
        output\vstmididrvcfg.exe \
        output\bassasio_vstdrv.dll \
        output\ASIO2WASAPI_vstdrv.dll \
        output\cpltasks32.xml \
        output\cpltasks64.xml \
        output\vstmididrvcfg.cpl \
        output\vstmidiproxy.exe \
        output\64\vstmididrv.dll \
        output\64\bassasio_vstdrv.dll \
        output\64\ASIO2WASAPI_vstdrv.dll \
        output\64\vstbridgeapp64.exe \
        output\64\vstmididrvcfg.exe 
        $(MKNSIS) vstmididrv.nsi

#------------------------------------------------------------------------------
# subtargets that need manual copying
#------------------------------------------------------------------------------

output\bassasio_vstdrv.dll: external_packages\lib\bassasio_vstdrv.dll
        @copy /y external_packages\lib\bassasio_vstdrv.dll output\bassasio_vstdrv.dll

output\ASIO2WASAPI_vstdrv.dll: external_packages\lib\ASIO2WASAPI_vstdrv.dll
        @copy /y external_packages\lib\ASIO2WASAPI_vstdrv.dll output\ASIO2WASAPI_vstdrv.dll

output\64\bassasio_vstdrv.dll: external_packages\lib64\bassasio_vstdrv.dll
        @copy /y external_packages\lib64\bassasio_vstdrv.dll output\64\bassasio_vstdrv.dll

output\64\ASIO2WASAPI_vstdrv.dll: external_packages\lib64\ASIO2WASAPI_vstdrv.dll
        @copy /y external_packages\lib64\ASIO2WASAPI_vstdrv.dll output\64\ASIO2WASAPI_vstdrv.dll

output\cpltasks32.xml: drivercfg\cpltasks32.xml
        @copy /y drivercfg\cpltasks32.xml output\cpltasks32.xml

output\cpltasks64.xml: drivercfg\cpltasks64.xml
        @copy /y drivercfg\cpltasks64.xml output\cpltasks64.xml 

      
       
  


!ENDIF // NSIS found