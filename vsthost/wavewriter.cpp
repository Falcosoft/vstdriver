// Simple Wave writer utility class. Only native VSTi 32-bit float format is supported.
// Copyright (C) 2024 Zoltan Bacsko - Falcosoft

#include "stdafx.h"
#include <stddef.h>
#include <mmreg.h>
#include "wavewriter.h"
#include "../external_packages/audiodefs.h"

#pragma warning(disable:28159)
bool IsWinNT4()
{
    OSVERSIONINFOEX osvi = { 0 };
    BOOL bOsVersionInfoEx;   
    osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEX);
    bOsVersionInfoEx = GetVersionEx((OSVERSIONINFO*)&osvi);
    if (bOsVersionInfoEx == FALSE) return false;
    if (VER_PLATFORM_WIN32_NT == osvi.dwPlatformId && osvi.dwMajorVersion == 4)
        return true;
    return false;
}
#pragma warning(default:28159)

#if(_WIN32_WINNT < 0x0500)
#define OPENFILENAME_SIZE_VERSION_400A  CDSIZEOF_STRUCT(OPENFILENAMEA,lpTemplateName)
#define OPENFILENAME_SIZE_VERSION_400W  CDSIZEOF_STRUCT(OPENFILENAMEW,lpTemplateName)
#ifdef UNICODE
#define OPENFILENAME_SIZE_VERSION_400  OPENFILENAME_SIZE_VERSION_400W
#else
#define OPENFILENAME_SIZE_VERSION_400  OPENFILENAME_SIZE_VERSION_400A
#endif 
#endif 

WaveWriter::WaveWriter() :
    fileHandle(),
    bytesWritten(),
    channelCount(),
    isRecordingStarted(),
    isCloseRequested()
{
    _tcscpy_s(fileName, L"VSTiCapture.wav");
}

uint32_t WaveWriter::Init(int chCount, DWORD sampleRate, HWND owner)
{	  
    OPENFILENAME ofn = { 0 };
    ofn.lStructSize = IsWinNT4() ? OPENFILENAME_SIZE_VERSION_400 : sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Wave Files (*.wav)\0*.wav\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_OVERWRITEPROMPT | OFN_EXPLORER | OFN_ENABLEHOOK | OFN_ENABLESIZING;
    ofn.lpstrDefExt = L"wav";
 
    if (GetSaveFileName(&ofn))
    {
        channelCount = chCount;
        isCloseRequested = false;

        bytesWritten = 0;
        fileHandle = _tfopen(fileName, L"wbS");
        if (!fileHandle) return WaveResponse::CannotCreate;
        
        setvbuf(fileHandle, NULL, _IOFBF, 16384);

        if (channelCount == 4)
        {
            WaveHeaderExtensible waveHeaderEx = { 0 };
            strncpy(waveHeaderEx.riffHeader, "RIFF", 4);
            strncpy(waveHeaderEx.waveHeader, "WAVE", 4);
            strncpy(waveHeaderEx.fmtHeader, "fmt ", 4);
            strncpy(waveHeaderEx.factHeader, "fact", 4);
            strncpy(waveHeaderEx.dataHeader, "data", 4);
            waveHeaderEx.fmtChunkSize = 40;
            waveHeaderEx.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
            waveHeaderEx.nChannels = channelCount;
            waveHeaderEx.nSamplesPerSec = sampleRate;
            waveHeaderEx.wBitsPerSample = 32;
            waveHeaderEx.nBlockAlign = waveHeaderEx.nChannels * waveHeaderEx.wBitsPerSample / 8;
            waveHeaderEx.nAvgBytesPerSec = waveHeaderEx.nBlockAlign * waveHeaderEx.nSamplesPerSec;
            waveHeaderEx.cbSize = 22;
            waveHeaderEx.factSize = 4;
            waveHeaderEx.dwChannelMask = SPEAKER_QUAD;
            waveHeaderEx.wValidBitsPerSample = waveHeaderEx.wBitsPerSample;
            waveHeaderEx.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;           
                     
            WriteData(&waveHeaderEx, sizeof(waveHeaderEx));     
        }
        else 
        {
            WaveHeaderEx waveHeaderEx = { 0 };
            strncpy(waveHeaderEx.riffHeader, "RIFF", 4);
            strncpy(waveHeaderEx.waveHeader, "WAVE", 4);
            strncpy(waveHeaderEx.fmtHeader, "fmt ", 4);
            strncpy(waveHeaderEx.factHeader, "fact", 4);
            strncpy(waveHeaderEx.dataHeader, "data", 4);
            waveHeaderEx.fmtChunkSize = 18;
            waveHeaderEx.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
            waveHeaderEx.nChannels = channelCount;
            waveHeaderEx.nSamplesPerSec = sampleRate;
            waveHeaderEx.wBitsPerSample = 32;
            waveHeaderEx.nBlockAlign = waveHeaderEx.nChannels * waveHeaderEx.wBitsPerSample / 8;
            waveHeaderEx.nAvgBytesPerSec = waveHeaderEx.nBlockAlign * waveHeaderEx.nSamplesPerSec;
            waveHeaderEx.factSize = 4;                   
               
            WriteData(&waveHeaderEx, sizeof(waveHeaderEx)); 
        }       
        
        isRecordingStarted = true;       

        return WaveResponse::Success;
    }  
    return WaveResponse::NotSelected;
}

uint32_t WaveWriter::WriteData(const void* data, DWORD size)
{  
    if (isCloseRequested)
    {
        Close();
        return WaveResponse::Success;
    }
    
    if (!fileHandle || bytesWritten >= 0xFFFE0000) return WaveResponse::WriteError; //prevent creating unplayable 4GB+ wave files
    DWORD sizeDone = (DWORD)_fwrite_nolock(data, 1, size, fileHandle);
    bytesWritten += sizeDone;
    if(sizeDone != size) return WaveResponse::WriteError;       

    return WaveResponse::Success;
}

void WaveWriter::CloseRequest()
{
    isCloseRequested = true;
}

void WaveWriter::Close()
{
    if(!fileHandle) return;

    isRecordingStarted = false;
    isCloseRequested = false;    

    if (channelCount == 4)
    {        
        DWORD ckSize = bytesWritten - 8;
        _fseek_nolock(fileHandle, offsetof(WaveHeaderExtensible, wavSize), SEEK_SET);
        _fwrite_nolock(&ckSize, 1, sizeof(DWORD), fileHandle);

        ckSize = bytesWritten - sizeof(WaveHeaderExtensible);
        _fseek_nolock(fileHandle, offsetof(WaveHeaderExtensible, dataSize), SEEK_SET);
        _fwrite_nolock(&ckSize, 1, sizeof(DWORD), fileHandle);

        ckSize /= 4;
        _fseek_nolock(fileHandle, offsetof(WaveHeaderExtensible, dwSampleLength), SEEK_SET);
        _fwrite_nolock(&ckSize, 1, sizeof(DWORD), fileHandle);
    }
    else
    { 
        DWORD ckSize = bytesWritten - 8;
        _fseek_nolock(fileHandle, offsetof(WaveHeaderEx, wavSize), SEEK_SET);
        _fwrite_nolock(&ckSize, 1, sizeof(DWORD), fileHandle);

        ckSize = bytesWritten - sizeof(WaveHeaderEx);
        _fseek_nolock(fileHandle, offsetof(WaveHeaderEx, dataSize), SEEK_SET);
        _fwrite_nolock(&ckSize, 1, sizeof(DWORD), fileHandle);

        ckSize /= 4;
        _fseek_nolock(fileHandle, offsetof(WaveHeaderEx, dwSampleLength), SEEK_SET);
        _fwrite_nolock(&ckSize, 1, sizeof(DWORD), fileHandle);
    }    
    
    _fclose_nolock(fileHandle);
    fileHandle = NULL; 
}