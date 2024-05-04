/// Copyright (C) 2023 Zoltan Bacsko - Falcosoft

#include "stdafx.h"

/// Define BASSASIO functions as pointers
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


#include "../version.h"
#include "../external_packages/bassasio.h"
#include "VSTDriver.h"
#include "AudioDrivers.h"

extern "C" { extern HINSTANCE hinst_vst_driver; }

namespace VSTMIDIDRV {

#pragma region WaveOutWin32
	WaveOutWin32::WaveOutWin32(MidiSynth* pSynth) :
		parentSynth(pSynth),
		errorShown(),
		hWaveOut(),
		WaveHdr(),
		hEvent(),
		hThread(),
		chunks(),
		prevPlayPos(),
		stopProcessing(),
		usingFloat(),
		channels() {}

	int WaveOutWin32::Init(void* buffer, unsigned int bufferSize, unsigned int chunkSize, unsigned int sampleRate, bool useFloat, WORD channelCount) {
		usingFloat = useFloat;
		hEvent = NULL;
		hThread = NULL;
		hEvent = CreateEvent(NULL, false, true, NULL);
		DWORD_PTR callback = (DWORD_PTR)hEvent;	
		DWORD callbackType = CALLBACK_EVENT;

		//freopen_s((FILE**)stdout, "CONOUT$", "w", stdout); //redirect to allocated console;			

		PCMWAVEFORMAT wFormatLegacy = { 0 };
		WAVEFORMATEXTENSIBLE wFormat;
		memset(&wFormat, 0, sizeof(wFormat));

		bool isWinNT4 = IsWinNT4();

		if (isWinNT4)
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
		int wResult = waveOutOpen(&hWaveOut, GetWaveOutDeviceId(), isWinNT4 ? (LPWAVEFORMATEX)&wFormatLegacy : &wFormat.Format, callback, (DWORD_PTR)parentSynth, callbackType);
		if (wResult != MMSYSERR_NOERROR) {
			if (!errorShown[FailedToOpen]) {
				MessageBox(NULL, _T("Failed to open waveform output device"), _T("VST MIDI Driver"), MB_OK | MB_ICONEXCLAMATION);
				errorShown[FailedToOpen] = true;
			}
			return 2;
		}

		// Prepare headers
		chunks = bufferSize / chunkSize;
		WaveHdr = new WAVEHDR[chunks];
		LPSTR chunkStart = (LPSTR)buffer;
		DWORD chunkBytes = (usingFloat ? sizeof(float) * channels : sizeof(short) * channels) * chunkSize;
		for (UINT i = 0; i < chunks; i++) {

			WaveHdr[i].dwBufferLength = chunkBytes;
			WaveHdr[i].lpData = chunkStart;
			WaveHdr[i].dwFlags = 0L;
			WaveHdr[i].dwLoops = 0L;
			chunkStart += chunkBytes;

			wResult = waveOutPrepareHeader(hWaveOut, &WaveHdr[i], sizeof(WAVEHDR));
			if (wResult != MMSYSERR_NOERROR) {
				if (!errorShown[FailedToPrepare]) {
					MessageBox(NULL, _T("Failed to Prepare Header"), _T("VST MIDI Driver"), MB_OK | MB_ICONEXCLAMATION);
					errorShown[FailedToPrepare] = true;
				}

				return 3;
			}
		}
		stopProcessing = false;
		return 0;
	}

	int WaveOutWin32::Close() {
		stopProcessing = true;
		SetEvent(hEvent);
		if (hThread != NULL) {
			WaitForSingleObject(hThread, 2000);
			CloseHandle(hThread);
			hThread = NULL;
		}
		int wResult = waveOutReset(hWaveOut);
		if (wResult != MMSYSERR_NOERROR) {
			if (!errorShown[FailedToReset]) {
				MessageBox(NULL, _T("Failed to Reset WaveOut"), _T("VST MIDI Driver"), MB_OK | MB_ICONEXCLAMATION);
				errorShown[FailedToReset] = true;
			}

			return 8;
		}

		for (UINT i = 0; i < chunks; i++) {
			wResult = waveOutUnprepareHeader(hWaveOut, &WaveHdr[i], sizeof(WAVEHDR));
			if (wResult != MMSYSERR_NOERROR) {
				if (!errorShown[FailedToUnprepare]) {
					MessageBox(NULL, _T("Failed to Unprepare Wave Header"), _T("VST MIDI Driver"), MB_OK | MB_ICONEXCLAMATION);
					errorShown[FailedToUnprepare] = true;
				}

				return 8;
			}
		}
		delete[] WaveHdr;
		WaveHdr = NULL;

		wResult = waveOutClose(hWaveOut);
		if (wResult != MMSYSERR_NOERROR) {
			if (!errorShown[FailedToClose]) {
				MessageBox(NULL, _T("Failed to Close WaveOut"), _T("VST MIDI Driver"), MB_OK | MB_ICONEXCLAMATION);
				errorShown[FailedToClose] = true;
			}

			return 8;
		}
		if (hEvent != NULL) {
			CloseHandle(hEvent);
			hEvent = NULL;
		}
		return 0;
	}

	int WaveOutWin32::Start() {
		//getPosWraps = 0;
		prevPlayPos = 0;
		for (UINT i = 0; i < chunks; i++) {
			if (waveOutWrite(hWaveOut, &WaveHdr[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
				if (!errorShown[FailedToWrite]) {
					MessageBox(NULL, _T("Failed to write block to device"), _T("VST MIDI Driver"), MB_OK | MB_ICONEXCLAMATION);
					errorShown[FailedToWrite] = true;
				}

				return 4;
			}
		}
		hThread = (HANDLE)_beginthreadex(NULL, 16384, &RenderingThread, this, 0, NULL);
		return 0;
	}

	int WaveOutWin32::Pause() {
		if (waveOutPause(hWaveOut) != MMSYSERR_NOERROR) {
			if (!errorShown[FailedToPause]) {
				MessageBox(NULL, _T("Failed to Pause wave playback"), _T("VST MIDI Driver"), MB_OK | MB_ICONEXCLAMATION);
				errorShown[FailedToPause] = true;
			}

			return 9;
		}
		return 0;
	}

	int WaveOutWin32::Resume() {
		if (waveOutRestart(hWaveOut) != MMSYSERR_NOERROR) {
			if (!errorShown[FailedToResume]) {
				MessageBox(NULL, _T("Failed to Resume wave playback"), _T("VST MIDI Driver"), MB_OK | MB_ICONEXCLAMATION);
				errorShown[FailedToResume] = true;
			}

			return 9;
		}
		return 0;
	}

	UINT64 WaveOutWin32::GetPos(WORD channels) {

		DWORD WRAP_BITS = 27;
		if (usingFloat) WRAP_BITS--;
		if (channels == 4) WRAP_BITS--;

		UINT64 WRAP_MASK = (UINT64)((1 << WRAP_BITS) - 1);
		int WRAP_THRESHOLD = 1 << (WRAP_BITS - 1);

		// Taking a snapshot to avoid possible thread interference
		UINT64 playPositionSnapshot = prevPlayPos;
		DWORD wrapCount = DWORD(playPositionSnapshot >> WRAP_BITS);
		DWORD wrappedPosition = DWORD(playPositionSnapshot & WRAP_MASK);

		MMTIME mmTime = { 0 };
		mmTime.wType = TIME_SAMPLES;

		if (waveOutGetPosition(hWaveOut, &mmTime, sizeof(MMTIME)) != MMSYSERR_NOERROR) {
			if (!errorShown[FailedToGetPosition]) {
				MessageBox(NULL, _T("Failed to get current playback position"), _T("VST MIDI Driver"), MB_OK | MB_ICONEXCLAMATION);
				errorShown[FailedToGetPosition] = true;
			}

			return 10;
		}
		if (mmTime.wType != TIME_SAMPLES) {
			if (!errorShown[FailedToGetSamples]) {
				MessageBox(NULL, _T("Failed to get # of samples played"), _T("VST MIDI Driver"), MB_OK | MB_ICONEXCLAMATION);
				errorShown[FailedToGetSamples] = true;
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
		}
		else if (delta < 0) {
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

	unsigned __stdcall WaveOutWin32::RenderingThread(void* pthis)
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

			if (!result)
				SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
		}
		else
		{
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
		}


		WaveOutWin32* _this = static_cast<WaveOutWin32*>(pthis);

		while (!_this->stopProcessing)
		{
			bool allBuffersRendered = true;
			for (UINT i = 0; i < _this->chunks; i++) {
				if (_this->WaveHdr[i].dwFlags & WHDR_DONE) {
					allBuffersRendered = false;
					if (_this->usingFloat)
						_this->parentSynth->RenderFloat((float*)_this->WaveHdr[i].lpData, _this->WaveHdr[i].dwBufferLength / (sizeof(float) * _this->channels));
					else
						_this->parentSynth->Render((short*)_this->WaveHdr[i].lpData, _this->WaveHdr[i].dwBufferLength / (sizeof(short) * _this->channels));
					if (waveOutWrite(_this->hWaveOut, &_this->WaveHdr[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
						if (!_this->errorShown[FailedToWrite]) {
							MessageBox(NULL, _T("Failed to write block to device"), _T("VST MIDI Driver"), MB_OK | MB_ICONEXCLAMATION);
							_this->errorShown[FailedToWrite] = true;
						}

					}
				}
			}
			if (allBuffersRendered) {
				// Ensure the playback position is monitored frequently enough in order not to miss a wraparound
				_this->GetPos(_this->channels);
				WaitForSingleObject(_this->hEvent, 2000);
			}
		}

		if (AvrtLib) FreeLibrary(AvrtLib);
		AvrtLib = NULL;

		_endthreadex(0);
		return 0;
	}
#pragma endregion Win32 WinMM WaveOut class
	
///////////////////////////////////////

#pragma region BassAsioOut
	/// BassASIO Output class. Also utilized by internal ASIO2WASAPI plugin.
	BassAsioOut::BassAsioOut(MidiSynth* pSynth) :
		parentSynth(pSynth),
		startTimeQp(),
		installPath(),
		bassAsioPath(),
		asio2WasapiPath(),
		usingFloat(),
		buflen(),
		channels(),
		bassAsio(),
		isASIO2WASAPI(),
		usingQPC(),
		samplerate(),
		startTime(),
		queryPerformanceUnit() {}

	int BassAsioOut::Init(unsigned int bufferSizeMS, unsigned int sampleRate, bool useFloat, WORD channelCount)
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
			if (!BASS_ASIO_ChannelSetFormat(FALSE, channelId, usingFloat ? BASS_ASIO_FORMAT_FLOAT : BASS_ASIO_FORMAT_16BIT)) return -2;
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

	int BassAsioOut::Close(bool doDelay)
	{
		if (bassAsio)
		{
			// Stop ASIO device in case it was playing
			if (BASS_ASIO_IsStarted())
			{
				BASS_ASIO_Stop();
				if (doDelay) Sleep(20);

			}
			BASS_ASIO_Free();
			if (doDelay) Sleep(20);

			FreeLibrary(bassAsio);
			bassAsio = NULL;
		}


		return 0;
	}

	int BassAsioOut::Start()
	{
		if (bassAsio)
		{
			//queryPerformanceUnit = 0.0;
			LARGE_INTEGER tmpFreq;
			if (QueryPerformanceFrequency(&tmpFreq))
			{
				usingQPC = true;
				queryPerformanceUnit = 1.0 / (tmpFreq.QuadPart * 0.001);
				QueryPerformanceCounter(const_cast<LARGE_INTEGER*>(&startTimeQp));
			}
			else
			{
				usingQPC = false;
				startTime = timeGetTime();
			}

			if (!BASS_ASIO_Start(buflen, 0)) return -2;
		}

		return 0;
	}

	int BassAsioOut::Pause()
	{
		if (bassAsio)
		{
			BASS_ASIO_ChannelPause(FALSE, -1);
		}

		return 0;
	}

	int BassAsioOut::Resume()
	{
		if (bassAsio)
		{
			BASS_ASIO_ChannelReset(FALSE, -1, BASS_ASIO_RESET_PAUSE);
			if (usingQPC) QueryPerformanceCounter(const_cast<LARGE_INTEGER*>(&startTimeQp));
			else startTime = timeGetTime();
		}

		return 0;
	}

	DWORD BassAsioOut::GetPos()
	{
		int res;
		if (usingQPC)
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

	void BassAsioOut::InitializePaths()
	{
		GetModuleFileName(hinst_vst_driver, installPath, MAX_PATH);
		//PathRemoveFileSpec(installPath);
		TCHAR* chrP = _tcsrchr(installPath, '\\'); //removes SHLWAPI dependency for WIN NT4
		if (chrP) chrP[0] = 0;

		_tcscat_s(installPath, _T("\\vstmididrv\\"));

		_tcscpy_s(bassAsioPath, installPath);
		_tcscat_s(bassAsioPath, _T("bassasio_vstdrv.dll"));
	}

	bool BassAsioOut::LoadBassAsio()
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
			static const GUID CLSID_ASIO2WASAPI_DRIVER = { 0x3981c4c8, 0xfe12, 0x4b0f,{ 0x98, 0xa0, 0xd1, 0xb6, 0x67, 0xbd, 0xa6, 0x15 } };
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

	DWORD BassAsioOut::getBufPref()
	{
		BASS_ASIO_INFO info;
		BASS_ASIO_GetInfo(&info);
		return info.bufpref;
	}

	tstring BassAsioOut::GetAsioDriverName()
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
				regValue = (TCHAR*)calloc(dwSize + sizeof(TCHAR), 1);
				if (regValue)
				{
					RegQueryValueEx(hKey, _T("Bass ASIO x64"), NULL, &dwType, (LPBYTE)regValue, &dwSize);
					wresult = regValue;
					free(regValue);
				}
			}

			if (wresult.empty())
			{
				result = RegQueryValueEx(hKey, _T("Bass ASIO"), NULL, &dwType, NULL, &dwSize);
				if (result == NO_ERROR && dwType == REG_SZ)
				{
					regValue = (TCHAR*)calloc(dwSize + sizeof(TCHAR), 1);
					if (regValue)
					{
						RegQueryValueEx(hKey, _T("Bass ASIO"), NULL, &dwType, (LPBYTE)regValue, &dwSize);
						wresult = regValue;
						free(regValue);
					}
				}

			}

#else
			result = RegQueryValueEx(hKey, _T("Bass ASIO"), NULL, &dwType, NULL, &dwSize);
			if (result == NO_ERROR && dwType == REG_SZ)
			{
				regValue = (TCHAR*)calloc(dwSize + sizeof(TCHAR), 1);
				if (regValue)
				{
					RegQueryValueEx(hKey, _T("Bass ASIO"), NULL, &dwType, (LPBYTE)regValue, &dwSize);
					wresult = regValue;
					free(regValue);
				}
			}

			if (wresult.empty())
			{
				result = RegQueryValueEx(hKey, _T("Bass ASIO x64"), NULL, &dwType, NULL, &dwSize);
				if (result == NO_ERROR && dwType == REG_SZ)
				{
					regValue = (TCHAR*)calloc(dwSize + sizeof(TCHAR), 1);
					if (regValue)
					{
						RegQueryValueEx(hKey, _T("Bass ASIO x64"), NULL, &dwType, (LPBYTE)regValue, &dwSize);
						wresult = regValue;
						free(regValue);
					}
				}

			}
#endif				

			RegCloseKey(hKey);
		}

		return wresult;
	}

	void BassAsioOut::GetSelectedAsioDriver(int& selectedDeviceId, int& selectedChannelId)
	{
		selectedDeviceId = 0;
		selectedChannelId = 0;
		tstring selectedOutputDriver = GetAsioDriverName();
		if (selectedOutputDriver.empty()) return;

		size_t period = selectedOutputDriver.find(L'.');
		tstring sdeviceId = selectedOutputDriver.substr(0, period);
		selectedDeviceId = _ttoi(sdeviceId.c_str());

		size_t space = selectedOutputDriver.find(L' ');
		tstring schannelid = selectedOutputDriver.substr(period + 1, space - period - 1);
		selectedChannelId = _ttoi(schannelid.c_str());
	}

	DWORD BassAsioOut::GetPortBOffset()
	{
		DWORD portbOffset = 2;
		portbOffset = GetDwordData(_T("PortBOffset"), portbOffset);
		return portbOffset;
	}

	DWORD BassAsioOut::AsioProc(BOOL input, DWORD channel, void* buffer, DWORD length, void* user)
	{
		BassAsioOut* _this = static_cast<BassAsioOut*>(user);

		if (_this->usingFloat)
		{
			_this->parentSynth->RenderFloat((float*)buffer, length / (sizeof(float) * _this->channels));
		}
		else
		{
			_this->parentSynth->Render((short*)buffer, length / (sizeof(short) * _this->channels));
		}

		if (_this->usingQPC) QueryPerformanceCounter(const_cast<LARGE_INTEGER*>(&_this->startTimeQp));
		else _this->startTime = timeGetTime();

		return length;
	}

	void BassAsioOut::resetAsioTimerProc(UINT uTimerID, UINT uMsg, DWORD_PTR dwUser, DWORD_PTR dw1, DWORD_PTR dw2)
	{
		BassAsioOut* _this = reinterpret_cast<BassAsioOut*>(dwUser);
		_this->Close(false);
		_this->Init(_this->parentSynth->getBufferSizeMS(), _this->parentSynth->getSampleRate(), _this->parentSynth->getUsingFloat(), _this->parentSynth->getChannels());
		_this->Start();
	}

	void BassAsioOut::AsioNotifyProc(DWORD notify, void* user)
	{
		if (notify == BASS_ASIO_NOTIFY_RESET)
			timeSetEvent(1, 1, resetAsioTimerProc,(DWORD_PTR)user, TIME_ONESHOT | TIME_CALLBACK_FUNCTION);
	}
#pragma endregion BassASIO Output class. Also utilized by internal ASIO2WASAPI plugin.
}