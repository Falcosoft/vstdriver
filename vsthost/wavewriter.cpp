// Simple Wave writer utility class. Only native VSTi 32-bit float format is supported.
// Copyright (C) 2024 Zoltan Bacsko - Falcosoft

#include "stdafx.h"
#include <stddef.h>
#include <mmreg.h>
#include "wavewriter.h"
#include "../version.h"
#include "../external_packages/audiodefs.h"


WaveWriter::WaveWriter() :
    buffer(),
    startEvent(),
	workEvent(),
    hThread(),
    bufferPosition(),
    bufferPart(),
    fileHandle(),
    bytesWritten(),
    channelCount(),
    isRecordingStarted(),
    stopProcessing(),
    msgWindow(),
    msg()
{
    _tcscpy_s(fileName, L"VSTiCapture.wav");   
}

uint32_t WaveWriter::Init(int chCount, DWORD sampleRate, HWND ownerWindow, HWND messageWindow, DWORD message)
{	  
    OPENFILENAME ofn = { 0 };
    ofn.lStructSize = IsWinNT4() ? OPENFILENAME_SIZE_VERSION_400 : sizeof(ofn);
    ofn.hwndOwner = ownerWindow;
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
        
        buffer = (char*)malloc(Buffer_Size);
        if (!buffer) return WaveResponse::CannotCreate;
        bufferPosition = 0;  

        stopProcessing = false;
		startEvent = CreateEvent(NULL, false, false, NULL);  
		workEvent = CreateEvent(NULL, false, false, NULL);  

		hThread = (HANDLE)_beginthreadex(NULL, 8192, &WritingThread, this, 0, NULL);
		
		if(!hThread || !startEvent || !workEvent) return WaveResponse::CannotCreate;

		DWORD retRes = WaitForSingleObject(startEvent, 2000);
        if(retRes != WAIT_OBJECT_0) return WaveResponse::CannotCreate;
	    
        channelCount = chCount;
        msgWindow = messageWindow;
        msg = message;        
        bytesWritten = 0;       
          
       
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
                     
            WriteDataToDisk(&waveHeaderEx, sizeof(waveHeaderEx));
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
               
            WriteDataToDisk(&waveHeaderEx, sizeof(waveHeaderEx));
        }     
               
		isRecordingStarted = true;        

        return WaveResponse::Success;
    }  
    return WaveResponse::NotSelected;
}

void WaveWriter::WriteData(const void* data, DWORD size)
{       
    
    if (size < Buffer_Size / Buffer_Part_Count)
    {
        DWORD oldPart = (bufferPosition / (Buffer_Size / Buffer_Part_Count)) & (Buffer_Part_Count -1);
        
        if (bufferPosition + size <= Buffer_Size)
        {
            memcpy(buffer + bufferPosition, data, size);
            bufferPosition += size;
        }
        else
        {
            DWORD diff = Buffer_Size - bufferPosition;
            memcpy(buffer + bufferPosition, data, diff);
            memcpy(buffer, (char*)data + diff, size - diff);

            bufferPosition = size - diff;
        }

        DWORD newPart = (bufferPosition / (Buffer_Size / Buffer_Part_Count)) & (Buffer_Part_Count - 1);
        if(oldPart != newPart)
        {
            bufferPart = newPart;
            SetEvent(workEvent); //send write request to writing thread.
        }
    }
    else //this should never happen, but...
    {
        stopProcessing = true;       
        PostMessage(msgWindow, WM_COMMAND, (WPARAM)msg, 0);
    }      
}

uint32_t WaveWriter::WriteDataToDisk(const void* data, DWORD size)
{   
    if (!fileHandle || bytesWritten >= 0xFFF80000) return WaveResponse::WriteError; //Check for 4G - 512K to prevent creating unplayable 4GB+ wave files.
    DWORD sizeDone;
    WriteFile(fileHandle, data, size, &sizeDone, NULL);
    bytesWritten += sizeDone;
    if (sizeDone != size) return WaveResponse::WriteError;

    return WaveResponse::Success;
}

void WaveWriter::Close()
{
    if(!fileHandle) return;

    isRecordingStarted = false;
    stopProcessing = true;
    SetEvent(workEvent);
    if (hThread != NULL) {
        WaitForSingleObject(hThread, 5000);
        CloseHandle(hThread);
        hThread = NULL;
    }

    DWORD writeSize = bufferPosition - (bufferPart * (Buffer_Size / Buffer_Part_Count));
    if (writeSize > 0) WriteDataToDisk(buffer + (bufferPart * (Buffer_Size / Buffer_Part_Count)), writeSize);
   
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
    
    if (buffer)
    {
        free(buffer);
        buffer = NULL;
    }
    if (workEvent)
    {
        CloseHandle(workEvent);
        workEvent = NULL;
    }
	if (startEvent)
    {
        CloseHandle(startEvent);
        startEvent = NULL;
    }
}

unsigned __stdcall WaveWriter::WritingThread(void* pthis)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    WaveWriter* _this = static_cast<WaveWriter*>(pthis);
	
	SetEvent(_this->startEvent);

    for (;;)
    {
        WaitForSingleObject(_this->workEvent, INFINITE);
        if (_this->stopProcessing) break;
        
        if (!_this->fileHandle || _this->bytesWritten >= 0xFFF80000) //Check for 4G - 512K to prevent creating unplayable 4GB+ wave files.
        {                    
            PostMessage(_this->msgWindow, WM_COMMAND, (WPARAM)_this->msg, 0);
            break;
        }

        DWORD sizeDone;
        DWORD tmpPart = _this->bufferPart == 0 ? Buffer_Part_Count - 1  : _this->bufferPart - 1;
        WriteFile(_this->fileHandle, _this->buffer + (tmpPart * (Buffer_Size / Buffer_Part_Count)), Buffer_Size / Buffer_Part_Count, &sizeDone, NULL);
        _this->bytesWritten += sizeDone;
       
        if (sizeDone != Buffer_Size / Buffer_Part_Count)
        {                  
            PostMessage(_this->msgWindow, WM_COMMAND, (WPARAM)_this->msg, 0);
            break;
        }
    }

    _endthreadex(0);
    return 0;
}