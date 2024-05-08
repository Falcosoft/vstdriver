// Simple Wave writer utility class. Only native VSTi 32-bit float format is supported.
// Copyright (C) 2024 Zoltan Bacsko - Falcosoft

#pragma once

#if(_WIN32_WINNT < 0x0500)
#define OPENFILENAME_SIZE_VERSION_400A  CDSIZEOF_STRUCT(OPENFILENAMEA,lpTemplateName)
#define OPENFILENAME_SIZE_VERSION_400W  CDSIZEOF_STRUCT(OPENFILENAMEW,lpTemplateName)
#ifdef UNICODE
#define OPENFILENAME_SIZE_VERSION_400  OPENFILENAME_SIZE_VERSION_400W
#else
#define OPENFILENAME_SIZE_VERSION_400  OPENFILENAME_SIZE_VERSION_400A
#endif 
#endif 

#include "stdafx.h"
#include <commdlg.h>
#include <process.h>

#pragma pack(push, 1)
typedef struct wav_header_ex {
    // RIFF Header
    char riffHeader[4]; 
    uint32_t wavSize; 
    // WAVE Header
    char waveHeader[4]; 

    //fmt Header
    char fmtHeader[4]; 
    uint32_t fmtChunkSize; 
    uint16_t wFormatTag; 
    uint16_t nChannels;
    uint32_t nSamplesPerSec;
    uint32_t nAvgBytesPerSec; 
    uint16_t nBlockAlign; 
    uint16_t wBitsPerSample; 
    uint16_t cbSize;

    //fact
    char factHeader[4]; 
    uint32_t factSize;  
    uint32_t dwSampleLength; 

    // data
    char dataHeader[4]; 
    uint32_t dataSize;     

} WaveHeaderEx;

typedef struct wav_header_extensible {
    // RIFF Header
    char riffHeader[4];
    uint32_t wavSize;
    // WAVE Header
    char waveHeader[4];

    //fmt Header
    char fmtHeader[4];
    uint32_t fmtChunkSize;
    uint16_t wFormatTag;
    uint16_t nChannels;
    uint32_t nSamplesPerSec;
    uint32_t nAvgBytesPerSec;
    uint16_t nBlockAlign;
    uint16_t wBitsPerSample;
    uint16_t cbSize;
    uint16_t wValidBitsPerSample;
    uint32_t dwChannelMask;
    GUID SubFormat;

    //fact
    char factHeader[4]; 
    uint32_t factSize;  
    uint32_t dwSampleLength; 

    // data
    char dataHeader[4]; 
    uint32_t dataSize;     

} WaveHeaderExtensible;

#pragma pack(pop)

namespace WaveResponse {
    enum : uint32_t
    {
        CannotCreate = 0,
        Success = 1,
        NotSelected = 2,
        WriteError = 3,
    };
};

class WaveWriter
{
public: 
    WaveWriter();
    uint32_t Init(int chCount, DWORD sampleRate, HWND ownerWindow, HWND messageWindow, DWORD message);
    void WriteData(const void* data, DWORD size);
    void Close();    
    inline bool getIsRecordingStarted() { return isRecordingStarted; }   

private:
    const static DWORD Buffer_Size = 512 * 1024; 
    const static DWORD Buffer_Part_Count = 4; //quad buffering, 128K parts

    volatile bool isRecordingStarted;
    volatile bool stopProcessing;
    volatile DWORD bufferPart;
    volatile DWORD bytesWritten;
    HWND msgWindow;
    DWORD msg;
    char* buffer;    
    DWORD bufferPosition;   
    HANDLE fileHandle;
    int channelCount; 
    HANDLE workEvent;
	HANDLE startEvent;
    HANDLE hThread;
    TCHAR fileName[MAX_PATH];     

    uint32_t WriteDataToDisk(const void* data, DWORD size);  
    WaveWriter(const WaveWriter& that);
    WaveWriter& operator=(const WaveWriter& that);

    static unsigned __stdcall WritingThread(void* pthis);

};

