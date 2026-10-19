// Simple Wave writer utility class. Only native VSTi 32-bit float format is supported.
// Copyright (C) 2024 Zoltan Bacsko - Falcosoft

#include "stdafx.h"
#include <stddef.h>
#include <mmreg.h>
#include "wavewriter.h"
#include "../version.h"
#include "../external_packages/audiodefs.h"


WaveWriter::WaveWriter() :
	controlLock(true),
	bufferLock(true),
	Buffer_Size(),
	Buffer_Part_Count(),
	buffer(),
	writeBuffer(),
	workEvent(),
	hThread(),
	bufferPosition(),
	bufferedBytes(),
	bufferReadPosition(),
	fileHandle(),
	bytesWritten(),
	channelCount(),
	isRecordingStarted(),
	stopProcessing(),
	errorPending(),
	errorReported(),
	msgWindow(),
	msg()
{
	_tcscpy_s(fileName, _T("VSTiCapture.wav"));
}

uint32_t WaveWriter::Init(int chCount, DWORD sampleRate, HWND ownerWindow, HWND messageWindow, DWORD message)
{
	ScopeLock<Win32Lock> controlScope(&controlLock);
	// A timed-out Close retains ownership until the old writer really exits.
	if (fileHandle || buffer || writeBuffer || hThread || workEvent) Close();
	if (fileHandle || buffer || writeBuffer || hThread || workEvent) return WaveResponse::CannotCreate;
	if ((chCount != 2 && chCount != 4) || !sampleRate) return WaveResponse::CannotCreate;
	// Preserve the audio capacity of 1 MiB at 48 kHz stereo (about 2.73 seconds).
	// Round up to whole 128K parts, with at least the original 8 parts.
	UINT64 parts = ((UINT64)sampleRate * chCount * 8 + 95999) / 96000;
	if (parts < 8) parts = 8;
	// Reject excessive formats before showing the dialog or truncating a file.
	if (parts > Buffer_Max_Size / Buffer_Part_Size) return WaveResponse::CannotCreate;
	OPENFILENAME ofn = { 0 };
	ofn.lStructSize = IsWinNT4() ? OPENFILENAME_SIZE_VERSION_400 : sizeof(ofn);
	ofn.hwndOwner = ownerWindow;
	ofn.lpstrFilter = _T("Wave Files (*.wav)\0*.wav\0All Files (*.*)\0*.*\0");
	ofn.lpstrFile = fileName;
	ofn.nMaxFile = MAX_PATH;
	ofn.Flags = OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_OVERWRITEPROMPT | OFN_EXPLORER | OFN_ENABLEHOOK | OFN_ENABLESIZING;
	ofn.lpstrDefExt = _T("wav");

	if (GetSaveFileName(&ofn))
	{
		fileHandle = CreateFile(fileName, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (fileHandle == INVALID_HANDLE_VALUE)
		{
			fileHandle = NULL;
			return WaveResponse::CannotCreate;
		}

		channelCount = chCount;
		Buffer_Part_Count = (DWORD)parts;
		Buffer_Size = Buffer_Part_Count * Buffer_Part_Size;
		bytesWritten = 0;
		bufferPosition = bufferReadPosition = bufferedBytes = 0;
		buffer = (char*)malloc(Buffer_Size);
		writeBuffer = (char*)malloc(Buffer_Part_Size);
		if (!buffer || !writeBuffer) {
			ReleaseResources();
			return WaveResponse::CannotCreate;
		}

		stopProcessing = false;
		errorPending = errorReported = false;
		workEvent = CreateEvent(NULL, false, false, NULL);

		if (!workEvent) {
			ReleaseResources();
			return WaveResponse::CannotCreate;
		}

		msgWindow = messageWindow;
		msg = message;

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

			if (WriteDataToDisk(&waveHeaderEx, sizeof(waveHeaderEx)) != WaveResponse::Success) {
				ReleaseResources();
				return WaveResponse::CannotCreate;
			}
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

			if (WriteDataToDisk(&waveHeaderEx, sizeof(waveHeaderEx)) != WaveResponse::Success) {
				ReleaseResources();
				return WaveResponse::CannotCreate;
			}
		}

		// Do not publish recording or start the consumer before initialization completes.
		hThread = (HANDLE)_beginthreadex(NULL, 8192, &WritingThread, this, 0, NULL);
		if (!hThread) {
			ReleaseResources();
			return WaveResponse::CannotCreate;
		}
		{
			ScopeLock<Win32Lock> bufferScope(&bufferLock);
			isRecordingStarted = true;
		}

		return WaveResponse::Success;
	}
	return WaveResponse::NotSelected;
}

bool WaveWriter::getIsRecordingStarted()
{
	ScopeLock<Win32Lock> bufferScope(&bufferLock);
	return isRecordingStarted;
}

bool WaveWriter::TakeRecordingError()
{
	ScopeLock<Win32Lock> bufferScope(&bufferLock);
	bool pending = errorPending;
	errorPending = false;
	return pending;
}

void WaveWriter::WriteData(const void* data, DWORD size)
{
	ScopeLock<Win32Lock> bufferScope(&bufferLock);
	// Recheck after acquiring the lock: the caller's earlier flag read can be stale.
	if (!isRecordingStarted || stopProcessing || !buffer || !size) return;
	if (!data || size > Buffer_Size - bufferedBytes) {
		// Preserve the queued prefix. Never overwrite unread data or wait on disk.
		OutputDebugString(_T("VST MIDI Driver: recording buffer overrun; stopping recording\n"));
		StopRecordingOnError();
		return;
	}
	DWORD first = size < Buffer_Size - bufferPosition ? size : Buffer_Size - bufferPosition;
	memcpy(buffer + bufferPosition, data, first);
	if (size > first) memcpy(buffer, (const char*)data + first, size - first);
	bufferPosition = (bufferPosition + size) % Buffer_Size;
	bufferedBytes += size;
	if (bufferedBytes >= Buffer_Part_Size) SetEvent(workEvent); //send write request to writing thread.
}

void WaveWriter::StopRecordingOnError()
{
	// bufferLock explicitly uses a recursive critical section on every OS.
	ScopeLock<Win32Lock> bufferScope(&bufferLock);
	bool notify = !errorReported;
	if (notify) errorPending = errorReported = true;
	isRecordingStarted = false;
	stopProcessing = true;
	if (workEvent) SetEvent(workEvent);
	if (notify) PostMessage(msgWindow, WM_COMMAND, (WPARAM)msg, 0);
}

uint32_t WaveWriter::WriteDataToDisk(const void* data, DWORD size)
{
	if (!fileHandle || bytesWritten >= 0xFFF80000) return WaveResponse::WriteError; //Check for 4G - 512K to prevent creating unplayable 4GB+ wave files.
	DWORD sizeDone = 0;
	BOOL success = WriteFile(fileHandle, data, size, &sizeDone, NULL);
	bytesWritten += sizeDone;
	if (!success || sizeDone != size) return WaveResponse::WriteError;

	return WaveResponse::Success;
}

void WaveWriter::Close()
{
	ScopeLock<Win32Lock> controlScope(&controlLock);
	{
		ScopeLock<Win32Lock> bufferScope(&bufferLock);
		if (!fileHandle && !buffer && !writeBuffer && !hThread && !workEvent) return;
		isRecordingStarted = false;
		stopProcessing = true;
		if (workEvent) SetEvent(workEvent);
	}
	// Never hold bufferLock while waiting for disk I/O. Audio can immediately
	// enter WriteData, see recording stopped, and return without waiting on disk.
	if (hThread != NULL) {
		if (WaitForSingleObject(hThread, 5000) != WAIT_OBJECT_0) {
			// Keep ALL resources alive; a later Close/Init can retry cleanup.
			OutputDebugString(_T("VST MIDI Driver: recording writer still stopping\n"));
			return;
		}
		CloseHandle(hThread);
		hThread = NULL;
	}

	OVERLAPPED ovr[3] = { 0 };
	DWORD sizeDone[3] = { 0 };
	DWORD ckSize[3] = { 0 };

	if (channelCount == 4)
	{
		ckSize[0] = bytesWritten - 8;
		ckSize[2] = bytesWritten - sizeof(WaveHeaderExtensible);
		ckSize[1] = ckSize[2] / (channelCount * 4);

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
		ckSize[1] = ckSize[2] / (channelCount * 4);

		ovr[0].Offset = offsetof(WaveHeaderEx, wavSize);
		WriteFile(fileHandle, &ckSize[0], sizeof(DWORD), &sizeDone[0], &ovr[0]);

		ovr[1].Offset = offsetof(WaveHeaderEx, dwSampleLength);
		WriteFile(fileHandle, &ckSize[1], sizeof(DWORD), &sizeDone[1], &ovr[1]);

		ovr[2].Offset = offsetof(WaveHeaderEx, dataSize);
		WriteFile(fileHandle, &ckSize[2], sizeof(DWORD), &sizeDone[2], &ovr[2]);
	}

	ReleaseResources();
}

void WaveWriter::ReleaseResources()
{
	if (fileHandle) CloseHandle(fileHandle);
	fileHandle = NULL;
	if (buffer) free(buffer);
	buffer = NULL;
	if (writeBuffer) free(writeBuffer);
	writeBuffer = NULL;
	bufferedBytes = bufferPosition = bufferReadPosition = 0;
	Buffer_Size = Buffer_Part_Count = 0;
	if (workEvent) CloseHandle(workEvent);
	workEvent = NULL;
}

unsigned __stdcall WaveWriter::WritingThread(void* pthis)
{
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
	WaveWriter* self = static_cast<WaveWriter*>(pthis);
	for (;;) {
		if (WaitForSingleObject(self->workEvent, INFINITE) != WAIT_OBJECT_0) {
			self->StopRecordingOnError();
			return 0;
		}
		for (;;) {
			DWORD count = 0;
			{
				ScopeLock<Win32Lock> bufferScope(&self->bufferLock);
				DWORD part = Buffer_Part_Size;
				if (!self->bufferedBytes) {
					if (self->stopProcessing) return 0;
					break;
				}
				if (self->bufferedBytes < part && !self->stopProcessing) break;
				count = self->bufferedBytes < part ? self->bufferedBytes : part;
				DWORD first = count < self->Buffer_Size - self->bufferReadPosition ? count : self->Buffer_Size - self->bufferReadPosition;
				memcpy(self->writeBuffer, self->buffer + self->bufferReadPosition, first);
				if (count > first) memcpy(self->writeBuffer + first, self->buffer, count - first);
				self->bufferReadPosition = (self->bufferReadPosition + count) % self->Buffer_Size;
				self->bufferedBytes -= count;
			}
			// The producer can reuse the ring immediately. This private copy stays
			// alive even if Close times out while WriteFile is still in progress.
			if (self->WriteDataToDisk(self->writeBuffer, count) != WaveResponse::Success) {
				self->StopRecordingOnError();
				return 0;
			}
		}
	}
}
