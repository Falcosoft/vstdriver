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

// for dynamic loading of Avrt.dll functions
#define WIN32DEF(f) (WINAPI *f)
typedef enum _MYAVRT_PRIORITY
{
	AVRT_PRIORITY_VERYLOW = -2,
	AVRT_PRIORITY_LOW,
	AVRT_PRIORITY_NORMAL,
	AVRT_PRIORITY_HIGH,
	AVRT_PRIORITY_CRITICAL
} MYAVRT_PRIORITY, * PMYAVRT_PRIORITY;


#include "../external_packages/bassasio.h"

#include "VSTDriver.h"
#include <string>
#include <math.h>

extern "C"{ extern HINSTANCE hinst_vst_driver; }

namespace VSTMIDIDRV{

	static MidiSynth &midiSynth = MidiSynth::getInstance();	
	
	static Win32Lock synthLock;
	
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

		DWORD PutMessage(DWORD port, DWORD msg, DWORD timestamp) {		
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

		DWORD PutSysEx(DWORD port, unsigned char * sysex, DWORD sysex_len, DWORD timestamp)	{			
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
		
	
	inline static DWORD DwordMin(DWORD a, DWORD b) //min macro calls bassAsioOut.GetPos() 2 times that can cause problems...
	{
		if (a < b) return a;
		return b;
	}

	inline static DWORD DwordMax(DWORD a, DWORD b)
	{
		if (a > b) return a;
		return b;
	}

	static DWORD GetDwordData (LPCTSTR valueName, DWORD defaultValue) 
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
		bool errorShown[10];

		volatile UINT64 prevPlayPos;
		volatile bool stopProcessing;

	public:
		int Init(void* buffer, unsigned int bufferSize, unsigned int chunkSize, unsigned int sampleRate, bool useFloat, WORD channelCount){
			DWORD callbackType = CALLBACK_NULL;
			DWORD_PTR callback = NULL;
			usingFloat = useFloat;
			hEvent = NULL;
			hThread = NULL;
			hEvent = CreateEvent(NULL, false, true, NULL);
			callback = (DWORD_PTR)hEvent;
			callbackType = CALLBACK_EVENT;

			for (int i = 0; i < 10; i++) errorShown[i] = false;

			//freopen_s((FILE**)stdout, "CONOUT$", "w", stdout); //redirect to allocated console;			

			PCMWAVEFORMAT wFormatLegacy = {0};
			WAVEFORMATEXTENSIBLE wFormat;
			memset(&wFormat, 0, sizeof(wFormat));

			bool isWinNT4 = IsWinNT4();

			if(isWinNT4) 
			{
				channels = 2;				
				usingFloat = false;
				wFormatLegacy.wf.wFormatTag = WAVE_FORMAT_PCM;
				wFormatLegacy.wf.nChannels = channels;
				wFormatLegacy.wf.nSamplesPerSec = sampleRate;
				wFormatLegacy.wBitsPerSample = 16;
				wFormatLegacy.wf.nBlockAlign = wFormatLegacy.wf.nChannels * wFormatLegacy.wBitsPerSample / 8;
				wFormatLegacy.wf.nAvgBytesPerSec = wFormatLegacy.wf.nBlockAlign * wFormatLegacy.wf.nSamplesPerSec;				
			}
			else
			{
				channels = channelCount;
				wFormat.Format.cbSize = sizeof(wFormat) - sizeof(wFormat.Format);
				wFormat.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
				wFormat.Format.nChannels = channels;
				wFormat.Format.nSamplesPerSec = sampleRate;
				wFormat.Format.wBitsPerSample = usingFloat ? 32 : 16;
				wFormat.Format.nBlockAlign = wFormat.Format.nChannels * wFormat.Format.wBitsPerSample / 8;
				wFormat.Format.nAvgBytesPerSec = wFormat.Format.nBlockAlign * wFormat.Format.nSamplesPerSec;
				wFormat.dwChannelMask = channels == 2 ? SPEAKER_STEREO : SPEAKER_QUAD;
				wFormat.Samples.wValidBitsPerSample = wFormat.Format.wBitsPerSample;
				wFormat.SubFormat = usingFloat ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
			}

			// Open waveout device
			int wResult = waveOutOpen(&hWaveOut, GetWaveOutDeviceId(), isWinNT4 ? (LPWAVEFORMATEX)&wFormatLegacy : &wFormat.Format, callback, (DWORD_PTR)&midiSynth, callbackType);
			if (wResult != MMSYSERR_NOERROR) {
				if (!errorShown[VSTMIDIDRV::FailedToOpen]) {
					MessageBox(NULL, _T("Failed to open waveform output device"), _T("VST MIDI Driver"), MB_OK | MB_ICONEXCLAMATION);
					errorShown[VSTMIDIDRV::FailedToOpen] = true;
				}
				return 2;
			}

			// Prepare headers
			chunks = bufferSize / chunkSize;
			WaveHdr = new WAVEHDR[chunks];
			LPSTR chunkStart = (LPSTR)buffer;
			DWORD chunkBytes = (usingFloat ? sizeof(float) * channels : sizeof(short) * channels) * chunkSize;
			DWORD bufferBytes = (usingFloat ? sizeof(float) * channels : sizeof(short) * channels) * bufferSize;
			for (UINT i = 0; i < chunks; i++) {

				WaveHdr[i].dwBufferLength = chunkBytes;
				WaveHdr[i].lpData = chunkStart;
				WaveHdr[i].dwFlags = 0L;
				WaveHdr[i].dwLoops = 0L;
				chunkStart += chunkBytes;

				wResult = waveOutPrepareHeader(hWaveOut, &WaveHdr[i], sizeof(WAVEHDR));
				if (wResult != MMSYSERR_NOERROR) {
					if (!errorShown[VSTMIDIDRV::FailedToPrepare]) {
						MessageBox(NULL, _T("Failed to Prepare Header"), _T("VST MIDI Driver"), MB_OK | MB_ICONEXCLAMATION);
						errorShown[VSTMIDIDRV::FailedToPrepare] = true;
					}

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
				CloseHandle(hThread);
				hThread = NULL;
			}
			int wResult = waveOutReset(hWaveOut);
			if (wResult != MMSYSERR_NOERROR) {
				if (!errorShown[VSTMIDIDRV::FailedToReset]) {
					MessageBox(NULL, _T("Failed to Reset WaveOut"), _T("VST MIDI Driver"), MB_OK | MB_ICONEXCLAMATION);
					errorShown[VSTMIDIDRV::FailedToReset] = true;
				}

				return 8;
			}

			for (UINT i = 0; i < chunks; i++) {
				wResult = waveOutUnprepareHeader(hWaveOut, &WaveHdr[i], sizeof(WAVEHDR));
				if (wResult != MMSYSERR_NOERROR) {
					if (!errorShown[VSTMIDIDRV::FailedToUnprepare]) {
						MessageBox(NULL, _T("Failed to Unprepare Wave Header"), _T("VST MIDI Driver"), MB_OK | MB_ICONEXCLAMATION);
						errorShown[VSTMIDIDRV::FailedToUnprepare] = true;
					}

					return 8;
				}
			}
			delete[] WaveHdr;
			WaveHdr = NULL;

			wResult = waveOutClose(hWaveOut);
			if (wResult != MMSYSERR_NOERROR) {
				if (!errorShown[VSTMIDIDRV::FailedToClose]) {
					MessageBox(NULL, _T("Failed to Close WaveOut"), _T("VST MIDI Driver"), MB_OK | MB_ICONEXCLAMATION);
					errorShown[VSTMIDIDRV::FailedToClose] = true;
				}

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
					if (!errorShown[VSTMIDIDRV::FailedToWrite]) {
						MessageBox(NULL, _T("Failed to write block to device"), _T("VST MIDI Driver"), MB_OK | MB_ICONEXCLAMATION);
						errorShown[VSTMIDIDRV::FailedToWrite] = true;
					}

					return 4;
				}
			}
			hThread = (HANDLE)_beginthreadex(NULL, 16384, &RenderingThread, this, 0, NULL);
			return 0;
		}

		int Pause(){
			if (waveOutPause(hWaveOut) != MMSYSERR_NOERROR) {
				if (!errorShown[VSTMIDIDRV::FailedToPause]) {
					MessageBox(NULL, _T("Failed to Pause wave playback"), _T("VST MIDI Driver"), MB_OK | MB_ICONEXCLAMATION);
					errorShown[VSTMIDIDRV::FailedToPause] = true;
				}

				return 9;
			}
			return 0;
		}

		int Resume(){
			if (waveOutRestart(hWaveOut) != MMSYSERR_NOERROR) {
				if (!errorShown[VSTMIDIDRV::FailedToResume]) {
					MessageBox(NULL, _T("Failed to Resume wave playback"), _T("VST MIDI Driver"), MB_OK | MB_ICONEXCLAMATION);
					errorShown[VSTMIDIDRV::FailedToResume] = true;
				}

				return 9;
			}
			return 0;
		}

		UINT64 GetPos(WORD channels){

			DWORD WRAP_BITS = 27;
			if(usingFloat) WRAP_BITS--;
			if(channels == 4) WRAP_BITS--;

			UINT64 WRAP_MASK = (UINT64)((1 << WRAP_BITS) - 1);
			int WRAP_THRESHOLD = 1 << (WRAP_BITS - 1);

			// Taking a snapshot to avoid possible thread interference
			UINT64 playPositionSnapshot = prevPlayPos;
			DWORD wrapCount = DWORD(playPositionSnapshot >> WRAP_BITS);
			DWORD wrappedPosition = DWORD(playPositionSnapshot & WRAP_MASK);

			MMTIME mmTime = {0};
			mmTime.wType = TIME_SAMPLES;

			if (waveOutGetPosition(hWaveOut, &mmTime, sizeof(MMTIME)) != MMSYSERR_NOERROR) {
				if (!errorShown[VSTMIDIDRV::FailedToGetPosition]) {
					MessageBox(NULL, _T("Failed to get current playback position"), _T("VST MIDI Driver"), MB_OK | MB_ICONEXCLAMATION);
					errorShown[VSTMIDIDRV::FailedToGetPosition] = true;
				}

				return 10;
			}
			if (mmTime.wType != TIME_SAMPLES) {
				if (!errorShown[VSTMIDIDRV::FailedToGetSamples]) {
					MessageBox(NULL, _T("Failed to get # of samples played"), _T("VST MIDI Driver"), MB_OK | MB_ICONEXCLAMATION);
					errorShown[VSTMIDIDRV::FailedToGetSamples] = true;
				}

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
			prevPlayPos = playPositionSnapshot = (UINT64)(mmTime.u.sample + (wrapCount << WRAP_BITS));

#ifdef _DEBUG
			std::cout << "VST MIDI Driver: GetPos()" << playPositionSnapshot << "\n";
#endif				
		
			return playPositionSnapshot;
		}

		static unsigned __stdcall RenderingThread(void* pthis)
		{			
			HANDLE WIN32DEF(AvSetMmThreadCharacteristicsW)(LPCWSTR TaskName, LPDWORD TaskIndex) = NULL;
			BOOL WIN32DEF(AvSetMmThreadPriority)(HANDLE AvrtHandle, MYAVRT_PRIORITY Priority) = NULL;

			HMODULE AvrtLib = LoadLibrary(_T("avrt.dll"));
			if (AvrtLib) 
			{
				*((void**)&AvSetMmThreadCharacteristicsW) = GetProcAddress(AvrtLib, "AvSetMmThreadCharacteristicsW");
				*((void**)&AvSetMmThreadPriority) = GetProcAddress(AvrtLib, "AvSetMmThreadPriority");	 
			}

			if (AvSetMmThreadCharacteristicsW && AvSetMmThreadPriority)
			{
				DWORD taskIndex = 0;
				HANDLE hAv = AvSetMmThreadCharacteristicsW(L"Audio", &taskIndex);
				
				BOOL result = FALSE;
				if (hAv)
					result = AvSetMmThreadPriority(hAv, AVRT_PRIORITY_CRITICAL);
				
				if(!result)
					SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
			}
			else 
			{
				SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
			}
			
			
			WaveOutWin32* _this = (WaveOutWin32*)pthis;

			while (!waveOut.stopProcessing)
			{
				bool allBuffersRendered = true;
				for (UINT i = 0; i < waveOut.chunks; i++) {
					if (waveOut.WaveHdr[i].dwFlags & WHDR_DONE) {
						allBuffersRendered = false;
						if (_this->usingFloat)
							midiSynth.RenderFloat((float*)waveOut.WaveHdr[i].lpData, waveOut.WaveHdr[i].dwBufferLength / (sizeof(float) * _this->channels));
						else
							midiSynth.Render((short*)waveOut.WaveHdr[i].lpData, waveOut.WaveHdr[i].dwBufferLength / (sizeof(short) * _this->channels));
						if (waveOutWrite(waveOut.hWaveOut, &waveOut.WaveHdr[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
							if (!_this->errorShown[VSTMIDIDRV::FailedToWrite]) {
								MessageBox(NULL, _T("Failed to write block to device"), _T("VST MIDI Driver"), MB_OK | MB_ICONEXCLAMATION);
								_this->errorShown[VSTMIDIDRV::FailedToWrite] = true;
							}

						}
					}
				}
				if (allBuffersRendered) {
					// Ensure the playback position is monitored frequently enough in order not to miss a wraparound
					waveOut.GetPos(_this->channels);
					WaitForSingleObject(waveOut.hEvent, 2000);
				}
			}

			if (AvrtLib) FreeLibrary(AvrtLib);
			AvrtLib = NULL;
			
			_endthreadex(0);
			return 0;
		}

	}waveOut;
	

	static class BassAsioOut
	{
	private:

		HINSTANCE bassAsio; // bassasio handle

		TCHAR* outputDriver;

		bool usingFloat;
		bool usingQPC;
		DWORD buflen;
		WORD channels;
		unsigned int samplerate;
		bool isASIO2WASAPI;

		volatile DWORD startTime;
		volatile LARGE_INTEGER startTimeQp;
		double	queryPerformanceUnit;

		TCHAR installPath[MAX_PATH];
		TCHAR bassAsioPath[MAX_PATH];
		TCHAR asio2WasapiPath[MAX_PATH];


		void InitializePaths()
		{
			GetModuleFileName(hinst_vst_driver, installPath, MAX_PATH);
			//PathRemoveFileSpec(installPath);
			TCHAR *chrP = _tcsrchr(installPath, '\\'); //removes SHLWAPI dependency for WIN NT4
			if(chrP) chrP[0] = 0;

			_tcscat_s(installPath, _T("\\vstmididrv\\"));

			_tcscpy_s(bassAsioPath, installPath);
			_tcscat_s(bassAsioPath, _T("bassasio_vstdrv.dll"));
		}

		bool LoadBassAsio()
		{
			// Load Bass Asio
			bassAsio = LoadLibrary(bassAsioPath);
			if (!bassAsio) return false;	
						
			_tcscpy_s(asio2WasapiPath, installPath);
			_tcscat_s(asio2WasapiPath, _T("ASIO2WASAPI_vstdrv.dll"));
			if (IsVistaOrNewer() && GetFileAttributes(asio2WasapiPath) != INVALID_FILE_ATTRIBUTES)
			{
				LOADBASSASIOFUNCTION(BASS_ASIO_AddDevice);
				
				char asio2WasapiAnsiPath[MAX_PATH] = { 0 };				
#ifdef UNICODE  
				WideCharToMultiByte(CP_ACP, 0, (LPCWSTR)asio2WasapiPath, -1, (char*)asio2WasapiAnsiPath, MAX_PATH, NULL, NULL);
#else
				strcpy_s(asio2WasapiAnsiPath, asio2WasapiPath);
#endif		
				static const GUID CLSID_ASIO2WASAPI_DRIVER = { 0x3981c4c8, 0xfe12, 0x4b0f, { 0x98, 0xa0, 0xd1, 0xb6, 0x67, 0xbd, 0xa6, 0x15 } };
				BASS_ASIO_AddDevice(&CLSID_ASIO2WASAPI_DRIVER, asio2WasapiAnsiPath, "VSTDriver-ASIO2WASAPI");
								
			}

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

		tstring GetAsioDriverName()
		{
			HKEY hKey;
			DWORD dwType = REG_SZ;
			DWORD dwSize = 0;
			TCHAR* regValue;
			tstring wresult;

			long result = RegOpenKeyEx(HKEY_CURRENT_USER, _T("Software\\VSTi Driver\\Output Driver"), 0, KEY_READ, &hKey);
			if (result == NO_ERROR)
			{
#ifdef WIN64
				result = RegQueryValueEx(hKey, _T("Bass ASIO x64"), NULL, &dwType, NULL, &dwSize);
				if (result == NO_ERROR && dwType == REG_SZ)
				{
					regValue = (TCHAR*) calloc( dwSize + sizeof(TCHAR), 1 );
					RegQueryValueEx(hKey, _T("Bass ASIO x64"), NULL, &dwType, (LPBYTE) regValue, &dwSize);
					wresult = regValue;
					free(regValue);
				}

				if (wresult.empty()) 
				{
					result = RegQueryValueEx(hKey, _T("Bass ASIO"), NULL, &dwType, NULL, &dwSize);
					if (result == NO_ERROR && dwType == REG_SZ)
					{
						regValue = (TCHAR*) calloc( dwSize + sizeof(TCHAR), 1 );
						RegQueryValueEx(hKey, _T("Bass ASIO"), NULL, &dwType, (LPBYTE) regValue, &dwSize);
						wresult = regValue;
						free(regValue);
					}

				}						

#else
				result = RegQueryValueEx(hKey, _T("Bass ASIO"), NULL, &dwType, NULL, &dwSize);
				if (result == NO_ERROR && dwType == REG_SZ)
				{
					regValue = (TCHAR*) calloc( dwSize + sizeof(TCHAR), 1 );
					RegQueryValueEx(hKey, _T("Bass ASIO"), NULL, &dwType, (LPBYTE) regValue, &dwSize);
					wresult = regValue;
					free(regValue);
				}

				if (wresult.empty()) 
				{
					result = RegQueryValueEx(hKey, _T("Bass ASIO x64"), NULL, &dwType, NULL, &dwSize);
					if (result == NO_ERROR && dwType == REG_SZ)
					{
						regValue = (TCHAR*) calloc( dwSize + sizeof(TCHAR), 1 );
						RegQueryValueEx(hKey, _T("Bass ASIO x64"), NULL, &dwType, (LPBYTE) regValue, &dwSize);
						wresult = regValue;
						free(regValue);
					}

				}
#endif				

				RegCloseKey(hKey);
			}

			return wresult;
		}

		void GetSelectedAsioDriver(int& selectedDeviceId, int& selectedChannelId)
		{
			selectedDeviceId = 0;
			selectedChannelId = 0;
			tstring selectedOutputDriver = GetAsioDriverName();
			if(selectedOutputDriver.empty()) return;

			size_t period = selectedOutputDriver.find(L'.');
			tstring sdeviceId = selectedOutputDriver.substr(0, period);
			selectedDeviceId = _ttoi(sdeviceId.c_str());

			size_t space = selectedOutputDriver.find(L' ');
			tstring schannelid = selectedOutputDriver.substr(period + 1, space - period - 1);
			selectedChannelId = _ttoi(schannelid.c_str());
		}

		DWORD GetPortBOffset()
		{
			DWORD portbOffset = 2;
			portbOffset = GetDwordData(_T("PortBOffset"), portbOffset);
			return portbOffset;
		}

	public:		

		BassAsioOut()
		{
			usingFloat = true;
			buflen = 0;
			channels = 2;
			bassAsio = NULL;
			isASIO2WASAPI = false;
		}

		bool getIsASIO2WASAPI() { return isASIO2WASAPI; }		

		int Init(unsigned int bufferSizeMS, unsigned int sampleRate, bool useFloat, WORD channelCount)
		{
			if (bassAsioPath[0] == NULL)
			{
				InitializePaths();
			}

			if (!LoadBassAsio())
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

				BASS_ASIO_SetNotify(AsioNotifyProc, this);

				BASS_ASIO_SetRate(sampleRate);

				sampleRate = (int)BASS_ASIO_GetRate();
				samplerate = sampleRate;

				buflen = bufferSizeMS == 0 ? 0 : DWORD(bufferSizeMS * (sampleRate / 1000.0f));				
				tstring selectedOutputDriver = GetAsioDriverName();
				if (selectedOutputDriver.find(_T("VSTDriver-ASIO2WASAPI")) != tstring::npos)
				{
					buflen = 0;
					isASIO2WASAPI = true;
				}

				channels = channelCount;

				BASS_ASIO_INFO info;
				BASS_ASIO_GetInfo(&info);

				// Enable 1st output channel
				if (!BASS_ASIO_ChannelEnable(FALSE, channelId, AsioProc, this)) return -2;

				usingFloat = useFloat;
				if(!BASS_ASIO_ChannelSetFormat(FALSE, channelId, usingFloat ? BASS_ASIO_FORMAT_FLOAT : BASS_ASIO_FORMAT_16BIT)) return -2;
				//if(!BASS_ASIO_ChannelSetRate(FALSE, channelId, sampleRate)) return -2;

				// Join the next channel to it (stereo)
				if (!BASS_ASIO_ChannelJoin(FALSE, (channelId + 1) % info.outputs, channelId)) return -2;

				if (channels == 4) {
					DWORD offset = GetPortBOffset();
					// Join the next channel to it (quad left)
					if (!BASS_ASIO_ChannelJoin(FALSE, (channelId + offset) % info.outputs, channelId)) return -2;

					// Join the next channel to it (quad right)
					if (!BASS_ASIO_ChannelJoin(FALSE, (channelId + offset + 1) % info.outputs, channelId)) return -2;
				}

			}

			return 0;
		}

		int Close(bool doDelay = true)
		{
			if (bassAsio)
			{
				// Stop ASIO device in case it was playing
				if (BASS_ASIO_IsStarted())
				{
					BASS_ASIO_Stop();
					if(doDelay) Sleep(20);

				}
				BASS_ASIO_Free();
				if(doDelay) Sleep(20);

				FreeLibrary(bassAsio);
				bassAsio = NULL;
			}


			return 0;
		}

		int Start()
		{
			if (bassAsio)
			{
				//queryPerformanceUnit = 0.0;
				LARGE_INTEGER tmpFreq;
				if(QueryPerformanceFrequency(&tmpFreq)) 
				{
					usingQPC = true;
					queryPerformanceUnit = 1.0 / (tmpFreq.QuadPart * 0.001);
					QueryPerformanceCounter(const_cast<LARGE_INTEGER *>(&startTimeQp));
				}
				else 
				{
					usingQPC = false;
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
				BASS_ASIO_ChannelPause(FALSE, -1);
			}

			return 0;
		}

		int Resume()
		{
			if (bassAsio)
			{				
				BASS_ASIO_ChannelReset(FALSE, -1, BASS_ASIO_RESET_PAUSE);
				if(usingQPC) QueryPerformanceCounter(const_cast<LARGE_INTEGER *>(&startTimeQp));
				else startTime = timeGetTime();
			}

			return 0;
		}

		DWORD GetPos()
		{
			int res;
			if(usingQPC)
			{				
				LONGLONG tmpStartTimeQp = startTimeQp.QuadPart;
				LARGE_INTEGER tmpCounter;
				QueryPerformanceCounter(&tmpCounter);
				res = int((tmpCounter.QuadPart - tmpStartTimeQp) * queryPerformanceUnit * (samplerate * 0.001));
			}
			else res = int((timeGetTime() - startTime) * (samplerate * 0.001));

#ifdef _DEBUG
			std::cout << "VST MIDI Driver: GetPos(): " << res << "\n";
#endif			
			if (res < 0) res = 0; // negative values can occur with Athlon64 X2 without dual core optimizations...
			return (DWORD)res;
		}

		static DWORD CALLBACK AsioProc(BOOL input, DWORD channel, void* buffer, DWORD length, void* user)
		{
			BassAsioOut* _this = (BassAsioOut*)user;
			if (_this->usingFloat)
			{
				midiSynth.RenderFloat((float*)buffer, length / (sizeof(float) * _this->channels));					
			}
			else
			{				
				midiSynth.Render((short*)buffer, length / (sizeof(short) * _this->channels));				
			}

			if (_this->usingQPC) QueryPerformanceCounter(const_cast<LARGE_INTEGER*>(&_this->startTimeQp));
			else _this->startTime = timeGetTime();
			
			return length;
		}

		static void CALLBACK resetAsioTimerProc(UINT uTimerID, UINT uMsg, DWORD_PTR dwUser, DWORD_PTR dw1, DWORD_PTR dw2)
		{		
			bassAsioOut.Close(false);
			bassAsioOut.Init(midiSynth.getBufferSizeMS(), midiSynth.getSampleRate(), midiSynth.getUsingFloat(), midiSynth.getChannels());
			bassAsioOut.Start();
		}

		static void CALLBACK AsioNotifyProc(DWORD notify, void* user)
		{
			if(notify == BASS_ASIO_NOTIFY_RESET)
				timeSetEvent(1, 1, resetAsioTimerProc, NULL, TIME_ONESHOT | TIME_CALLBACK_FUNCTION);			
		}
		

	}bassAsioOut;
		

	MidiSynth::MidiSynth()
	{
		lastOpenedPort = 0;			
		vstDriver = NULL;
	}	

	MidiSynth &MidiSynth::getInstance(){
		static MidiSynth *instance = new MidiSynth;
		return *instance;
	}

	void CALLBACK MidiSynth::ResetSynthTimerProc(UINT uTimerID, UINT uMsg, DWORD_PTR dwUser, DWORD_PTR dw1, DWORD_PTR dw2)
	{
		midiSynth.Close(true);
		midiSynth.Init(DWORD(-1));
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
				
				{
					ScopeLock<Win32Lock> scopeLock(&synthLock);

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
				}
				
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
				timeSetEvent(1, 1, ResetSynthTimerProc, NULL, TIME_ONESHOT | TIME_CALLBACK_FUNCTION);
				
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
				{
					ScopeLock<Win32Lock> scopeLock(&synthLock);

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
				}
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
				timeSetEvent(1, 1, ResetSynthTimerProc, NULL, TIME_ONESHOT | TIME_CALLBACK_FUNCTION);
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
		sampleRate = GetDwordData(_T("SampleRate"), sampleRate);
		return sampleRate;
	}

	DWORD GetBufferSize(){
		DWORD bufferSize = 80;	    
		bufferSize = GetDwordData(_T("BufferSize"), bufferSize);
		return bufferSize;
	}

	int GetGain(){
		int gain = 0;		
		gain = GetDwordData(_T("Gain"), gain);
		return gain;
	}

	WORD GetChannelCount(){		
		BOOL enabled = FALSE;		
		enabled = GetDwordData(_T("Use4ChannelMode"), enabled);
		return enabled ? 4 : 2;
	}

	bool IsShowVSTDialog(){
		BOOL showVstDialog = FALSE;
		showVstDialog = GetDwordData(_T("ShowVstDialog"),showVstDialog);
		return showVstDialog != FALSE;		
	}

	bool GetUsingFloat(){
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

	void MidiSynth::LoadSettings(){
		channels = GetChannelCount();
		outputGain = pow(10.0f, GetGain() * 0.05f);
		sampleRate = GetSampleRate();		
		bufferSizeMS = GetBufferSize();
		bufferSize = MillisToFrames(bufferSizeMS);
		chunkSizeMS = bufferSizeMS < 120 ? bufferSizeMS / 4 : bufferSizeMS < 200 ? bufferSizeMS / 5 : bufferSizeMS / 8;
		chunkSize = MillisToFrames(chunkSizeMS);
		midiLatencyMS = 0;
		midiLatency = MillisToFrames(midiLatencyMS);

		usingFloat = GetUsingFloat();
		useAsio = UseAsio();
		enableSinglePort32ChMode = GetEnableSinglePort32ChMode();
		keepLoaded = GetKeepDriverLoaded();
		

		if (chunkSize) {
			// Number of chunks should be ceil(bufferSize / chunkSize)
			DWORD chunks = (bufferSize + chunkSize - 1) / chunkSize;
			// Refine bufferSize as chunkSize * number of chunks, no less then the specified value
			bufferSize = chunks * chunkSize;
		}
	}	

	void MidiSynth::InitDialog(unsigned uDeviceID){	
		
		if(IsShowVSTDialog()) vstDriver->displayEditorModal(uDeviceID);
	}

	int MidiSynth::Init(unsigned uDeviceID){		
						
		LoadSettings();

		if (sampleRate == 0) sampleRate = 48000;
		if (!useAsio && !bufferSize) {
			bufferSize = MillisToFrames(80);
			chunkSize =  bufferSize / 4;
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
			wResult = bassAsioOut.Init(bufferSizeMS, sampleRate, usingFloat, channels);
			if (wResult) bassAsioOut.Close();
			else
			{
				sampleRate = (int)BASS_ASIO_GetRate();
				if (bassAsioOut.getIsASIO2WASAPI()) bufferSize = 0;
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

			wResult = waveOut.Init(usingFloat ? (void*)bufferf : (void*)buffer, bufferSize, chunkSize, sampleRate, usingFloat, channels);
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

	int MidiSynth::Reset(unsigned uDeviceID) {		

		//UINT wResult = useAsio ? bassAsioOut.Pause() : waveOut.Pause();
		//if (wResult) return wResult;

		{
			ScopeLock<Win32Lock> scopeLock(&synthLock);
			vstDriver->ResetDriver(uDeviceID);
			midiStream.Reset();
			statusBuff[uDeviceID] = 0;			
			isSinglePort32Ch = false;
			virtualPortNum = 0;			
		}

		//wResult = useAsio ? bassAsioOut.Resume() : waveOut.Resume();
		//return wResult;
		return 0;
	}

	bool MidiSynth::PreprocessMIDI(unsigned int& uDeviceID, DWORD& msg){
		//// support for F5 xx port select message (FSMP can send this message)
		if ((unsigned char)(msg) == 0xF5 && enableSinglePort32ChMode) {
			if (!isSinglePort32Ch) {

				vstDriver->setSinglePort32ChMode();
				InitDialog((unsigned int)!uDeviceID);
				isSinglePort32Ch = true;
			}
			//F5 xx command uses 1 as first port, but we use 0 and we have only A/B ports. This way 1 -> 0, 2 -> 1, 3 -> 0, 4 -> 1 and so on. 
			virtualPortNum = !(((unsigned char*)&msg)[1] & 1);
			
			return false;
		}

		if (isSinglePort32Ch) uDeviceID = virtualPortNum;
		////

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
				velocity = (midiVol[uDeviceID] == 0.0 || velocity == 0) ? 0 : (unsigned char)DwordMax(DWORD(velocity * midiVol[uDeviceID]), 1);
				((unsigned char*)&msg)[2] = velocity;
			}
		}
		////

		return true;		
	}

	bool MidiSynth::PreprocessSysEx(unsigned int& uDeviceID, unsigned char* bufpos, const DWORD len) {

		//// support for F5 xx port select message (FSMP can send this message)
		if (bufpos[0] == 0xF5 && len > 1 && enableSinglePort32ChMode) {
			if (!isSinglePort32Ch) {

				vstDriver->setSinglePort32ChMode();
				InitDialog((unsigned int)!uDeviceID);
				isSinglePort32Ch = true;
			}
			//F5 xx command uses 1 as first port, but we use 0 and we have only A/B ports. This way 1 -> 0, 2 -> 1, 3 -> 0, 4 -> 1 and so on.
			virtualPortNum = !(bufpos[1] & 1);

			return false;
		}

		if (isSinglePort32Ch) uDeviceID = virtualPortNum;
		////

		statusBuff[uDeviceID] = 0; //clear running status also in case of SysEx messages
		
		return true;
	}
	
	void MidiSynth::PushMIDI(unsigned uDeviceID, DWORD msg){
		
		ScopeLock<Win32Lock> scopeLock(&synthLock);
		
		if (!PreprocessMIDI(uDeviceID, msg))
			return;
		
		DWORD tmpTimestamp = useAsio ? DwordMin(bassAsioOut.GetPos() + midiLatency, bufferSize - 1) : (DWORD)((waveOut.GetPos(channels) + midiLatency) % bufferSize);
		midiStream.PutMessage(uDeviceID, msg, tmpTimestamp);						
	}

	void MidiSynth::PlaySysEx(unsigned uDeviceID, unsigned char *bufpos, DWORD len){

		ScopeLock<Win32Lock> scopeLock(&synthLock);
		
		if(!PreprocessSysEx(uDeviceID, bufpos, len)) 
			return;
		
		DWORD tmpTimestamp = useAsio ? DwordMin(bassAsioOut.GetPos() + midiLatency, bufferSize - 1) : (DWORD)((waveOut.GetPos(channels) + midiLatency) % bufferSize);
		midiStream.PutSysEx(uDeviceID, bufpos, len, tmpTimestamp);		
	}	

	void MidiSynth::SetVolume(unsigned uDeviceID, float volume){
		midiVol[uDeviceID] = volume;
	}

	void MidiSynth::Close(bool forceUnload){
		if (useAsio) {
			bassAsioOut.Pause();
			bassAsioOut.Close();
		}
		else {
			waveOut.Pause();
			waveOut.Close();
		}		

		if(!keepLoaded || forceUnload)
		{
			ScopeLock<Win32Lock> scopeLock(&synthLock);

			// Cleanup memory
			delete vstDriver;
			vstDriver = NULL;			
		}		
	}

}
