/* Copyright (C) 2011, 2012 Sergey V. Mikayev
*  Copyright (C) 2023 Zoltan Bacsko - Falcosoft
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU Lesser General Public License as published by
* the Free Software Foundation, either version 2.1 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public License
* along with this program. If not, see <http://www.gnu.org/licenses/>.
*/


#include "stdafx.h"
#include "VSTDriver.h"
#include "AudioDrivers.h"
#include <string>
#include <math.h>
#include <new>
#include "../common/LegacySafety.h"

#ifdef _DEBUG
#define _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>
#endif

#undef GetMessage

namespace VSTMIDIDRV {

	MidiSynth& midiSynth = MidiSynth::getInstance();

	static Win32Lock synthLock;
	static Win32Lock lifecycleLock(true); // Init/Close/reset serialization; recursive on NT4 too.

#pragma region Utility_Functions
	DWORD GetDwordData(LPCTSTR valueName, DWORD defaultValue)
	{
		HKEY hKey;
		DWORD retResult = defaultValue;

		long result = RegOpenKeyEx(HKEY_CURRENT_USER, _T("Software\\VSTi Driver"), 0, KEY_READ, &hKey);
		if (result == NO_ERROR)
		{
			DWORD size = sizeof(retResult);
			result = RegQueryValueEx(hKey, valueName, NULL, NULL, (LPBYTE)&retResult, &size);
			if (result == NO_ERROR)
			{
				RegCloseKey(hKey);
				return retResult;
			}

			RegCloseKey(hKey);
		}

		return retResult;
	}

	DWORD GetSampleRate() {
		DWORD sampleRate = 48000;
		sampleRate = GetDwordData(_T("SampleRate"), sampleRate);
		return sampleRate;
	}

	DWORD GetBufferSize() {
		DWORD bufferSize = 80;
		bufferSize = GetDwordData(_T("BufferSize"), bufferSize);
		return bufferSize;
	}

	int GetGain() {
		int gain = 0;
		gain = GetDwordData(_T("Gain"), gain);
		return gain;
	}

	WORD GetChannelCount() {
		BOOL enabled = FALSE;
		enabled = GetDwordData(_T("Use4ChannelMode"), enabled);
		return enabled ? 4 : 2;
	}

	bool IsShowVSTDialog() {
		BOOL showVstDialog = FALSE;
		showVstDialog = GetDwordData(_T("ShowVstDialog"), showVstDialog);
		return showVstDialog != FALSE;
	}

	bool GetUsingFloat() {
		BOOL usingFloat = TRUE;
		usingFloat = GetDwordData(_T("UseFloat"), usingFloat);
		return usingFloat != FALSE;
	}

	DWORD GetHighDpiMode() {
		DWORD highDpiMode = 0;
		highDpiMode = GetDwordData(_T("HighDpiMode"), highDpiMode);
		return highDpiMode;
	}

	bool GetEnableSinglePort32ChMode() {
		BOOL enableSinglePort32ChMode = TRUE;
		enableSinglePort32ChMode = GetDwordData(_T("EnableSinglePort32ChMode"), enableSinglePort32ChMode);
		return enableSinglePort32ChMode != FALSE;
	}

	bool GetKeepDriverLoaded() {
		BOOL keepDriverLoaded = TRUE;
		keepDriverLoaded = GetDwordData(_T("KeepDriverLoaded"), keepDriverLoaded);
		return keepDriverLoaded != FALSE;
	}

	template <typename T>
	inline T Min(T a, T b) //min macro calls bassAsioOut->GetPos() 2 times that can cause problems...
	{
		if (a < b) return a;
		return b;
	}

	template <typename T>
	inline T Max(T a, T b)
	{
		if (a > b) return a;
		return b;
	}
#pragma endregion Utility functions



#pragma region MidiStream class	
	MidiStream::MidiStream() : startpos(0), endpos(0), droppedCount(0), stream() {
	}

	MidiStream::~MidiStream() {
		Reset();
	}

	DWORD MidiStream::ReportDrop() {
		if (!droppedCount) OutputDebugString(_T("VST MIDI Driver: MIDI queue full or SysEx allocation failed; dropping new events\n"));
		++droppedCount;
		return (DWORD)-1;
	}

	void MidiStream::Reset(DWORD port) {
		unsigned read = startpos, write = startpos;
		while (read != endpos) {
			message& item = stream[read];
			if (port == (DWORD)-1 || (item.port_type & 0x7fffffff) == port) {
				if ((item.port_type & 0x80000000) && item.sysex) free(item.sysex);
			}
			else {
				if (write != read) stream[write] = item;
				write = (write + 1) % maxPos;
			}
			read = (read + 1) % maxPos;
		}
		endpos = write;
		droppedCount = 0;
	}

	DWORD MidiStream::PutMessage(DWORD port, DWORD msg, DWORD timestamp) {
		unsigned int newEndpos = endpos;
		newEndpos++;
		if (newEndpos == maxPos) // check for buffer rolloff
			newEndpos = 0;
		if (startpos == newEndpos) // check for buffer full
			return ReportDrop();
		stream[endpos].sysex = NULL;
		stream[endpos].msg = msg; // ok to put data and update endpos
		stream[endpos].timestamp = timestamp;
		stream[endpos].port_type = port;
		endpos = newEndpos;
		return 0;
	}

	DWORD MidiStream::PutSysEx(DWORD port, const unsigned char* sysex, DWORD sysex_len, DWORD timestamp) {
		if (!sysex || !sysex_len || sysex_len > VSTDriverMaxSysExBytes) return (DWORD)-1;
		unsigned int newEndpos = endpos;
		void* sysexCopy;

		newEndpos++;
		if (newEndpos == maxPos)
			newEndpos = 0;
		if (startpos == newEndpos)
			return ReportDrop();

		sysexCopy = malloc(sysex_len);
		if (!sysexCopy)
			return ReportDrop();

		memcpy(sysexCopy, sysex, sysex_len);

		stream[endpos].sysex = sysexCopy;
		stream[endpos].msg = sysex_len;
		stream[endpos].timestamp = timestamp;
		stream[endpos].port_type = port | 0x80000000;
		endpos = newEndpos;
		return 0;
	}

	void MidiStream::GetMessage(DWORD& port, DWORD& message, unsigned char*& sysex, DWORD& sysex_len) {
		if (startpos == endpos) // check for buffer empty
		{
			port = 0;
			message = 0;
			sysex = NULL;
			sysex_len = 0;
			return;
		}
		port = stream[startpos].port_type & 0x7fffffff;
		if (stream[startpos].port_type & 0x80000000)
		{
			message = 0;
			sysex = (unsigned char*)stream[startpos].sysex;
			sysex_len = stream[startpos].msg;
		}
		else
		{
			message = stream[startpos].msg;
			sysex = NULL;
			sysex_len = 0;
		}
		stream[startpos].sysex = NULL; // Ownership of a dequeued SysEx passes to the renderer.
		startpos++;
		if (startpos == maxPos) // check for buffer rolloff
			startpos = 0;
	}

	DWORD MidiStream::PeekMessageTime() {
		if (startpos == endpos) // check for buffer empty
			return (DWORD)-1;
		return stream[startpos].timestamp;
	}

	DWORD MidiStream::PeekMessageTimeAt(unsigned int pos) {
		if (startpos == endpos) // check for buffer empty
			return -1;
		unsigned int peekPos = (startpos + pos) % maxPos;
		return stream[peekPos].timestamp;
	}

	DWORD MidiStream::PeekMessageCount() {
		if (endpos < startpos) {
			return endpos + maxPos - startpos;
		}
		else {
			return endpos - startpos;
		}
	}
#pragma endregion 

#pragma region MidiSynth
	MidiSynth::MidiSynth() :
		settingsLoaded(false),
		configuredSampleRate(),
		configuredKeepLoaded(),
		midiStream(new MidiStream),
		statusBuff(),
		midiVol(),
		lastOpenedPort(),
		vstDriver(),
		waveOut(),
		bassAsioOut(),
		virtualPortNum(),
		usingFloat(),
		useAsio(),
		sampleRate(),
		outputGain(),
		midiLatency(),
		midiLatencyMS(),
		keepLoaded(),
		isSinglePort32Ch(),
		framesRendered(),
		stopping(1),
		resetPending(),
		enableSinglePort32ChMode(),
		chunkSize(),
		chunkSizeMS(),
		channels(),
		bufferSize(),
		bufferSizeMS(),
		bufferCapacity(),
		bufferfCapacity(),
		bufferf(),
		buffer()
	{
#ifdef _DEBUG
		_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_WNDW);
		_CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_WNDW);
#endif
	}

	MidiSynth::~MidiSynth() {
		if (waveOut) {
			delete waveOut;
			waveOut = NULL;
		}

		if (bufferf) {
			delete[] bufferf;
			bufferf = NULL;
		}
		if (buffer) {
			delete[] buffer;
			buffer = NULL;
		}

		if (bassAsioOut) {
			delete bassAsioOut;
			bassAsioOut = NULL;
		}
		if (midiStream) {
			delete midiStream;
			midiStream = NULL;
		}

#ifdef _DEBUG
		_CrtDumpMemoryLeaks();
#endif
	}

	MidiSynth& MidiSynth::getInstance() {
		static MidiSynth instance;
		return instance;
	}

	void MidiSynth::LoadSettings() {
		// Keep these settings for this MidiSynth instance, including plugin switches.
		// A replacement bridge still loads the currently selected plugin separately.
		if (settingsLoaded) return;

		channels = GetChannelCount();
		outputGain = pow(10.0f, GetGain() * 0.05f);
		configuredSampleRate = GetSampleRate();
		if (!configuredSampleRate) configuredSampleRate = 48000;
		bufferSizeMS = GetBufferSize();
		chunkSizeMS = bufferSizeMS < 120 ? bufferSizeMS / 4 : bufferSizeMS < 200 ? bufferSizeMS / 5 : bufferSizeMS / 8;
		midiLatencyMS = 0;

		usingFloat = GetUsingFloat();
		useAsio = UseAsio();
		enableSinglePort32ChMode = GetEnableSinglePort32ChMode();
		configuredKeepLoaded = GetKeepDriverLoaded();
		settingsLoaded = true;
	}

	int MidiSynth::Init(unsigned uDeviceID) {

		ScopeLock<Win32Lock> lifecycleScope(&lifecycleLock);

		LoadSettings();

		// ASIO may negotiate a different rate; SC-VA may force keepLoaded.
		// Start every initialization from the saved user settings.
		sampleRate = configuredSampleRate;
		keepLoaded = configuredKeepLoaded;

		UINT64 bufferFrames = (UINT64)sampleRate * bufferSizeMS / 1000;
		UINT64 chunkFrames = (UINT64)sampleRate * chunkSizeMS / 1000;
		if (bufferFrames > MAXDWORD || chunkFrames > MAXDWORD)
			return MMSYSERR_INVALPARAM;

		chunkSize = (unsigned int)chunkFrames;
		if (chunkSize) {
			// Round up without overflowing the frame-count arithmetic.
			bufferFrames = ((bufferFrames + chunkSize - 1) / chunkSize) * chunkSize;
			if (bufferFrames > MAXDWORD) return MMSYSERR_INVALPARAM;
		}
		bufferSize = (unsigned int)bufferFrames;
		midiLatency = MillisToFrames(midiLatencyMS);

		if (!useAsio && (!bufferSize || !chunkSize)) {
			bufferSize = MillisToFrames(80);
			chunkSize = bufferSize / 4;
			if (!bufferSize || !chunkSize) return MMSYSERR_INVALPARAM;
		}

		midiVol[0] = 1.0f;
		midiVol[1] = 1.0f;
		statusBuff[0] = 0;
		statusBuff[1] = 0;
		isSinglePort32Ch = false;
		virtualPortNum = 0;

		UINT wResult = 0;
		if (useAsio)
		{
			if (!bassAsioOut) bassAsioOut = new BassAsioOut(this);

			wResult = bassAsioOut->Init(bufferSizeMS, sampleRate, usingFloat, channels);
			if (wResult) bassAsioOut->Close();
			else
			{
				sampleRate = bassAsioOut->getSampleRate();
				if (bassAsioOut->getIsASIO2WASAPI()) bufferSize = 0;

				if (!bufferSize)
					bufferSize = bassAsioOut->getBufPref();
				else
					bufferSize = MillisToFrames(bufferSizeMS);
			}
		}
		else
		{
			if (!waveOut) waveOut = new WaveOutWin32(this);

			// Keep allocation and byte-count arithmetic valid on 32-bit builds too.
			UINT64 requiredSamples = (UINT64)channels * bufferSize;
			UINT64 requiredBytes = requiredSamples * (usingFloat ? sizeof(float) : sizeof(short));
			if (!requiredSamples || requiredBytes > MAXDWORD)
				return MMSYSERR_INVALPARAM;
			unsigned int capacityNeeded = (unsigned int)requiredSamples;

			// Init runs before output starts. Replace only after allocation succeeds.
			if (usingFloat)
			{
				if (!bufferf || bufferfCapacity < capacityNeeded) {
					float* replacement = new (std::nothrow) float[capacityNeeded];
					if (!replacement) return MMSYSERR_NOMEM;
					delete[] bufferf;
					bufferf = replacement;
					bufferfCapacity = capacityNeeded;
				}
			}
			else
			{
				if (!buffer || bufferCapacity < capacityNeeded) {
					short* replacement = new (std::nothrow) short[capacityNeeded];
					if (!replacement) return MMSYSERR_NOMEM;
					delete[] buffer;
					buffer = replacement;
					bufferCapacity = capacityNeeded;
				}
			}

			wResult = waveOut->Init(usingFloat ? (void*)bufferf : (void*)buffer, bufferSize, chunkSize, sampleRate, usingFloat, channels);
		}

		if (wResult) return wResult;

		if (!vstDriver)
		{
			vstDriver = new VSTDriver;
			if (!vstDriver->OpenVSTDriver(NULL, sampleRate))
			{
				delete vstDriver;
				vstDriver = NULL;
				return 1;
			}

			vstDriver->setHighDpiMode(GetHighDpiMode());
			vstDriver->initSysTray();
		}

		keepLoaded = keepLoaded || vstDriver->getIsSCVA();

		if (uDeviceID == (DWORD)-1)
			uDeviceID = lastOpenedPort;
		else
			lastOpenedPort = uDeviceID;

		InitDialog(uDeviceID);

		framesRendered = 0;

		InterlockedExchange(&stopping, 0);

		// Start playing stream
		if (useAsio) {
			wResult = bassAsioOut->Start();
			if (wResult) bassAsioOut->Close();
		}
		else
		{
			if (usingFloat)
				memset(bufferf, 0, bufferSize * sizeof(float) * channels);
			else
				memset(buffer, 0, bufferSize * sizeof(short) * channels);

			wResult = waveOut->Start();
		}

		if (wResult) InterlockedExchange(&stopping, 1);
		return wResult;
	}

	// Renders totalFrames frames starting from bufpos
	// The number of frames rendered is added to the global counter framesRendered
	void MidiSynth::Render(short* bufpos, DWORD totalFrames) {
		while (totalFrames > 0) {
			if (stopping) {
				memset(bufpos, 0, (size_t)totalFrames * channels * sizeof(*bufpos));
				return;
			}
			DWORD timeStamp = (DWORD)-1;
			// Incoming MIDI messages timestamped with the current audio playback position + midiLatency
			while (!stopping) {
				{
					DWORD msg;
					DWORD sysex_len;
					DWORD port;
					unsigned char* sysex;

					ScopeLock<Win32Lock> scopeLock(&synthLock);
					timeStamp = midiStream->PeekMessageTime();
					if (stopping || timeStamp != framesRendered) break;

					midiStream->GetMessage(port, msg, sysex, sysex_len);

					if (msg && !sysex)
					{
						vstDriver->ProcessMIDIMessage(port, msg);
					}
					else if (!msg && sysex && sysex_len)
					{
						vstDriver->ProcessSysEx(port, sysex, sysex_len);
						free(sysex);
					}
				}

			}

			if (stopping) {
				memset(bufpos, 0, (size_t)totalFrames * channels * sizeof(*bufpos));
				return;
			}
			// Find out how many frames to render. The value of timeStamp == -1 indicates the MIDI buffer is empty
			DWORD framesToRender = timeStamp - framesRendered;
			if (framesToRender > totalFrames) {
				// MIDI message is too far - render the rest of frames
				framesToRender = totalFrames;
			}

			uint32_t result;
			{
				ScopeLock<Win32Lock> scopeLock(&synthLock);

				result = vstDriver->Render(bufpos, framesToRender, outputGain, channels);
			}


			framesRendered += framesToRender;
			bufpos += framesToRender * channels;
			totalFrames -= framesToRender;
			if (result == ResetRequest)
				RequestReset();

		}

		// Wrap framesRendered counter
		if (framesRendered >= bufferSize) {
			framesRendered -= bufferSize;
		}
	}

	void MidiSynth::RenderFloat(float* bufpos, DWORD totalFrames) {
		while (totalFrames > 0) {
			if (stopping) {
				memset(bufpos, 0, (size_t)totalFrames * channels * sizeof(*bufpos));
				return;
			}
			DWORD timeStamp = (DWORD)-1;
			// Incoming MIDI messages timestamped with the current audio playback position + midiLatency
			while (!stopping) {
				{
					DWORD msg;
					DWORD sysex_len;
					DWORD port;
					unsigned char* sysex;

					ScopeLock<Win32Lock> scopeLock(&synthLock);
					timeStamp = midiStream->PeekMessageTime();
					if (stopping || timeStamp != framesRendered) break;

					midiStream->GetMessage(port, msg, sysex, sysex_len);

					if (msg && !sysex)
					{
						vstDriver->ProcessMIDIMessage(port, msg);
					}
					else if (!msg && sysex && sysex_len)
					{
						vstDriver->ProcessSysEx(port, sysex, sysex_len);
						free(sysex);
					}
				}
			}

			if (stopping) {
				memset(bufpos, 0, (size_t)totalFrames * channels * sizeof(*bufpos));
				return;
			}
			// Find out how many frames to render. The value of timeStamp == -1 indicates the MIDI buffer is empty
			DWORD framesToRender = timeStamp - framesRendered;
			if (framesToRender > totalFrames) {
				// MIDI message is too far - render the rest of frames
				framesToRender = totalFrames;
			}

			uint32_t result;
			{
				ScopeLock<Win32Lock> scopeLock(&synthLock);

				result = vstDriver->RenderFloat(bufpos, framesToRender, outputGain, channels);
			}

			framesRendered += framesToRender;
			bufpos += framesToRender * channels;
			totalFrames -= framesToRender;
			if (result == ResetRequest)
				RequestReset();
		}

		// Wrap framesRendered counter
		if (framesRendered >= bufferSize) {
			framesRendered -= bufferSize;
		}
	}

	unsigned int MidiSynth::MillisToFrames(unsigned int millis) {
		return (unsigned int)((UINT64)sampleRate * millis / 1000);
	}

	void MidiSynth::InitDialog(unsigned uDeviceID) {
		ScopeLock<Win32Lock> lifecycleScope(&lifecycleLock);
		ScopeLock<Win32Lock> scopeLock(&synthLock);
		InitDialogInternal(uDeviceID);
	}

	void MidiSynth::InitDialogInternal(unsigned uDeviceID) {
		// Keep the complete editor request/reply exchange exclusive with rendering.
		// F5 preprocessing already holds synthLock, which may be nonrecursive.
		if (vstDriver && IsShowVSTDialog()) vstDriver->displayEditorModal(uDeviceID, (HWND)-1);
	}

	int MidiSynth::Reset(unsigned uDeviceID) {
		ScopeLock<Win32Lock> lifecycleScope(&lifecycleLock);

		//UINT wResult = useAsio ? bassAsioOut->Pause() : waveOut->Pause();
		//if (wResult) return wResult;

		{
			ScopeLock<Win32Lock> scopeLock(&synthLock);
			vstDriver->ResetDriver(uDeviceID);
			midiStream->Reset(isSinglePort32Ch ? (DWORD)-1 : uDeviceID);
			if (isSinglePort32Ch) statusBuff[0] = statusBuff[1] = 0;
			else statusBuff[uDeviceID] = 0;
			isSinglePort32Ch = false;
			virtualPortNum = 0;
		}

		//wResult = useAsio ? bassAsioOut->Resume() : waveOut->Resume();
		//return wResult;
		return 0;
	}

	bool MidiSynth::PreprocessMIDI(unsigned int& uDeviceID, DWORD& msg) {
		//// support for F5 xx port select message (FSMP can send this message)
		if ((unsigned char)(msg) == 0xF5 && enableSinglePort32ChMode) {
			if (!isSinglePort32Ch) {

				vstDriver->setSinglePort32ChMode();
				InitDialogInternal((unsigned int)!uDeviceID);
				isSinglePort32Ch = true;
			}
			//F5 xx command uses 1 as first port, but we use 0 and we have only A/B ports. This way 1 -> 0, 2 -> 1, 3 -> 0, 4 -> 1 and so on. 
			virtualPortNum = !(((unsigned char*)&msg)[1] & 1);

			return false;
		}

		if (isSinglePort32Ch) uDeviceID = virtualPortNum;

		////falco: running status support
		if ((unsigned char)msg >= 0x80 && (unsigned char)msg <= 0xEF) {
			statusBuff[uDeviceID] = (unsigned char)msg; //store status in case of normal Channel/Voice messages.
		}
		else if ((unsigned char)msg < 0x80 && statusBuff[uDeviceID] >= 0x80 && statusBuff[uDeviceID] <= 0xEF) {
			msg = (msg << 8) | statusBuff[uDeviceID]; //expand messages without status to full messages.								
		}
		else if ((unsigned char)msg > 0xF0 && (unsigned char)msg < 0xF7) {
			statusBuff[uDeviceID] = 0;  //clear running status in case of System Common messages				
		}
		else if ((unsigned char)msg < 0x80)
		{
			return false; //no valid status always means malformed Midi message 
		}


		////falco: midiOutSetVolume support
		if (midiVol[uDeviceID] != 1.0f) {
			if ((msg & 0xF0) == 0x90) {
				unsigned char velocity = ((unsigned char*)&msg)[2];
				velocity = (midiVol[uDeviceID] == 0.0 || velocity == 0) ? 0 : (unsigned char)Max<DWORD>(DWORD(velocity * midiVol[uDeviceID]), 1);
				((unsigned char*)&msg)[2] = velocity;
			}
		}

		return true;
	}

	bool MidiSynth::PreprocessSysEx(unsigned int& uDeviceID, const unsigned char* bufpos, const DWORD len) {

		//// support for F5 xx port select message (FSMP can send this message)
		if (bufpos[0] == 0xF5 && len > 1 && enableSinglePort32ChMode) {
			if (!isSinglePort32Ch) {

				vstDriver->setSinglePort32ChMode();
				InitDialogInternal((unsigned int)!uDeviceID);
				isSinglePort32Ch = true;
			}
			//F5 xx command uses 1 as first port, but we use 0 and we have only A/B ports. This way 1 -> 0, 2 -> 1, 3 -> 0, 4 -> 1 and so on.
			virtualPortNum = !(bufpos[1] & 1);

			return false;
		}

		if (isSinglePort32Ch) uDeviceID = virtualPortNum;

		statusBuff[uDeviceID] = 0; //clear running status also in case of SysEx messages

		return true;
	}

	void MidiSynth::PushMIDI(unsigned uDeviceID, DWORD msg) {

		ScopeLock<Win32Lock> scopeLock(&synthLock);
		if (stopping) return;

		if (!PreprocessMIDI(uDeviceID, msg))
			return;

		DWORD tmpTimestamp = useAsio ? Min<DWORD>(bassAsioOut->GetPos() + midiLatency, bufferSize - 1) : (DWORD)((waveOut->GetPos(channels) + midiLatency) % bufferSize);
		midiStream->PutMessage(uDeviceID, msg, tmpTimestamp);
	}

	void MidiSynth::PlaySysEx(unsigned uDeviceID, unsigned char* bufpos, DWORD len) {
		if (!bufpos || !len || len > VSTDriverMaxSysExBytes) {
			OutputDebugString(_T("VST MIDI Driver: invalid SysEx buffer rejected\n"));
			return;
		}

		//Short Midi channel messages can be also sent by midiOutLongMsg(). But VSTi plugins many times cannot handle short messages sent as kVstSysExType.
		//So we will send short channel messages always as kVstMidiType. No running status support in this case.  
		//bufpos[0] > 0x7F check has to be added because of SysEx Continuation messages...
		if (bufpos[0] < 0xF0 && bufpos[0] > 0x7F && len > 1) {

			DWORD startPos = 0;
			for (DWORD i = 0; i < len; i++) {
				if (bufpos[i] > 0x7F || i == len - 1) {
					DWORD shortMsg = 0;
					memcpy(&shortMsg, &bufpos[startPos], Min<DWORD>(i - startPos + 1, 3));
					PushMIDI(uDeviceID, shortMsg);
					startPos = i;
				}
			}
			return;
		}

		ScopeLock<Win32Lock> scopeLock(&synthLock);

		if (stopping) return;
		if (!PreprocessSysEx(uDeviceID, bufpos, len))
			return;

		DWORD tmpTimestamp = useAsio ? Min<DWORD>(bassAsioOut->GetPos() + midiLatency, bufferSize - 1) : (DWORD)((waveOut->GetPos(channels) + midiLatency) % bufferSize);
		midiStream->PutSysEx(uDeviceID, bufpos, len, tmpTimestamp);
	}

	void MidiSynth::SetVolume(unsigned uDeviceID, float volume) {
		midiVol[uDeviceID] = volume;
	}

	void MidiSynth::Close(bool forceUnload) {
		ScopeLock<Win32Lock> lifecycleScope(&lifecycleLock);
		InterlockedExchange(&stopping, 1);
		if (useAsio && bassAsioOut) {
			bassAsioOut->Pause();
			bassAsioOut->Close();
		}
		else if (!useAsio && waveOut) {
			waveOut->Pause();
			waveOut->Close();
		}

		{
			ScopeLock<Win32Lock> scopeLock(&synthLock);
			if (midiStream) midiStream->Reset();
		}

		// The final client's reset is deferred until the audio worker has exited.
		if (vstDriver && keepLoaded && !forceUnload && !vstDriver->isIOCancelled()) {
			Reset(0);
			Reset(1);
		}
		// An interrupted exchange cannot be reused: discard that bridge after joining.
		if (vstDriver && vstDriver->isIOCancelled()) forceUnload = true;
		if (!keepLoaded || forceUnload)
		{
			ScopeLock<Win32Lock> scopeLock(&synthLock);

			// Cleanup memory
			delete vstDriver;
			vstDriver = NULL;
		}
	}

	void MidiSynth::CancelPendingRender() {
		// Do not take synthLock: the rendering thread may hold it while waiting.
		if (vstDriver) vstDriver->cancelPendingIO();
	}

	void MidiSynth::RequestReset() {
		if (stopping || InterlockedCompareExchange(&resetPending, 1, 0)) return;
		if (!timeSetEvent(1, 1, ResetSynthTimerProc, NULL, TIME_ONESHOT | TIME_CALLBACK_FUNCTION))
			InterlockedExchange(&resetPending, 0);
	}

	void CALLBACK MidiSynth::ResetSynthTimerProc(UINT uTimerID, UINT uMsg, DWORD_PTR dwUser, DWORD_PTR dw1, DWORD_PTR dw2) {
		ScopeLock<Win32Lock> lifecycleScope(&lifecycleLock);
		// A queued plugin-switch timer must not resurrect an explicitly closed synth.
		if (!midiSynth.stopping) {
			midiSynth.Close(true);
			midiSynth.Init(DWORD(-1));
		}
		InterlockedExchange(&midiSynth.resetPending, 0);
	}
#pragma endregion MidiSynth class (singleton)
}
