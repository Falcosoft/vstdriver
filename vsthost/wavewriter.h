// Simple Wave writer utility class. Only native VSTi 32-bit float format is supported.
// Copyright (C) 2024 Zoltan Bacsko - Falcosoft

#pragma once

#include "stdafx.h"
#include <commdlg.h>

bool IsWinNT4();

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
    uint32_t Init(int chCount, DWORD sampleRate);
    uint32_t WriteData(const void* data, DWORD size);
    void Close();   

private:
    FILE* fileHandle;
    DWORD bytesWritten;
    int channelCount;   
    TCHAR fileName[MAX_PATH];   
    WaveWriter(const WaveWriter& that);
    WaveWriter& operator=(const WaveWriter& that); 
};
