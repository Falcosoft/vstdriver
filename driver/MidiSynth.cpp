/* Copyright (C) 2011, 2012 Sergey V. Mikayev
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

#undef GetMessage

// Define BASSASIO functions as pointers
#define BASSASIODEF(f) (WINAPI *f)
#define LOADBASSASIOFUNCTION(f) *((void**)&f)=GetProcAddress(bassAsio,#f)

#include "../external_packages/bassasio.h"

#include "VSTDriver.h"
#include <string>
#include <math.h>

using std::string;
using std::wstring;


extern "C"{ extern HINSTANCE hinst_vst_driver; }


namespace VSTMIDIDRV{

	static MidiSynth &midiSynth = MidiSynth::getInstance();

	static class MidiStream{
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
		MidiStream(){
			Reset();
		}

		void Reset(){
			startpos = 0;
			endpos = 0;
		}

		DWORD PutMessage(DWORD port, DWORD msg, DWORD timestamp){
			unsigned int newEndpos = endpos;

			newEndpos++;
			if (newEndpos == maxPos) // check for buffer rolloff
				newEndpos = 0;
			if (startpos == newEndpos) // check for buffer full
				return -1;
			stream[endpos].sysex = 0;
			stream[endpos].msg = msg; // ok to put data and update endpos
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

		void GetMessage(DWORD & port, DWORD & message, unsigned char * & sysex, DWORD & sysex_len ){
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

		DWORD PeekMessageTime(){
			if (startpos == endpos) // check for buffer empty
				return (DWORD)-1;
			return stream[startpos].timestamp;
		}

		DWORD PeekMessageTimeAt(unsigned int pos){
			if (startpos == endpos) // check for buffer empty
				return -1;
			unsigned int peekPos = (startpos + pos) % maxPos;
			return stream[peekPos].timestamp;
		}

		DWORD PeekMessageCount()
		{
			if (endpos < startpos)
			{
				return endpos + maxPos - startpos;
			}
			else
			{
				return endpos - startpos;
			}
		}
	}midiStream;

	static class SynthMutexWin32{
	private:
		CRITICAL_SECTION cCritSec;

	public:
		int Init(){
			InitializeCriticalSection(&cCritSec);
			return 0;
		}

		void Close(){
			DeleteCriticalSection(&cCritSec);
		}

		void Enter(){
			EnterCriticalSection(&cCritSec);
		}

		void Leave(){
			LeaveCriticalSection(&cCritSec);
		}
	}synthMutex;


	static UINT GetWaveOutDeviceId(){

		HKEY hKey;
		DWORD dwType = REG_SZ;
		DWORD dwSize = 0;
		WAVEOUTCAPSW caps;
		wchar_t* regValue;

		long result = RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\VSTi Driver\\Output Driver", 0, KEY_READ | KEY_WOW64_32KEY, &hKey);
		if (result == NO_ERROR) {

			result = RegQueryValueEx(hKey, _T("WinMM WaveOut"), NULL, &dwType, NULL, &dwSize);
			if (result == NO_ERROR && dwType == REG_SZ) {

				regValue = (TCHAR*) calloc( dwSize + sizeof(TCHAR), 1 );
				RegQueryValueEx(hKey, _T("WinMM WaveOut"), NULL, &dwType, (LPBYTE) regValue, &dwSize);

				for (size_t deviceId = -1; waveOutGetDevCaps(deviceId, &caps, sizeof(caps)) == MMSYSERR_NOERROR; ++deviceId) {
					if (!wcscmp(regValue, caps.szPname)) return deviceId;

				}
			}
		}

		return WAVE_MAPPER;

	}

	static class WaveOutWin32{
	private:
		HWAVEOUT hWaveOut;
		WAVEHDR* WaveHdr;
		HANDLE hEvent;
		HANDLE hThread;
		DWORD chunks;
		//DWORD getPosWraps;
		bool usingFloat;
		WORD channels;

		volatile UINT64 prevPlayPos;
		volatile bool stopProcessing;

	public:
		int Init(void* buffer, unsigned int bufferSize, unsigned int chunkSize, bool useRingBuffer, unsigned int sampleRate, bool useFloat, WORD channelCount){
			DWORD callbackType = CALLBACK_NULL;
			DWORD_PTR callback = NULL;
			usingFloat = useFloat;
			hEvent = NULL;
			hThread = NULL;
			if (!useRingBuffer) {
				hEvent = CreateEvent(NULL, false, true, NULL);
				callback = (DWORD_PTR)hEvent;
				callbackType = CALLBACK_EVENT;
			}

			//freopen_s((FILE**)stdout, "CONOUT$", "w", stdout); //redirect to allocated console;

			struct __declspec(uuid("00000003-0000-0010-8000-00aa00389b71")) KSDATAFORMAT_SUBTYPE_IEEE_FLOAT_STRUCT;
#define KSDATAFORMAT_SUBTYPE_IEEE_FLOAT __uuidof(KSDATAFORMAT_SUBTYPE_IEEE_FLOAT_STRUCT)

			channels = channelCount;

			PCMWAVEFORMAT wFormat = { WAVE_FORMAT_PCM, channels, sampleRate, sampleRate * channels * 2, channels * 2, 16 };
			WAVEFORMATEXTENSIBLE wFormatFloat;
			memset( &wFormatFloat, 0, sizeof(wFormatFloat));

			wFormatFloat.Format.cbSize = sizeof(wFormatFloat) - sizeof(wFormatFloat.Format);
			wFormatFloat.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
			wFormatFloat.Format.nChannels = channels;
			wFormatFloat.Format.nSamplesPerSec = sampleRate;
			wFormatFloat.Format.wBitsPerSample = 32;
			wFormatFloat.Format.nBlockAlign = wFormatFloat.Format.nChannels * wFormatFloat.Format.wBitsPerSample / 8;
			wFormatFloat.Format.nAvgBytesPerSec = wFormatFloat.Format.nBlockAlign * wFormatFloat.Format.nSamplesPerSec;
			wFormatFloat.dwChannelMask = channels == 2 ? SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT : SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
			wFormatFloat.Samples.wValidBitsPerSample = wFormatFloat.Format.wBitsPerSample;
			wFormatFloat.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;


			// Open waveout device
			int wResult = waveOutOpen(&hWaveOut, GetWaveOutDeviceId(), useFloat ? &wFormatFloat.Format : (LPWAVEFORMATEX)&wFormat, callback, (DWORD_PTR)&midiSynth, callbackType);
			if (wResult != MMSYSERR_NOERROR) {
				MessageBox(NULL, L"Failed to open waveform output device", L"VST MIDI Driver", MB_OK | MB_ICONEXCLAMATION);
				return 2;
			}

			// Prepare headers
			chunks = useRingBuffer ? 1 : bufferSize / chunkSize;
			WaveHdr = new WAVEHDR[chunks];
			LPSTR chunkStart = (LPSTR)buffer;
			DWORD chunkBytes = (useFloat ? sizeof(float) * channels : sizeof(short) * channels) * chunkSize;
			DWORD bufferBytes = (useFloat ? sizeof(float) * channels : sizeof(short) * channels) * bufferSize;
			for (UINT i = 0; i < chunks; i++) {
				if (useRingBuffer) {
					WaveHdr[i].dwBufferLength = bufferBytes;
					WaveHdr[i].lpData = chunkStart;
					WaveHdr[i].dwFlags = WHDR_BEGINLOOP | WHDR_ENDLOOP;
					WaveHdr[i].dwLoops = -1L;
				}
				else {
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

		int Close(){
			stopProcessing = true;
			SetEvent(hEvent);
			if (hThread != NULL) {
				WaitForSingleObject(hThread, 2000);
				hThread = NULL;
			}
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

		int Start(){
			//getPosWraps = 0;
			prevPlayPos = 0;
			for (UINT i = 0; i < chunks; i++) {
				if (waveOutWrite(hWaveOut, &WaveHdr[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
					MessageBox(NULL, L"Failed to write block to device", L"VST MIDI Driver", MB_OK | MB_ICONEXCLAMATION);
					return 4;
				}
			}
			hThread = (HANDLE)_beginthread(RenderingThread, 16384, this);
			return 0;
		}

		int Pause(){
			if (waveOutPause(hWaveOut) != MMSYSERR_NOERROR) {
				MessageBox(NULL, L"Failed to Pause wave playback", L"VST MIDI Driver", MB_OK | MB_ICONEXCLAMATION);
				return 9;
			}
			return 0;
		}

		int Resume(){
			if (waveOutRestart(hWaveOut) != MMSYSERR_NOERROR) {
				MessageBox(NULL, L"Failed to Resume wave playback", L"VST MIDI Driver", MB_OK | MB_ICONEXCLAMATION);
				return 9;
			}
			return 0;
		}

		UINT64 GetPos(WORD channels){

			DWORD WRAP_BITS = 27;
			if(usingFloat) WRAP_BITS--;
			if(channels == 4) WRAP_BITS--;

			UINT64 WRAP_MASK = (1 << WRAP_BITS) - 1;
			int WRAP_THRESHOLD = 1 << (WRAP_BITS - 1);

			// Taking a snapshot to avoid possible thread interference
			UINT64 playPositionSnapshot = prevPlayPos;
			DWORD wrapCount = DWORD(playPositionSnapshot >> WRAP_BITS);
			DWORD wrappedPosition = DWORD(playPositionSnapshot & WRAP_MASK);

			MMTIME mmTime;
			mmTime.wType = TIME_SAMPLES;

			if (waveOutGetPosition(hWaveOut, &mmTime, sizeof(MMTIME)) != MMSYSERR_NOERROR) {
				MessageBox(NULL, L"Failed to get current playback position", L"VST MIDI Driver", MB_OK | MB_ICONEXCLAMATION);
				return 10;
			}
			if (mmTime.wType != TIME_SAMPLES) {
				MessageBox(NULL, L"Failed to get # of samples played", L"VST MIDI Driver", MB_OK | MB_ICONEXCLAMATION);
				return 10;
			}
			mmTime.u.sample &= WRAP_MASK;

			// Deal with waveOutGetPosition() wraparound. For 16-bit stereo output, it equals 2^27,
			// presumably caused by the internal 32-bit counter of bits played.
			// The output of that nasty waveOutGetPosition() isn't monotonically increasing
			// even during 2^27 samples playback, so we have to ensure the difference is big enough...
			int delta = mmTime.u.sample - wrappedPosition;
			if (delta < -WRAP_THRESHOLD) {
#ifdef _DEBUG
				std::cout << "VST MIDI Driver: GetPos() wrap: " << delta << "\n";
#endif
				++wrapCount;
			} else if (delta < 0) {
				// This ensures the return is monotonically increased
#ifdef _DEBUG
				std::cout << "VST MIDI Driver: GetPos() went back by " << delta << " samples\n";
#endif
				return playPositionSnapshot;
			}
			prevPlayPos = playPositionSnapshot = mmTime.u.sample + (wrapCount << WRAP_BITS);

#ifdef _DEBUG
			std::cout << "VST MIDI Driver: GetPos()" << playPositionSnapshot << "\n";
#endif

			return playPositionSnapshot;
		}

		static void RenderingThread(void* pthis){
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
			WaveOutWin32* _this = (WaveOutWin32*)pthis;
			if (waveOut.chunks == 1) {
				// Rendering using single looped ring buffer
				while (!waveOut.stopProcessing) {
					midiSynth.RenderAvailableSpace();
				}
			}
			else {
				while (!waveOut.stopProcessing) {
					bool allBuffersRendered = true;
					for (UINT i = 0; i < waveOut.chunks; i++) {
						if (waveOut.WaveHdr[i].dwFlags & WHDR_DONE) {
							allBuffersRendered = false;
							if (_this->usingFloat)
								midiSynth.RenderFloat((float*)waveOut.WaveHdr[i].lpData, waveOut.WaveHdr[i].dwBufferLength / (sizeof(float) * _this->channels));
							else
								midiSynth.Render((short*)waveOut.WaveHdr[i].lpData, waveOut.WaveHdr[i].dwBufferLength / (sizeof(short) * _this->channels));
							if (waveOutWrite(waveOut.hWaveOut, &waveOut.WaveHdr[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
								MessageBox(NULL, L"Failed to write block to device", L"VST MIDI Driver", MB_OK | MB_ICONEXCLAMATION);
							}
						}
					}
					if (allBuffersRendered) {
						// Ensure the playback position is monitored frequently enough in order not to miss a wraparound
						waveOut.GetPos(_this->channels);
						WaitForSingleObject(waveOut.hEvent, INFINITE);
					}
				}
			}
		}
	}waveOut;

	static class BassAsioOut
	{
	private:

		HINSTANCE bassAsio; // bassasio handle

		TCHAR* outputDriver;

		bool soundOutFloat;
		DWORD buflen;
		WORD channels;
		unsigned int samplerate;

		volatile DWORD startTime;
		volatile LARGE_INTEGER startTimeQp;
		double	queryPerformanceUnit;

		TCHAR installPath[MAX_PATH];
		TCHAR bassAsioPath[MAX_PATH];



		void InitializePaths()
		{
			GetModuleFileName(hinst_vst_driver, installPath, MAX_PATH);
			PathRemoveFileSpec(installPath);
			lstrcat(installPath, _T("\\vstmididrv\\"));

			lstrcpy(bassAsioPath, installPath);
			lstrcat(bassAsioPath, _T("bassasio.dll"));

		}

		bool LoadBass()
		{
			// Load Bass Asio
			bassAsio = LoadLibrary(bassAsioPath);
			LOADBASSASIOFUNCTION(BASS_ASIO_GetDeviceInfo);


			/// Check if there is at least one ASIO capable device
			BASS_ASIO_DEVICEINFO info;
			if (BASS_ASIO_GetDeviceInfo(0, &info))
			{
				LOADBASSASIOFUNCTION(BASS_ASIO_Init);
				LOADBASSASIOFUNCTION(BASS_ASIO_Free);
				LOADBASSASIOFUNCTION(BASS_ASIO_Stop);
				LOADBASSASIOFUNCTION(BASS_ASIO_Start);
				LOADBASSASIOFUNCTION(BASS_ASIO_GetInfo);
				LOADBASSASIOFUNCTION(BASS_ASIO_GetRate);
				LOADBASSASIOFUNCTION(BASS_ASIO_SetRate);
				LOADBASSASIOFUNCTION(BASS_ASIO_SetNotify);
				//LOADBASSASIOFUNCTION(BASS_ASIO_GetDeviceInfo);
				LOADBASSASIOFUNCTION(BASS_ASIO_ErrorGetCode);
				LOADBASSASIOFUNCTION(BASS_ASIO_ChannelSetRate);
				LOADBASSASIOFUNCTION(BASS_ASIO_ChannelGetInfo);
				LOADBASSASIOFUNCTION(BASS_ASIO_ChannelJoin);
				LOADBASSASIOFUNCTION(BASS_ASIO_ChannelPause);
				LOADBASSASIOFUNCTION(BASS_ASIO_IsStarted);
				LOADBASSASIOFUNCTION(BASS_ASIO_ChannelReset);
				LOADBASSASIOFUNCTION(BASS_ASIO_ChannelEnable);
				LOADBASSASIOFUNCTION(BASS_ASIO_ChannelIsActive);
				LOADBASSASIOFUNCTION(BASS_ASIO_ChannelSetFormat);
				LOADBASSASIOFUNCTION(BASS_ASIO_GetLatency);
				LOADBASSASIOFUNCTION(BASS_ASIO_GetInfo);


			}
			else
			{
				FreeLibrary(bassAsio);
				bassAsio = NULL;
			}


			if (bassAsio)
			{
				return true;
			}

			return false;
		}

		wstring GetAsioDriverName()
		{
			HKEY hKey;
			DWORD dwType = REG_SZ;
			DWORD dwSize = 0;
			wchar_t* regValue;
			wstring wresult;

			long result = RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\VSTi Driver\\Output Driver", 0, KEY_READ | KEY_WOW64_32KEY, &hKey);
			if (result == NO_ERROR)
			{
				result = RegQueryValueEx(hKey, _T("Bass ASIO"), NULL, &dwType, NULL, &dwSize);
				if (result == NO_ERROR && dwType == REG_SZ)
				{
					regValue = (TCHAR*) calloc( dwSize + sizeof(TCHAR), 1 );
					RegQueryValueEx(hKey, _T("Bass ASIO"), NULL, &dwType, (LPBYTE) regValue, &dwSize);
					wresult = regValue;
				}
			}

			return wresult;
		}
		
		void GetSelectedAsioDriver(int& selectedDeviceId, int& selectedChannelId)
		{
			selectedDeviceId = -1;
			selectedChannelId = 0;
			wstring selectedOutputDriver = GetAsioDriverName();

			size_t period = selectedOutputDriver.find(L'.');
			wstring sdeviceId = selectedOutputDriver.substr(0, period);
			selectedDeviceId = _wtoi(sdeviceId.c_str());

			size_t space = selectedOutputDriver.find(L' ');
			wstring schannelid = selectedOutputDriver.substr(period + 1, space - period - 1);
			selectedChannelId = _wtoi(schannelid.c_str());
		}

	public:
		BassAsioOut()
		{
			soundOutFloat = true;
			buflen = 0;
			channels = 2;
			bassAsio = NULL;
		}

		int Init(unsigned int bufferSizeMS, unsigned int sampleRate, bool useFloat, WORD channelCount)
		{
			if (bassAsioPath[0] == NULL)
			{
				InitializePaths();
			}

			if (!LoadBass())
			{
				return -1;
			}

			if (bassAsio)
			{
				int deviceId;
				int channelId;

				GetSelectedAsioDriver(deviceId, channelId);

				if (!BASS_ASIO_Init(deviceId, BASS_ASIO_THREAD | BASS_ASIO_JOINORDER))
				{
					DWORD res = BASS_ASIO_ErrorGetCode();
					Close();
					return (int)res;
				}

				BASS_ASIO_SetRate(sampleRate);

				sampleRate = (int)BASS_ASIO_GetRate();
				samplerate = sampleRate;

				buflen = bufferSizeMS == 0 ? 0 : bufferSizeMS * (sampleRate / 1000.0f);
				channels = channelCount;

				BASS_ASIO_INFO info;
				BASS_ASIO_GetInfo(&info);

				// Enable 1st output channel
				if (!BASS_ASIO_ChannelEnable(FALSE, channelId, AsioProc, this)) return -2;

				soundOutFloat = useFloat;
				if(!BASS_ASIO_ChannelSetFormat(FALSE, channelId, soundOutFloat ? BASS_ASIO_FORMAT_FLOAT : BASS_ASIO_FORMAT_16BIT)) return -2;
				//if(!BASS_ASIO_ChannelSetRate(FALSE, channelId, sampleRate)) return -2;

				// Join the next channel to it (stereo)
				if (!BASS_ASIO_ChannelJoin(FALSE, (channelId + 1) % info.outputs, channelId)) return -2;

				if (channels == 4) {
					// Join the next channel to it (quad left)
					if (!BASS_ASIO_ChannelJoin(FALSE, (channelId + 2) % info.outputs, channelId)) return -2;

					// Join the next channel to it (quad right)
					if (!BASS_ASIO_ChannelJoin(FALSE, (channelId + 3) % info.outputs, channelId)) return -2;
				}

			}

			return 0;
		}

		int Close()
		{
			if (bassAsio)
			{
				// Stop ASIO device in case it was playing
				if (BASS_ASIO_IsStarted())
				{
					BASS_ASIO_Stop();
					Sleep(50);

				}
				BASS_ASIO_Free();
				Sleep(50);

				FreeLibrary(bassAsio);
				bassAsio = NULL;
			}


			return 0;
		}

		int Start()
		{
			if (bassAsio)
			{
				queryPerformanceUnit = 0.0;
				LARGE_INTEGER tmpFreq;
				if(QueryPerformanceFrequency(&tmpFreq)) 
				{
					queryPerformanceUnit = 1.0 / (tmpFreq.QuadPart * 0.001);
					QueryPerformanceCounter(const_cast<LARGE_INTEGER *>(&startTimeQp));
				}
				else 
				{
					startTime = timeGetTime();
				}

				if(!BASS_ASIO_Start(buflen, 0)) return -2;
			}

			return 0;
		}

		int Pause()
		{
			if (bassAsio)
			{
				//BASS_ASIO_ChannelPause(FALSE, -1);
			}

			return 0;
		}

		int Resume()
		{
			if (bassAsio)
			{
				if(queryPerformanceUnit != 0.0) QueryPerformanceCounter(const_cast<LARGE_INTEGER *>(&startTimeQp));
				else startTime = timeGetTime();
				//BASS_ASIO_ChannelReset(FALSE, -1, BASS_ASIO_RESET_PAUSE);
			}

			return 0;
		}

		DWORD GetPos()
		{
			DWORD res;
			if(queryPerformanceUnit != 0.0)
			{
				LARGE_INTEGER tmpCounter;
				QueryPerformanceCounter(&tmpCounter);
				res = DWORD((tmpCounter.QuadPart - startTimeQp.QuadPart) * queryPerformanceUnit * (samplerate * 0.001));
			}
			else res = DWORD((timeGetTime() - startTime) * (samplerate * 0.001));
			//std::cout << "VST MIDI Driver: GetPos(): " << res << "\n";
			return res;
		}

		static DWORD CALLBACK AsioProc(BOOL input, DWORD channel, void* buffer, DWORD length, void* user)
		{
			BassAsioOut* _this = (BassAsioOut*)user;
			if (_this->soundOutFloat)
			{
				midiSynth.RenderFloat((float*)buffer, length / (sizeof(float) * _this->channels));
				if(_this->queryPerformanceUnit != 0.0) QueryPerformanceCounter(const_cast<LARGE_INTEGER *>(&_this->startTimeQp));
				else _this->startTime = timeGetTime();			
			}
			else
			{				
				midiSynth.Render((short*)buffer, length / (sizeof(short) * _this->channels));
				if(_this->queryPerformanceUnit != 0.0) QueryPerformanceCounter(const_cast<LARGE_INTEGER *>(&_this->startTimeQp));
				else _this->startTime = timeGetTime();
			}
			return length;
		}

	}bassAsioOut;

	MidiSynth::MidiSynth(){}

	MidiSynth &MidiSynth::getInstance(){
		static MidiSynth *instance = new MidiSynth;
		return *instance;
	}

	// Renders all the available space in the single looped ring buffer
	void MidiSynth::RenderAvailableSpace(){
		if (!useAsio) {
			DWORD playPos = (DWORD)(waveOut.GetPos(channels) % bufferSize);
			DWORD framesToRender;

			if (playPos < framesRendered) {
				// Buffer wrap, render 'till the end of the buffer
				framesToRender = bufferSize - framesRendered;
			}
			else {
				framesToRender = playPos - framesRendered;
				if (framesToRender < chunkSize) {
					Sleep(1 + (chunkSize - framesToRender) * 1000 / sampleRate);
					return;
				}
			}
			if (usingFloat)
				midiSynth.RenderFloat(bufferf + channels * framesRendered, framesToRender);
			else
				midiSynth.Render(buffer + channels * framesRendered, framesToRender);
		}
	}


	// Renders totalFrames frames starting from bufpos
	// The number of frames rendered is added to the global counter framesRendered
	void MidiSynth::Render(short *bufpos, DWORD totalFrames){
		while (totalFrames > 0) {
			DWORD timeStamp;
			// Incoming MIDI messages timestamped with the current audio playback position + midiLatency
			while ((timeStamp = midiStream.PeekMessageTime()) == framesRendered) {
				DWORD msg;
				DWORD sysex_len;
				DWORD port;
				unsigned char* sysex;
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

			// Find out how many frames to render. The value of timeStamp == -1 indicates the MIDI buffer is empty
			DWORD framesToRender = timeStamp - framesRendered;
			if (framesToRender > totalFrames) {
				// MIDI message is too far - render the rest of frames
				framesToRender = totalFrames;
			}
			synthMutex.Enter();
			vstDriver->Render(bufpos, framesToRender, outputGain, channels);
			synthMutex.Leave();
			framesRendered += framesToRender;
			bufpos += framesToRender * channels;
			totalFrames -= framesToRender;
		}

		// Wrap framesRendered counter
		if (framesRendered >= bufferSize) {
			framesRendered -= bufferSize;
		}
	}

	void MidiSynth::RenderFloat(float *bufpos, DWORD totalFrames){
		while (totalFrames > 0) {
			DWORD timeStamp;
			// Incoming MIDI messages timestamped with the current audio playback position + midiLatency
			while ((timeStamp = midiStream.PeekMessageTime()) == framesRendered) {
				DWORD msg;
				DWORD sysex_len;
				DWORD port;
				unsigned char* sysex;
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

			// Find out how many frames to render. The value of timeStamp == -1 indicates the MIDI buffer is empty
			DWORD framesToRender = timeStamp - framesRendered;
			if (framesToRender > totalFrames) {
				// MIDI message is too far - render the rest of frames
				framesToRender = totalFrames;
			}
			synthMutex.Enter();
			vstDriver->RenderFloat(bufpos, framesToRender, outputGain, channels);
			synthMutex.Leave();
			framesRendered += framesToRender;
			bufpos += framesToRender * channels;
			totalFrames -= framesToRender;
		}

		// Wrap framesRendered counter
		if (framesRendered >= bufferSize) {
			framesRendered -= bufferSize;
		}
	}

	unsigned int MidiSynth::MillisToFrames(unsigned int millis){
		return UINT(sampleRate * millis / 1000.f);
	}

	DWORD GetSampleRate(){
		DWORD sampleRate = 48000;
		HKEY hKey;

		long result = RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\VSTi Driver", 0, KEY_READ | KEY_WOW64_32KEY, &hKey);
		if (result == NO_ERROR) {
			DWORD size = 4;
			RegQueryValueEx(hKey, L"SampleRate", NULL, NULL, (LPBYTE)&sampleRate, &size);
		}
		return sampleRate;
	}

	DWORD GetBufferSize(){
		DWORD bufferSize = 80;
		HKEY hKey;

		long result = RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\VSTi Driver", 0, KEY_READ | KEY_WOW64_32KEY, &hKey);
		if (result == NO_ERROR) {
			DWORD size = 4;
			RegQueryValueEx(hKey, L"BufferSize", NULL, NULL, (LPBYTE)&bufferSize, &size);
		}
		return bufferSize;
	}

	int GetGain(){
		int gain = 1;
		HKEY hKey;

		long result = RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\VSTi Driver", 0, KEY_READ | KEY_WOW64_32KEY, &hKey);
		if (result == NO_ERROR) {
			DWORD size = 4;
			RegQueryValueEx(hKey, L"Gain", NULL, NULL, (LPBYTE)&gain, &size);
		}
		return gain;
	}

	WORD GetChannelCount(){
		WORD channels = 2;
		DWORD enabled = 0;
		HKEY hKey;
		long result = RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\VSTi Driver", 0, KEY_READ | KEY_WOW64_32KEY, &hKey);
		if (result == NO_ERROR) {
			DWORD size = 4;
			RegQueryValueEx(hKey, L"Use4ChannelMode", NULL, NULL, (LPBYTE)&enabled, &size);
		}
		channels = enabled ? 4 : 2;
		return channels;
	}

	void MidiSynth::LoadSettings(){
		channels = GetChannelCount();
		outputGain = pow(10.0f, GetGain() * 0.05f);
		sampleRate = GetSampleRate();
		bufferSizeMS = GetBufferSize();
		bufferSize = MillisToFrames(bufferSizeMS);
		chunkSizeMS = bufferSizeMS / 4;
		chunkSize = MillisToFrames(chunkSizeMS);
		midiLatencyMS = 0;
		midiLatency = MillisToFrames(midiLatencyMS);
		useRingBuffer = false;
		if (!useRingBuffer && chunkSize) {
			// Number of chunks should be ceil(bufferSize / chunkSize)
			DWORD chunks = (bufferSize + chunkSize - 1) / chunkSize;
			// Refine bufferSize as chunkSize * number of chunks, no less then the specified value
			bufferSize = chunks * chunkSize;
		}
	}

	bool IsXPOrNewer(){
		OSVERSIONINFOEX osvi;
		BOOL bOsVersionInfoEx;
		ZeroMemory(&osvi, sizeof(OSVERSIONINFOEX));
		osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEX);
		bOsVersionInfoEx = GetVersionEx((OSVERSIONINFO*)&osvi);
		if (bOsVersionInfoEx == FALSE) return FALSE;
		if (VER_PLATFORM_WIN32_NT == osvi.dwPlatformId && ((osvi.dwMajorVersion > 5) || (osvi.dwMajorVersion == 5 && osvi.dwMinorVersion >= 1)))
			return true;
		return false;
	}

	bool IsShowVSTDialog(){
		HKEY hKey;
		long result = RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\VSTi Driver", 0, KEY_READ | KEY_WOW64_32KEY, &hKey);
		if (result == NO_ERROR) {
			DWORD showVstDialog;
			DWORD size = 4;
			result = RegQueryValueEx(hKey, L"ShowVstDialog", NULL, NULL, (LPBYTE)&showVstDialog, &size);
			if (result == NO_ERROR && showVstDialog) return true;
			return false;
		}
		return false;
	}

	bool UseAsio(){

		HKEY hKey;
		DWORD dwType = REG_SZ;
		DWORD dwSize = 0;
		wchar_t* regValue;

		long result = RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\VSTi Driver\\Output Driver", 0, KEY_READ | KEY_WOW64_32KEY, &hKey);
		if (result == NO_ERROR) {

			result = RegQueryValueEx(hKey, _T("Driver Mode"), NULL, &dwType, NULL, &dwSize);
			if (result == NO_ERROR && dwType == REG_SZ) {

				regValue = (TCHAR*) calloc( dwSize + sizeof(TCHAR), 1 );
				RegQueryValueEx(hKey, _T("Driver Mode"), NULL, &dwType, (LPBYTE) regValue, &dwSize);
				if (!wcscmp(regValue, L"Bass ASIO"))
				{
					free(regValue);
					return true;
				}
			}
		}

		return false;
	}

	void MidiSynth::InitDialog(unsigned uDeviceID){

		if(IsShowVSTDialog()) vstDriver->displayEditorModal(uDeviceID);
	}

	int MidiSynth::Init(unsigned uDeviceID){
		LoadSettings();

		usingFloat = IsXPOrNewer();
		useAsio = UseAsio();

		// Init synth
		if (synthMutex.Init()) {
			return 1;
		}

		UINT wResult = 0;
		if (useAsio)
		{
			wResult = bassAsioOut.Init(bufferSizeMS, sampleRate, usingFloat, channels);
			if (wResult) bassAsioOut.Close();
			else
			{
				sampleRate = (int)BASS_ASIO_GetRate();
				if (!bufferSize)
				{
					BASS_ASIO_INFO info;
					BASS_ASIO_GetInfo(&info);
					bufferSize = info.bufpref;
				}
				else
				{
					bufferSize = MillisToFrames(bufferSizeMS);
				}
			}
		}
		else
		{
			if (usingFloat)
				bufferf = new float[channels * bufferSize];
			else
				buffer = new short[channels * bufferSize]; // each frame consists of two samples for both the Left and Right channels

			wResult = waveOut.Init(usingFloat ? (void*)bufferf : (void*)buffer, bufferSize, chunkSize, useRingBuffer, sampleRate, usingFloat, channels);
		}

		if (wResult) return wResult;

		vstDriver = new VSTDriver;
		if (!vstDriver->OpenVSTDriver(NULL, sampleRate)) {
			delete vstDriver;
			vstDriver = NULL;
			return 1;
		}

		InitDialog(uDeviceID);

		framesRendered = 0;

		// Start playing stream
		if (useAsio) {
			wResult = bassAsioOut.Start();
			if (wResult) bassAsioOut.Close();
		}
		else {
			if (usingFloat)
				memset(bufferf, 0, bufferSize * sizeof(float) * channels);
			else
				memset(buffer, 0, bufferSize * sizeof(short) * channels);

			wResult = waveOut.Start();
		}

		return wResult;
	}

	int MidiSynth::Reset(unsigned uDeviceID){
		UINT wResult = useAsio ? bassAsioOut.Pause() : waveOut.Pause();
		if (wResult) return wResult;

		synthMutex.Enter();
		vstDriver->ResetDriver(uDeviceID);
		midiStream.Reset();
		synthMutex.Leave();

		wResult = useAsio ? bassAsioOut.Resume() : waveOut.Resume();
		return wResult;
	}

	void MidiSynth::PushMIDI(unsigned uDeviceID, DWORD msg){
		synthMutex.Enter();

		if (useAsio) midiStream.PutMessage(uDeviceID, msg, min(bassAsioOut.GetPos() + midiLatency, bufferSize - 1));
		else midiStream.PutMessage(uDeviceID, msg, (DWORD)((waveOut.GetPos(channels) + midiLatency) % bufferSize));

		synthMutex.Leave();
	}

	void MidiSynth::PlaySysex(unsigned uDeviceID, unsigned char *bufpos, DWORD len){
		synthMutex.Enter();

		if (useAsio) midiStream.PutSysex(uDeviceID, bufpos, len, min(bassAsioOut.GetPos() + midiLatency, bufferSize - 1));
		else midiStream.PutSysex(uDeviceID, bufpos, len, (DWORD)((waveOut.GetPos(channels) + midiLatency) % bufferSize));

		synthMutex.Leave();
	}

	void MidiSynth::Close(){
		if (useAsio) {
			bassAsioOut.Close();
		}
		else {
			waveOut.Pause();
			waveOut.Close();
		}

		synthMutex.Enter();

		// Cleanup memory
		delete vstDriver;
		vstDriver = NULL;

		synthMutex.Leave();
		synthMutex.Close();
	}

}
