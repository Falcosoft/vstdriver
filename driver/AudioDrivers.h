/// Copyright (C) 2023 Zoltan Bacsko - Falcosoft

#pragma once

namespace VSTMIDIDRV
{
	class MidiSynth;

	class WaveOutWin32 {
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

		MidiSynth* parentSynth;
		
		static unsigned __stdcall RenderingThread(void* pthis);

	public:
		explicit WaveOutWin32(MidiSynth* pSynth);
		int Init(void* buffer, unsigned int bufferSize, unsigned int chunkSize, unsigned int sampleRate, bool useFloat, WORD channelCount);
		int Close();
		int Start();
		int Pause();
		int Resume();
		UINT64 GetPos(WORD channels);
		
	};	

	class BassAsioOut {
	private:
		HINSTANCE bassAsio; // bassasio handle		

		bool usingFloat;
		bool usingQPC;
		DWORD buflen;
		WORD channels;
		unsigned int samplerate;
		bool isASIO2WASAPI;

		volatile LARGE_INTEGER startTime;		
		double	queryPerformanceUnit;

		TCHAR installPath[MAX_PATH];
		TCHAR bassAsioPath[MAX_PATH];
		TCHAR asio2WasapiPath[MAX_PATH];

		MidiSynth* parentSynth;

		void InitializePaths();
		bool LoadBassAsio();
		tstring GetAsioDriverName();
		void GetSelectedAsioDriver(int& selectedDeviceId, int& selectedChannelId);
		DWORD GetPortBOffset();		

		static DWORD CALLBACK AsioProc(BOOL input, DWORD channel, void* buffer, DWORD length, void* user);
		static void CALLBACK resetAsioTimerProc(UINT uTimerID, UINT uMsg, DWORD_PTR dwUser, DWORD_PTR dw1, DWORD_PTR dw2);
		static void CALLBACK AsioNotifyProc(DWORD notify, void* user);

	public:
		explicit BassAsioOut(MidiSynth* pSynth);
		int Init(unsigned int bufferSizeMS, unsigned int sampleRate, bool useFloat, WORD channelCount);
		int Close(bool doDelay = true);
		int Start();
		int Pause();
		int Resume();
		DWORD GetPos();
		DWORD getBufPref();
		bool getIsASIO2WASAPI() { return isASIO2WASAPI; }
		DWORD getSampleRate() { return samplerate; }		
	};	
}
