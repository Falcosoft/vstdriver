/* Copyright (C) 2011, 2012 Sergey V. Mikayev
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation, either version 2.1 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "stdafx.h"

#undef GetMessage

#define BASSDEF(f) (WINAPI *f)	// define the BASS functions as pointers
#define BASSWASAPIDEF(f) (WINAPI *f)
#define LOADBASSFUNCTION(f) *((void**)&f)=GetProcAddress(bass,#f)
#define LOADBASSWASAPIFUNCTION(f) *((void**)&f)=GetProcAddress(basswasapi,#f)
#include <bass.h>
#include <basswasapi.h>

#include "VSTDriver.h"

extern "C" { extern HINSTANCE hinst_vst_driver; }

namespace VSTMIDIDRV {

static MidiSynth &midiSynth = MidiSynth::getInstance();

static class MidiStream {
private:
	static const unsigned int maxPos = 1024;
	unsigned int startpos;
	unsigned int endpos;
    struct message
    {
        void * sysex;
        DWORD msg;
        DWORD port_type;
    };
	message stream[maxPos];

public:
	MidiStream() {
		Reset();
	}

	void Reset() {
		startpos = 0;
		endpos = 0;
	}

	DWORD PutMessage(DWORD port, DWORD msg) {
		unsigned int newEndpos = endpos;

		newEndpos++;
		if (newEndpos == maxPos) // check for buffer rolloff
			newEndpos = 0;
		if (startpos == newEndpos) // check for buffer full
			return -1;
        stream[endpos].sysex = 0;
		stream[endpos].msg = msg;	// ok to put data and update endpos
        stream[endpos].port_type = port;
		endpos = newEndpos;
		return 0;
	}
    
    DWORD PutSysex(DWORD port, unsigned char * sysex, DWORD sysex_len)
    {
        unsigned int newEndpos = endpos;
        void * sysexCopy;
        
        newEndpos++;
        if (newEndpos == maxPos)
            newEndpos = 0;
        if (startpos == newEndpos)
            return -1;
        
        sysexCopy = malloc(sysex_len);
        if (!sysexCopy)
            return -1;
    
        memcpy(sysexCopy, sysex, sysex_len);
        
        stream[endpos].sysex = sysexCopy;
        stream[endpos].msg = sysex_len;
        stream[endpos].port_type = port | 0x80000000;
        endpos = newEndpos;
        return 0;
    }

	void GetMessage(DWORD & port, DWORD & message, unsigned char * & sysex, DWORD & sysex_len ) {
		if (startpos == endpos) // check for buffer empty
		{
			port = 0;
			message = 0;
			sysex = 0;
			sysex_len = 0;
			return;
		}
        port = stream[startpos].port_type & 0x7fffffff;
        if ( stream[startpos].port_type & 0x80000000 )
        {
            message = 0;
            sysex = (unsigned char *) stream[startpos].sysex;
            sysex_len = stream[startpos].msg;
        }
        else
        {
            message = stream[startpos].msg;
            sysex = 0;
            sysex_len = 0;
        }
		startpos++;
		if (startpos == maxPos) // check for buffer rolloff
			startpos = 0;
	}

	DWORD PeekMessageCount() {
		if (endpos < startpos) return endpos + maxPos - startpos;
		else return endpos - startpos;
	}
} midiStream;

static class SynthMutexWin32 {
private:
	CRITICAL_SECTION cCritSec;

public:
	int Init() {
		InitializeCriticalSection(&cCritSec);
		return 0;
	}

	void Close() {
		DeleteCriticalSection(&cCritSec);
	}

	void Enter() {
		EnterCriticalSection(&cCritSec);
	}

	void Leave() {
		LeaveCriticalSection(&cCritSec);
	}
} synthMutex;

static class WaveOutWin32 {
private:
	HINSTANCE   bass;			// bass handle
	HINSTANCE   basswasapi;        // basswasapi handle

	HSTREAM     hStOutput;

	bool        soundOutFloat;
	DWORD       wasapiBits;

public:
	WaveOutWin32() : bass(0), basswasapi(0), hStOutput(0) { }

	int Init(unsigned int bufferSize, unsigned int chunkSize, unsigned int sampleRate) {
		TCHAR installpath[MAX_PATH] = {0};
		TCHAR basspath[MAX_PATH];
		TCHAR basswasapipath[MAX_PATH];

		soundOutFloat = false;

		GetModuleFileName(hinst_vst_driver, installpath, MAX_PATH);
        lstrcpy(basswasapipath, installpath);
		PathRemoveFileSpec(installpath);
        TCHAR *fnpart = basswasapipath + lstrlen(installpath);
        TCHAR *fnend = fnpart + lstrlen(fnpart);
        while (fnend > fnpart && *fnend != _T('.'))
          fnend--;
        if (fnend != fnpart)
          *fnend = _T('\0');

		lstrcpy(basspath, installpath);
        lstrcat(basspath, fnpart);
		lstrcat(basspath, _T("\\bass.dll"));
		if (!(bass=LoadLibrary(basspath))) {
          lstrcpy(basspath, installpath);
          lstrcat(basspath, _T("\\bass.dll"));
          if (!(bass=LoadLibrary(basspath))) {
            OutputDebugString(_T("Failed to load BASS.dll.\n"));
            return -1;
            }
        else
          lstrcat(installpath, fnpart);
		}

		lstrcpy(basswasapipath, installpath);
		lstrcat(basswasapipath, _T("\\basswasapi.dll"));
		basswasapi=LoadLibrary(basswasapipath);

		LOADBASSFUNCTION(BASS_SetConfig);
		LOADBASSFUNCTION(BASS_Init);
		LOADBASSFUNCTION(BASS_Free);
		LOADBASSFUNCTION(BASS_StreamCreate);
		LOADBASSFUNCTION(BASS_StreamFree);
		LOADBASSFUNCTION(BASS_ChannelPlay);
		LOADBASSFUNCTION(BASS_ChannelStop);
		LOADBASSFUNCTION(BASS_ChannelPause);

		if (basswasapi) {
			LOADBASSWASAPIFUNCTION(BASS_WASAPI_Init);
			LOADBASSWASAPIFUNCTION(BASS_WASAPI_Free);
			LOADBASSWASAPIFUNCTION(BASS_WASAPI_Start);
			LOADBASSWASAPIFUNCTION(BASS_WASAPI_Stop);
			LOADBASSWASAPIFUNCTION(BASS_WASAPI_GetInfo);
		}

		BASS_SetConfig(BASS_CONFIG_UPDATEPERIOD, 5);

		if (BASS_Init(basswasapi ? 0 : -1, sampleRate, bufferSize, NULL, NULL)) {
			if (basswasapi) {
				BASS_WASAPI_INFO winfo;
				if (!BASS_WASAPI_Init(-1, 0, 2, BASS_WASAPI_EVENT, (float)bufferSize * 0.001f, (float)chunkSize * 0.001f, WasapiProc, this))
					return -2;
				if (!BASS_WASAPI_GetInfo(&winfo)) {
					BASS_WASAPI_Free();
					return -3;
				}
				sampleRate = winfo.freq;
				soundOutFloat = false;
				switch (winfo.format) {
				case BASS_WASAPI_FORMAT_8BIT:
					wasapiBits = 8;
					break;

				case BASS_WASAPI_FORMAT_16BIT:
					wasapiBits = 16;
					break;

				case BASS_WASAPI_FORMAT_24BIT:
					wasapiBits = 24;
					break;

				case BASS_WASAPI_FORMAT_32BIT:
					wasapiBits = 32;
					break;

				case BASS_WASAPI_FORMAT_FLOAT:
					soundOutFloat = TRUE;
					break;
				}
			}
			else {
				hStOutput = BASS_StreamCreate(sampleRate, 2, ( soundOutFloat ? BASS_SAMPLE_FLOAT : 0 ), StreamProc, this);
				if (!hStOutput) return -2;
			}
		}

		return sampleRate;
	}

	int Close() {
		if (hStOutput)
			BASS_ChannelStop(hStOutput);
		else if (basswasapi)
			BASS_WASAPI_Stop(TRUE);

		if (hStOutput) {
			BASS_StreamFree( hStOutput );
			hStOutput = 0;
		}

		if (basswasapi) {
			BASS_WASAPI_Free();
			FreeLibrary(basswasapi);
			basswasapi = 0;
		}
		if ( bass ) {
			BASS_Free();
			FreeLibrary(bass);
			bass = 0;
		}

		return 0;
	}

	int Start() {
		if (hStOutput)
			BASS_ChannelPlay(hStOutput, FALSE);
		else if (basswasapi)
			BASS_WASAPI_Start();
		return 0;
	}

	int Pause() {
		if (hStOutput)
			BASS_ChannelPause(hStOutput);
		else if (basswasapi)
			BASS_WASAPI_Stop(FALSE);
		return 0;
	}

	int Resume() {
		Start();
		return 0;
	}

	static DWORD CALLBACK StreamProc(HSTREAM handle, void *buffer, DWORD length, void *user) {
		WaveOutWin32 * _this = (WaveOutWin32 *)user;
		if (_this->soundOutFloat) {
			midiSynth.RenderFloat((float *)buffer, length / 8);
		} else {
			midiSynth.Render((short *)buffer, length / 4);
		}
		return length;
	}

	static DWORD CALLBACK WasapiProc(void *buffer, DWORD length, void *user)
	{
		WaveOutWin32 * _this = (WaveOutWin32 *)user;
		if (_this->soundOutFloat || _this->wasapiBits == 16)
			return StreamProc(NULL, buffer, length, user);
		else
		{
			int bytes_per_sample = _this->wasapiBits / 8;
			int bytes_done = 0;
			while (length)
			{
				unsigned short sample_buffer[1024];
				int length_todo = (length / bytes_per_sample);
				if (length_todo > 512) length_todo = 512;
				int bytes_done_this = StreamProc(NULL, sample_buffer, length_todo * 4, 0);
				if (bytes_done_this <= 0) return bytes_done;
				if (bytes_per_sample == 4)
				{
					unsigned int * out = (unsigned int *) buffer;
					for (int i = 0; i < bytes_done_this; i += 2)
					{
						*out++ = sample_buffer[i / 2] << 16;
					}
					buffer = out;
				}
				else if (bytes_per_sample == 3)
				{
					unsigned char * out = (unsigned char *) buffer;
					for (int i = 0; i < bytes_done_this; i += 2)
					{
						int sample = sample_buffer[i / 2];
						*out++ = 0;
						*out++ = sample & 0xFF;
						*out++ = (sample >> 8) & 0xFF;
					}
					buffer = out;
				}
				else if (bytes_per_sample == 1)
				{
					unsigned char * out = (unsigned char *) buffer;
					for (int i = 0; i < bytes_done_this; i += 2)
					{
						*out++ = (sample_buffer[i / 2] >> 8) & 0xFF;
					}
					buffer = out;
				}
				bytes_done += (bytes_done_this / 2) * bytes_per_sample;
				length -= (bytes_done_this / 2) * bytes_per_sample;
			}
			return bytes_done;
		}
	}
} waveOut;

MidiSynth::MidiSynth() {}

MidiSynth &MidiSynth::getInstance() {
	static MidiSynth *instance = new MidiSynth;
	return *instance;
}

// Renders totalFrames frames starting from bufpos
// The number of frames rendered is added to the global counter framesRendered
void MidiSynth::Render(short *bufpos, DWORD totalFrames) {
	DWORD count;
	// Incoming MIDI messages timestamped with the current audio playback position + midiLatency
	while ((count = midiStream.PeekMessageCount())) {
		DWORD msg;
		DWORD sysex_len;
		DWORD port;
		unsigned char * sysex;
		synthMutex.Enter();
		midiStream.GetMessage(port, msg, sysex, sysex_len);
		if (msg && !sysex)
		{
			vstDriver->ProcessMIDIMessage(port, msg);
		}
		else if (!msg && sysex && sysex_len)
		{
			vstDriver->ProcessSysEx(port, sysex, sysex_len);
			free(sysex);
		}
		synthMutex.Leave();
	}

	synthMutex.Enter();
	vstDriver->Render(bufpos, totalFrames);
	synthMutex.Leave();
}

void MidiSynth::RenderFloat(float *bufpos, DWORD totalFrames) {
	DWORD count;
	// Incoming MIDI messages timestamped with the current audio playback position + midiLatency
	while ((count = midiStream.PeekMessageCount())) {
		DWORD msg;
		DWORD sysex_len;
		DWORD port;
		unsigned char * sysex;
		synthMutex.Enter();
		midiStream.GetMessage(port, msg, sysex, sysex_len);
		if (msg && !sysex)
		{
			vstDriver->ProcessMIDIMessage(port, msg);
		}
		else if (!msg && sysex && sysex_len)
		{
			vstDriver->ProcessSysEx(port, sysex, sysex_len);
			free(sysex);
		}
		synthMutex.Leave();
	}

	synthMutex.Enter();
	vstDriver->RenderFloat(bufpos, totalFrames);
	synthMutex.Leave();
}

unsigned int MidiSynth::MillisToFrames(unsigned int millis) {
	return UINT(sampleRate * millis / 1000.f);
}

void MidiSynth::LoadSettings() {
	sampleRate = 44100;
	bufferSizeMS = 60;
	bufferSize = MillisToFrames(bufferSizeMS);
	chunkSizeMS = 10;
	chunkSize = MillisToFrames(chunkSizeMS);
	midiLatencyMS = 0;
	midiLatency = MillisToFrames(midiLatencyMS);
}

BOOL IsVistaOrNewer(){
	OSVERSIONINFOEX osvi;
	BOOL bOsVersionInfoEx;
	ZeroMemory(&osvi, sizeof(OSVERSIONINFOEX));
	osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEX);
	bOsVersionInfoEx = GetVersionEx((OSVERSIONINFO*)&osvi);
	if (bOsVersionInfoEx == FALSE) return FALSE;
	if (VER_PLATFORM_WIN32_NT == osvi.dwPlatformId &&
		osvi.dwMajorVersion > 5)
		return TRUE;
	return FALSE;
}

int MidiSynth::Init() {
	LoadSettings();

	// Init synth
	if (synthMutex.Init()) {
		return 1;
	}

	INT wResult = waveOut.Init(bufferSizeMS, chunkSizeMS, sampleRate);
	if (wResult < 0) return -wResult;
	sampleRate = wResult;

	vstDriver = new VSTDriver;
	if (!vstDriver->OpenVSTDriver(NULL, sampleRate)) {
		delete vstDriver;
		vstDriver = NULL;
		return 1;
	}

	wResult = waveOut.Start();
	return wResult;
}

int MidiSynth::Reset(unsigned uDeviceID) {
	UINT wResult = waveOut.Pause();
	if (wResult) return wResult;

	synthMutex.Enter();
	vstDriver->ResetDriver();
	midiStream.Reset();
	synthMutex.Leave();

	wResult = waveOut.Resume();
	return wResult;
}

void MidiSynth::PushMIDI(unsigned uDeviceID, DWORD msg) {
	synthMutex.Enter();
    midiStream.PutMessage(uDeviceID, msg);
	synthMutex.Leave();
}

void MidiSynth::PlaySysex(unsigned uDeviceID, unsigned char *bufpos, DWORD len) {
	synthMutex.Enter();
    midiStream.PutSysex(uDeviceID, bufpos, len);
	synthMutex.Leave();
}

void MidiSynth::Close() {
	waveOut.Pause();
	waveOut.Close();

	synthMutex.Enter();

	// Cleanup memory
	delete vstDriver;
	vstDriver = NULL;
    
	synthMutex.Leave();
	synthMutex.Close();
}

}
