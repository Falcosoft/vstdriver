// Simple Wave writer utility class. Only native VSTi 32-bit float format is supported.
// Copyright (C) 2024 Zoltan Bacsko - Falcosoft

#include "stdafx.h"
#include <stddef.h>
#include "wavewriter.h"
#include "../version.h"
#include "../external_packages/audiodefs.h"

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
        fileHandle = CreateFile(fileName, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (fileHandle == INVALID_HANDLE_VALUE)
        {
            fileHandle = NULL;
            return WaveResponse::CannotCreate;
        }       

        bytesWritten = 0;        
        isCloseRequested = false;
        channelCount = chCount; 
        uint32_t writeResult = WaveResponse::Success;       
       
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
                     
            writeResult = WriteData(&waveHeaderEx, sizeof(waveHeaderEx));     
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
               
            writeResult = WriteData(&waveHeaderEx, sizeof(waveHeaderEx)); 
        }       
        
        isRecordingStarted = (writeResult == WaveResponse::Success);
        return writeResult;
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
    DWORD sizeDone;
    WriteFile(fileHandle, data, size, &sizeDone, NULL);
    bytesWritten += sizeDone;
    if (sizeDone != size) return WaveResponse::WriteError;

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

    OVERLAPPED ovr[3] = { 0 };
    DWORD sizeDone[3] = { 0 };
    DWORD ckSize[3] = { 0 };

    if (channelCount == 4)
    {        
        ckSize[0] = bytesWritten - 8;
        ckSize[2] = bytesWritten - sizeof(WaveHeaderExtensible);
        ckSize[1] = ckSize[2] / 4;        

        ovr[0].Offset = offsetof(WaveHeaderExtensible, wavSize);
        WriteFile(fileHandle, &ckSize[0], sizeof(DWORD), &sizeDone[0], &ovr[0]);       
       
        ovr[1].Offset = offsetof(WaveHeaderExtensible, dwSampleLength);      
        WriteFile(fileHandle, &ckSize[1], sizeof(DWORD), &sizeDone[1], &ovr[1]);  

        ovr[2].Offset = offsetof(WaveHeaderExtensible, dataSize);
        WriteFile(fileHandle, &ckSize[2], sizeof(DWORD), &sizeDone[2], &ovr[2]); 
    }
    else
    { 
        ckSize[0] = bytesWritten - 8;
        ckSize[2] = bytesWritten - sizeof(WaveHeaderEx);
        ckSize[1] = ckSize[2] / 4;

        ovr[0].Offset = offsetof(WaveHeaderEx, wavSize);
        WriteFile(fileHandle, &ckSize[0], sizeof(DWORD), &sizeDone[0], &ovr[0]);

        ovr[1].Offset = offsetof(WaveHeaderEx, dwSampleLength);
        WriteFile(fileHandle, &ckSize[1], sizeof(DWORD), &sizeDone[1], &ovr[1]);

        ovr[2].Offset = offsetof(WaveHeaderEx, dataSize);
        WriteFile(fileHandle, &ckSize[2], sizeof(DWORD), &sizeDone[2], &ovr[2]);
    }    
    
    CloseHandle(fileHandle);  
    fileHandle = NULL;     
}