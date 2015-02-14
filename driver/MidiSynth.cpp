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

#include "VSTDriver.h"

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
        DWORD timestamp;
        DWORD port_type;
    };
	message stream[maxPos];

public:
	MidiStream() {
		startpos = 0;
		endpos = 0;
	}

	DWORD PutMessage(DWORD port, DWORD msg, DWORD timestamp) {
		unsigned int newEndpos = endpos;

		newEndpos++;
		if (newEndpos == maxPos) // check for buffer rolloff
			newEndpos = 0;
		if (startpos == newEndpos) // check for buffer full
			return -1;
        stream[endpos].sysex = 0;
		stream[endpos].msg = msg;	// ok to put data and update endpos
		stream[endpos].timestamp = timestamp;
        stream[endpos].port_type = port;
		endpos = newEndpos;
		return 0;
	}
    
    DWORD PutSysex(DWORD port, unsigned char * sysex, DWORD sysex_len, DWORD timestamp)
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
        stream[endpos].timestamp = timestamp;
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

	DWORD PeekMessageTime() {
		if (startpos == endpos) // check for buffer empty
			return (DWORD)-1;
		return stream[startpos].timestamp;
	}

	DWORD PeekMessageTimeAt(unsigned int pos) {
		if (startpos == endpos) // check for buffer empty
			return -1;
		unsigned int peekPos = (startpos + pos) % maxPos;
		return stream[peekPos].timestamp;
	}
} midiStream;

static class SynthEventWin32 {
private:
	HANDLE hEvent;

public:
	int Init() {
		hEvent = CreateEvent(NULL, false, true, NULL);
		if (hEvent == NULL) {
			MessageBox(NULL, L"Can't create sync object", L"VST MIDI Driver", MB_OK | MB_ICONEXCLAMATION);
			return 1;
		}
		return 0;
	}

	void Close() {
		CloseHandle(hEvent);
	}

	void Wait() {
		WaitForSingleObject(hEvent, INFINITE);
	}

	void Release() {
		SetEvent(hEvent);
	}
} synthEvent;

static class WaveOutWin32 {
private:
	HWAVEOUT	hWaveOut;
	WAVEHDR		*WaveHdr;
	HANDLE		hEvent;
	DWORD		chunks;
	DWORD		prevPlayPos;
	DWORD		getPosWraps;
	bool		stopProcessing;
	bool        usingFloat;

public:
	int Init(void *buffer, unsigned int bufferSize, unsigned int chunkSize, bool useRingBuffer, unsigned int sampleRate, bool useFloat) {
		DWORD callbackType = CALLBACK_NULL;
		DWORD_PTR callback = NULL;
		usingFloat = useFloat;
		hEvent = NULL;
		if (!useRingBuffer) {
			hEvent = CreateEvent(NULL, false, true, NULL);
			callback = (DWORD_PTR)hEvent;
			callbackType = CALLBACK_EVENT;
		}

		PCMWAVEFORMAT wFormat = {WAVE_FORMAT_PCM, 2, sampleRate, sampleRate * 4, 4, 16};
		WAVEFORMATEX wFormatFloat = { WAVE_FORMAT_IEEE_FLOAT, 2, sampleRate, sampleRate * 8, 8, 32, 0 };

		// Open waveout device
		int wResult = waveOutOpen(&hWaveOut, WAVE_MAPPER, useFloat ? &wFormatFloat : (LPWAVEFORMATEX)&wFormat, callback, (DWORD_PTR)&midiSynth, callbackType);
		if (wResult != MMSYSERR_NOERROR) {
			MessageBox(NULL, L"Failed to open waveform output device", L"VST MIDI Driver", MB_OK | MB_ICONEXCLAMATION);
			return 2;
		}

		// Prepare headers
		chunks = useRingBuffer ? 1 : bufferSize / chunkSize;
		WaveHdr = new WAVEHDR[chunks];
		LPSTR chunkStart = (LPSTR)buffer;
		DWORD chunkBytes = (useFloat ? 8 : 4) * chunkSize;
		DWORD bufferBytes = (useFloat ? 8 : 4) * bufferSize;
		for (UINT i = 0; i < chunks; i++) {
			if (useRingBuffer) {
				WaveHdr[i].dwBufferLength = bufferBytes;
				WaveHdr[i].lpData = chunkStart;
				WaveHdr[i].dwFlags = WHDR_BEGINLOOP | WHDR_ENDLOOP;
				WaveHdr[i].dwLoops = -1L;
			} else {
				WaveHdr[i].dwBufferLength = chunkBytes;
				WaveHdr[i].lpData = chunkStart;
				WaveHdr[i].dwFlags = 0L;
				WaveHdr[i].dwLoops = 0L;
				chunkStart += chunkBytes;
			}
			wResult = waveOutPrepareHeader(hWaveOut, &WaveHdr[i], sizeof(WAVEHDR));
			if (wResult != MMSYSERR_NOERROR) {
				MessageBox(NULL, L"Failed to Prepare Header", L"VST MIDI Driver", MB_OK | MB_ICONEXCLAMATION);
				return 3;
			}
		}
		stopProcessing = false;
		return 0;
	}

	int Close() {
		stopProcessing = true;
		SetEvent(hEvent);
		int wResult = waveOutReset(hWaveOut);
		if (wResult != MMSYSERR_NOERROR) {
			MessageBox(NULL, L"Failed to Reset WaveOut", L"VST MIDI Driver", MB_OK | MB_ICONEXCLAMATION);
			return 8;
		}

		for (UINT i = 0; i < chunks; i++) {
			wResult = waveOutUnprepareHeader(hWaveOut, &WaveHdr[i], sizeof(WAVEHDR));
			if (wResult != MMSYSERR_NOERROR) {
				MessageBox(NULL, L"Failed to Unprepare Wave Header", L"VST MIDI Driver", MB_OK | MB_ICONEXCLAMATION);
				return 8;
			}
		}
		delete[] WaveHdr;
		WaveHdr = NULL;

		wResult = waveOutClose(hWaveOut);
		if (wResult != MMSYSERR_NOERROR) {
			MessageBox(NULL, L"Failed to Close WaveOut", L"VST MIDI Driver", MB_OK | MB_ICONEXCLAMATION);
			return 8;
		}
		if (hEvent != NULL) {
			CloseHandle(hEvent);
			hEvent = NULL;
		}
		return 0;
	}

	int Start() {
		getPosWraps = 0;
		prevPlayPos = 0;
		for (UINT i = 0; i < chunks; i++) {
			if (waveOutWrite(hWaveOut, &WaveHdr[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
				MessageBox(NULL, L"Failed to write block to device", L"VST MIDI Driver", MB_OK | MB_ICONEXCLAMATION);
				return 4;
			}
		}
		_beginthread(RenderingThread, 16384, this);
		return 0;
	}

	int Pause() {
		if (waveOutPause(hWaveOut) != MMSYSERR_NOERROR) {
			MessageBox(NULL, L"Failed to Pause wave playback", L"VST MIDI Driver", MB_OK | MB_ICONEXCLAMATION);
			return 9;
		}
		return 0;
	}

	int Resume() {
		if (waveOutRestart(hWaveOut) != MMSYSERR_NOERROR) {
			MessageBox(NULL, L"Failed to Resume wave playback", L"VST MIDI Driver", MB_OK | MB_ICONEXCLAMATION);
			return 9;
		}
		return 0;
	}

	UINT64 GetPos() {
		MMTIME mmTime;
		mmTime.wType = TIME_SAMPLES;

		if (waveOutGetPosition(hWaveOut, &mmTime, sizeof MMTIME) != MMSYSERR_NOERROR) {
			MessageBox(NULL, L"Failed to get current playback position", L"VST MIDI Driver", MB_OK | MB_ICONEXCLAMATION);
			return 10;
		}
		if (mmTime.wType != TIME_SAMPLES) {
			MessageBox(NULL, L"Failed to get # of samples played", L"VST MIDI Driver", MB_OK | MB_ICONEXCLAMATION);
			return 10;
		}

		// Deal with waveOutGetPosition() wraparound. For 16-bit stereo output, it equals 2^27,
		// presumably caused by the internal 32-bit counter of bits played.
		// The output of that nasty waveOutGetPosition() isn't monotonically increasing
		// even during 2^27 samples playback, so we have to ensure the difference is big enough...
		int delta = mmTime.u.sample - prevPlayPos;
		if (usingFloat) {
			if (delta < -(1 << 25)) {
				std::cout << "VST MIDI Driver: GetPos() wrap: " << delta << "\n";
				++getPosWraps;
			}
			prevPlayPos = mmTime.u.sample;
			return mmTime.u.sample + getPosWraps * (1 << 26);
		}
		else {
			if (delta < -(1 << 26)) {
				std::cout << "VST MIDI Driver: GetPos() wrap: " << delta << "\n";
				++getPosWraps;
			}
			prevPlayPos = mmTime.u.sample;
			return mmTime.u.sample + getPosWraps * (1 << 27);
		}
	}

	static void RenderingThread(void *pthis) {
		WaveOutWin32 * _this = (WaveOutWin32 *)pthis;
		if (waveOut.chunks == 1) {
			// Rendering using single looped ring buffer
			while (!waveOut.stopProcessing) {
				midiSynth.RenderAvailableSpace();
			}
		} else {
			while (!waveOut.stopProcessing) {
				bool allBuffersRendered = true;
				for (UINT i = 0; i < waveOut.chunks; i++) {
					if (waveOut.WaveHdr[i].dwFlags & WHDR_DONE) {
						allBuffersRendered = false;
						if (_this->usingFloat)
							midiSynth.RenderFloat((float *)waveOut.WaveHdr[i].lpData, waveOut.WaveHdr[i].dwBufferLength / 8);
						else
							midiSynth.Render((short *)waveOut.WaveHdr[i].lpData, waveOut.WaveHdr[i].dwBufferLength / 4);
						if (waveOutWrite(waveOut.hWaveOut, &waveOut.WaveHdr[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
							MessageBox(NULL, L"Failed to write block to device", L"VST MIDI Driver", MB_OK | MB_ICONEXCLAMATION);
						}
					}
				}
				if (allBuffersRendered) {
					WaitForSingleObject(waveOut.hEvent, INFINITE);
				}
			}
		}
	}
} waveOut;

MidiSynth::MidiSynth() {}

MidiSynth &MidiSynth::getInstance() {
	static MidiSynth *instance = new MidiSynth;
	return *instance;
}

// Renders all the available space in the single looped ring buffer
void MidiSynth::RenderAvailableSpace() {
	DWORD playPos = waveOut.GetPos() % bufferSize;
	DWORD framesToRender;

	if (playPos < framesRendered) {
		// Buffer wrap, render 'till the end of the buffer
		framesToRender = bufferSize - framesRendered;
	} else {
		framesToRender = playPos - framesRendered;
		if (framesToRender < chunkSize) {
			Sleep(1 + (chunkSize - framesToRender) * 1000 / sampleRate);
			return;
		}
	}
	if (usingFloat)
		midiSynth.RenderFloat(bufferf + 2 * framesRendered, framesToRender);
	else
		midiSynth.Render(buffer + 2 * framesRendered, framesToRender);
}

// Renders totalFrames frames starting from bufpos
// The number of frames rendered is added to the global counter framesRendered
void MidiSynth::Render(short *bufpos, DWORD totalFrames) {
	while (totalFrames > 0) {
		DWORD timeStamp;
		// Incoming MIDI messages timestamped with the current audio playback position + midiLatency
		while ((timeStamp = midiStream.PeekMessageTime()) == framesRendered) {
            DWORD msg;
            DWORD sysex_len;
            DWORD port;
            unsigned char * sysex;
            midiStream.GetMessage(port, msg, sysex, sysex_len);
            synthEvent.Wait();
            if (msg && !sysex)
            {
                vstDriver->ProcessMIDIMessage(port, msg);
            }
            else if (!msg && sysex && sysex_len)
            {
                vstDriver->ProcessSysEx(port, sysex, sysex_len);
                free(sysex);
            }
			synthEvent.Release();
		}

		// Find out how many frames to render. The value of timeStamp == -1 indicates the MIDI buffer is empty
		DWORD framesToRender = timeStamp - framesRendered;
		if (framesToRender > totalFrames) {
			// MIDI message is too far - render the rest of frames
			framesToRender = totalFrames;
		}
		synthEvent.Wait();
		vstDriver->Render(bufpos, framesToRender);
		synthEvent.Release();
		framesRendered += framesToRender;
		bufpos += framesToRender * 2;
		totalFrames -= framesToRender;
	}

	// Wrap framesRendered counter
	if (framesRendered >= bufferSize) {
		framesRendered -= bufferSize;
	}
}

void MidiSynth::RenderFloat(float *bufpos, DWORD totalFrames) {
	while (totalFrames > 0) {
		DWORD timeStamp;
		// Incoming MIDI messages timestamped with the current audio playback position + midiLatency
		while ((timeStamp = midiStream.PeekMessageTime()) == framesRendered) {
			DWORD msg;
			DWORD sysex_len;
			DWORD port;
			unsigned char * sysex;
			midiStream.GetMessage(port, msg, sysex, sysex_len);
			synthEvent.Wait();
			if (msg && !sysex)
			{
				vstDriver->ProcessMIDIMessage(port, msg);
			}
			else if (!msg && sysex && sysex_len)
			{
				vstDriver->ProcessSysEx(port, sysex, sysex_len);
				free(sysex);
			}
			synthEvent.Release();
		}

		// Find out how many frames to render. The value of timeStamp == -1 indicates the MIDI buffer is empty
		DWORD framesToRender = timeStamp - framesRendered;
		if (framesToRender > totalFrames) {
			// MIDI message is too far - render the rest of frames
			framesToRender = totalFrames;
		}
		synthEvent.Wait();
		vstDriver->RenderFloat(bufpos, framesToRender);
		synthEvent.Release();
		framesRendered += framesToRender;
		bufpos += framesToRender * 2;
		totalFrames -= framesToRender;
	}

	// Wrap framesRendered counter
	if (framesRendered >= bufferSize) {
		framesRendered -= bufferSize;
	}
}

unsigned int MidiSynth::MillisToFrames(unsigned int millis) {
	return UINT(sampleRate * millis / 1000.f);
}

void MidiSynth::LoadSettings() {
	sampleRate = 44100;
	bufferSize = MillisToFrames(70);
	chunkSize = MillisToFrames(10);
	midiLatency = MillisToFrames(0);
	useRingBuffer = false;
	if (!useRingBuffer) {
		// Number of chunks should be ceil(bufferSize / chunkSize)
		DWORD chunks = (bufferSize + chunkSize - 1) / chunkSize;
		// Refine bufferSize as chunkSize * number of chunks, no less then the specified value
		bufferSize = chunks * chunkSize;
	}
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

	usingFloat = IsVistaOrNewer();

	if (usingFloat)
		bufferf = new float[2 * bufferSize];
	else
		buffer = new short[2 * bufferSize]; // each frame consists of two samples for both the Left and Right channels

	// Init synth
	if (synthEvent.Init()) {
		return 1;
	}

	vstDriver = new VSTDriver;
	if (!vstDriver->OpenVSTDriver()) {
		delete vstDriver;
		vstDriver = NULL;
		return 1;
	}

	UINT wResult = waveOut.Init(usingFloat ? (void*)bufferf : (void*)buffer, bufferSize, chunkSize, useRingBuffer, sampleRate, usingFloat);
	if (wResult) return wResult;

	// Start playing stream
	if (usingFloat)
		memset(bufferf, 0, bufferSize * sizeof(float) * 2);
	else
		memset(buffer, 0, bufferSize * sizeof(short) * 2);
	framesRendered = 0;

	wResult = waveOut.Start();
	return wResult;
}

int MidiSynth::Reset(unsigned uDeviceID) {
	UINT wResult = waveOut.Pause();
	if (wResult) return wResult;

	synthEvent.Wait();
	vstDriver->ResetDriver();
	synthEvent.Release();

	wResult = waveOut.Resume();
	return wResult;
}

void MidiSynth::PushMIDI(unsigned uDeviceID, DWORD msg) {
    midiStream.PutMessage(uDeviceID, msg, (waveOut.GetPos() + midiLatency) % bufferSize);
}

void MidiSynth::PlaySysex(unsigned uDeviceID, unsigned char *bufpos, DWORD len) {
    midiStream.PutSysex(uDeviceID, bufpos, len, (waveOut.GetPos() + midiLatency) % bufferSize);
}

void MidiSynth::Close() {
	waveOut.Pause();
	waveOut.Close();
	synthEvent.Wait();

	// Cleanup memory
	delete vstDriver;
	vstDriver = NULL;
    
	synthEvent.Close();
}

}
