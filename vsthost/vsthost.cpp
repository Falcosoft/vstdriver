// vsthost.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include <process.h>
#include "../version.h"

// #define LOG_EXCHANGE
// #define LOG
// #define MINIDUMP

#define USER32DEF(f) (WINAPI *f)
#define LOADUSER32FUNCTION(f) *((void**)&f)=GetProcAddress(user32,#f)

#define WM_ICONMSG  WM_APP + 1

#if(_WIN32_WINNT < 0x0500) 
	#define SM_CXVIRTUALSCREEN  78
	#define SM_CYVIRTUALSCREEN  79
	#define ASFW_ANY    ((DWORD)-1)
#endif 

enum
{
	BUFFER_SIZE = 4800  //matches better for typical 48/96/192 kHz
};

namespace Command {
	enum : uint32_t
	{
		GetChunkData = 1,
		SetChunkData = 2,
		HasEditor = 3,
		DisplayEditorModal = 4,
		SetSampleRate = 5,
		Reset = 6,
		SendMidiEvent = 7,
		SendMidiSysExEvent = 8,
		RenderAudioSamples = 9,
		DisplayEditorModalThreaded = 10,
		RenderAudioSamples4channel = 11,
		SetHighDpiMode = 12,
		SetSinglePort32ChMode = 13,
		InitSysTray = 14

	};
};

namespace Response {
	enum : uint32_t
	{
		NoError = 0,
		CannotLoadVstiDll = 6,
		CannotGetProcAddress = 7,
		NotAVsti = 8,
		CannotReset = 8,
		VstiIsNotAMidiSynth = 9,
		CannotSetSampleRate = 10,
		CommandUnknown = 12,
	};
};

namespace Error {
	enum : uint32_t
	{
		InvalidCommandLineArguments = 1,
		MalformedChecksum = 2,
		ChecksumMismatch = 3,
		//Comctl32LoadFailed = 4,
		ComInitializationFailed = 5,
	};
};

#ifdef WIN64
	static wchar_t bitnessStr[8] =L" 64-bit"; 
#else
	static wchar_t bitnessStr[8] =L" 32-bit"; 
#endif	

static wchar_t clientBitnessStr[8] = { 0 };
static wchar_t outputModeStr[8] = { 0 };

bool need_idle = false;
bool idle_started = false;

static HINSTANCE user32 = NULL;

#if(_WIN32_WINNT < 0x0500) 
	static HANDLE USER32DEF(AllowSetForegroundWindow)(DWORD dwProcessId) = NULL;
#endif

static HANDLE USER32DEF(SetThreadDpiAwarenessContext)(HANDLE dpiContext) = NULL;
static HANDLE highDpiMode = NULL;

static HWND editorHandle[2] = { NULL };
static HANDLE threadHandle[2] = { NULL };
static bool isPortActive[2] = { false };

static bool isSinglePort32Ch = false;

static uint32_t num_outputs_per_port = 2;
static uint32_t sample_rate = 48000;

static uint32_t sample_pos = 0;

static DWORD MainThreadId;
static char* dll_dir = NULL;

static char product_string[256] = { 0 };
static wchar_t midiClient[64] = { 0 };
static wchar_t trayTip[MAX_PATH] = { 0 };

static HANDLE null_file = NULL;
static HANDLE pipe_in = NULL;
static HANDLE pipe_out = NULL;

static AEffect* pEffect[2] = { NULL };

static VstTimeInfo vstTimeInfo = { 0 };

static HWND trayWndHandle = NULL;
static volatile int aboutBoxResult = 0;
static bool isSCVA = false;

static const unsigned char gmReset[] = { 0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7 };
static const unsigned char gsReset[] = { 0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7 };
static const unsigned char xgReset[] = { 0xF0, 0x43, 0x10, 0x4C, 0x00, 0x00, 0x7E, 0x00, 0xF7 };
static const unsigned char gm2Reset[] = { 0xF0, 0x7E, 0x7F, 0x09, 0x03, 0xF7 };

static const DWORD resetEventCount = 64; //4 messages for 16 channels. These can be useful if a synth does not support any SysEx reset messages. 
struct ResetVstEvents
{
	VstInt32 numEvents;
	VstIntPtr reserved;
	VstEvent* events[resetEventCount];
};

static VstMidiEvent resetMidiEvents[resetEventCount] = { 0 };
static ResetVstEvents resetEvents = { 0 };

#ifdef LOG
void Log(LPCTSTR szFormat, ...)
{
	static FILE* fpLog = NULL;
	va_list l;
	va_start(l, szFormat);

	if (!fpLog)
	{
		TCHAR logfile[MAX_PATH];
		GetEnvironmentVariable(_T("TEMP"), logfile, _countof(logfile));
		lstrcat(logfile, _T("\\vsthost.log"));
		fpLog = _tfopen(logfile, _T("a"));
	}
	if (fpLog)
	{
		_vftprintf(fpLog, szFormat, l);
		fflush(fpLog);  // make sure it's out!
	}
	va_end(l);
}
#else
void NoLog(LPCTSTR szFormat, ...) {}
#define Log while(0) NoLog
#endif

#pragma comment(lib,"Version.lib") 
static wchar_t* GetFileVersion(wchar_t* filePath, wchar_t* result)
{
	DWORD               dwSize = 0;
	BYTE* pVersionInfo = NULL;
	VS_FIXEDFILEINFO* pFileInfo = NULL;
	UINT                pLenFileInfo = 0;
	wchar_t tmpBuff[MAX_PATH];

	if(!filePath)
		GetModuleFileName(NULL, tmpBuff, MAX_PATH);
	else
		lstrcpy(tmpBuff, filePath);

	dwSize = GetFileVersionInfoSize(tmpBuff, NULL);
	if (dwSize == 0)
	{           
		return NULL;
	}

	pVersionInfo = new BYTE[dwSize]; 

	if (!GetFileVersionInfo(tmpBuff, 0, dwSize, pVersionInfo))
	{           
		delete[] pVersionInfo;
		return NULL;
	}

	if (!VerQueryValue(pVersionInfo, TEXT("\\"), (LPVOID*)&pFileInfo, &pLenFileInfo))
	{            
		delete[] pVersionInfo;
		return NULL;
	}      

	lstrcat(result, L"version: ");
	_ultow_s((pFileInfo->dwFileVersionMS >> 16) & 0xffff, tmpBuff, MAX_PATH, 10);
	lstrcat(result, tmpBuff);
	lstrcat(result, L".");
	_ultow_s((pFileInfo->dwFileVersionMS) & 0xffff, tmpBuff, MAX_PATH, 10);
	lstrcat(result, tmpBuff);	
	lstrcat(result, L".");
	_ultow_s((pFileInfo->dwFileVersionLS >> 16) & 0xffff, tmpBuff, MAX_PATH, 10);
	lstrcat(result, tmpBuff);
	//lstrcat(result, L".");
	//lstrcat(result, _ultow((pFileInfo->dwFileVersionLS) & 0xffff, tmpBuff, 10));

	return result;
}

static bool IsWinNT4()
{
	OSVERSIONINFOEX osvi;
	BOOL bOsVersionInfoEx;
	ZeroMemory(&osvi, sizeof(OSVERSIONINFOEX));
	osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEX);
	bOsVersionInfoEx = GetVersionEx((OSVERSIONINFO*)&osvi);
	if (bOsVersionInfoEx == FALSE) return FALSE;
	if (VER_PLATFORM_WIN32_NT == osvi.dwPlatformId && osvi.dwMajorVersion == 4)
		return true;
	return false;
}

static class MutexWin32
{
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
} dialogMutex;

struct myVstEvent
{
	struct myVstEvent* next;
	unsigned port;
	union
	{
		VstMidiEvent midiEvent;
		VstMidiSysexEvent sysexEvent;
	} ev;
} *evChain = NULL, * evTail = NULL;

void freeChain()
{
	myVstEvent* ev = evChain;
	while (ev)
	{
		myVstEvent* next = ev->next;
		if (ev->port && ev->ev.sysexEvent.type == kVstSysExType) free(ev->ev.sysexEvent.sysexDump);
		free(ev);
		ev = next;
	}
	evChain = NULL;
	evTail = NULL;
}

#ifdef LOG_EXCHANGE
unsigned exchange_count = 0;
#endif

void put_bytes(const void* out, uint32_t size)
{
	DWORD dwWritten;
	WriteFile(pipe_out, out, size, &dwWritten, NULL);
#ifdef LOG_EXCHANGE
	TCHAR logfile[MAX_PATH];
	_stprintf_s(logfile, _T("C:\\temp\\log\\bytes_%08u.out"), exchange_count++);
	FILE* f = _tfopen(logfile, _T("wb"));
	fwrite(out, 1, size, f);
	fclose(f);
#endif
}

void put_code(uint32_t code)
{
	put_bytes(&code, sizeof(code));
}

void get_bytes(void* in, uint32_t size)
{
	DWORD dwRead;
	if (!ReadFile(pipe_in, in, size, &dwRead, NULL) || dwRead < size)
	{
		memset(in, 0, size);
#ifdef LOG_EXCHANGE
		TCHAR logfile[MAX_PATH];
		_stprintf_s(logfile, _T("C:\\temp\\log\\bytes_%08u.err"), exchange_count++);
		FILE* f = _tfopen(logfile, _T("wb"));
		_ftprintf(f, _T("Wanted %u bytes, got %u"), size, dwRead);
		fclose(f);
#endif
	}
	else
	{
#ifdef LOG_EXCHANGE
		TCHAR logfile[MAX_PATH];
		_stprintf_s(logfile, _T("C:\\temp\\log\\bytes_%08u.in"), exchange_count++);
		FILE* f = _tfopen(logfile, _T("wb"));
		fwrite(in, 1, size, f);
		fclose(f);
#endif
	}
}

uint32_t get_code()
{
	uint32_t code;
	get_bytes(&code, sizeof(code));
	return code;
}

void getChunk(AEffect* pEffect, std::vector<uint8_t>& out)
{
	out.resize(0);
	uint32_t unique_id = pEffect->uniqueID;
	append_be(out, unique_id);
	bool type_chunked = !!(pEffect->flags & effFlagsProgramChunks);
	append_be(out, type_chunked);
	if (!type_chunked)
	{
		uint32_t num_params = pEffect->numParams;
		append_be(out, num_params);
		for (unsigned i = 0; i < num_params; ++i)
		{
			float parameter = pEffect->getParameter(pEffect, i);
			append_be(out, parameter);
		}
	}
	else
	{
		void* chunk;
		uint32_t size = (uint32_t)pEffect->dispatcher(pEffect, effGetChunk, 0, 0, &chunk, 0);
		append_be(out, size);
		size_t chunk_size = out.size();
		out.resize(chunk_size + size);
		if (size) memcpy(&out[chunk_size], chunk, size);
	}
}

void setChunk(AEffect* pEffect, std::vector<uint8_t> const& in)
{
	unsigned size = (unsigned)in.size();
	if (pEffect && size)
	{
#if (defined(_MSC_VER) && (_MSC_VER < 1600))
		const uint8_t* inc = &in.front();
#else
		const uint8_t* inc = in.data();
#endif
		uint32_t effect_id;
		retrieve_be(effect_id, inc, size);
		if (effect_id != pEffect->uniqueID) return;
		bool type_chunked;
		retrieve_be(type_chunked, inc, size);
		if (type_chunked != !!(pEffect->flags & effFlagsProgramChunks)) return;
		if (!type_chunked)
		{
			uint32_t num_params;
			retrieve_be(num_params, inc, size);
			if (num_params != pEffect->numParams) return;
			for (unsigned i = 0; i < num_params; ++i)
			{
				float parameter;
				retrieve_be(parameter, inc, size);
				pEffect->setParameter(pEffect, i, parameter);
			}
		}
		else
		{
			uint32_t chunk_size;
			retrieve_be(chunk_size, inc, size);
			if (chunk_size > size) return;
			pEffect->dispatcher(pEffect, effSetChunk, 0, chunk_size, (void*)inc, 0);
		}
	}
}


static BOOL settings_save(AEffect* pEffect)
{
	BOOL retResult = FALSE;
	long lResult;
	DWORD dwType = REG_SZ;
	HKEY hKey;
	wchar_t vst_path[MAX_PATH] = { 0 };
	ULONG size;
	lResult = RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\VSTi Driver", 0, KEY_READ, &hKey);
	if (lResult == ERROR_SUCCESS)
	{
		lResult = RegQueryValueEx(hKey, L"plugin", NULL, &dwType, NULL, &size);
		if (lResult == ERROR_SUCCESS && dwType == REG_SZ)
		{
			RegQueryValueEx(hKey, L"plugin", NULL, &dwType, (LPBYTE)vst_path, &size);
			wchar_t* chrP = wcsrchr(vst_path, '.'); // removes extension
			if (chrP) chrP[0] = 0;
			lstrcat(vst_path, L".set");

			HANDLE fileHandle = CreateFile(vst_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

			if (fileHandle != INVALID_HANDLE_VALUE)
			{
				std::vector<uint8_t> chunk;
				if (pEffect) getChunk(pEffect, chunk);
#if (defined(_MSC_VER) && (_MSC_VER < 1600))

				if (chunk.size() > (2 * sizeof(uint32_t) + sizeof(bool))) retResult = WriteFile(fileHandle, &chunk.front(), (DWORD)chunk.size(), &size, NULL);
#else

				if (chunk.size() > (2 * sizeof(uint32_t) + sizeof(bool))) retResult = WriteFile(fileHandle, chunk.data(), (DWORD)chunk.size(), &size, NULL);
#endif

				CloseHandle(fileHandle);
			}

		}
		RegCloseKey(hKey);
	}

	return retResult;
}

static void getEditorPosition(int port, int& x, int& y)
{
	HKEY hKey;

	long result = RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\VSTi Driver", 0, KEY_READ, &hKey);
	if (result == NO_ERROR)
	{
		DWORD size = 4;
		if (!port)
		{
			RegQueryValueEx(hKey, L"PortAWinPosX", NULL, NULL, (LPBYTE)&x, &size);
			RegQueryValueEx(hKey, L"PortAWinPosY", NULL, NULL, (LPBYTE)&y, &size);
		}
		else
		{
			RegQueryValueEx(hKey, L"PortBWinPosX", NULL, NULL, (LPBYTE)&x, &size);
			RegQueryValueEx(hKey, L"PortBWinPosY", NULL, NULL, (LPBYTE)&y, &size);

		}

		RegCloseKey(hKey);

		// Deal with changed multimonitor setup to prevent dialogs positioned outside of currently available desktop.
		int xWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN); // 0 result means error. This can happen on NT4
		int yWidth = GetSystemMetrics(SM_CYVIRTUALSCREEN); // 0 result means error. This can happen on NT4
		if (xWidth && yWidth)
		{
			if (xWidth - 24 < x || yWidth - 24 < y || y < (GetSystemMetrics(SM_CYCAPTION) / 2 * -1))
			{
				x = 0xFFFFFF;
				y = 0xFFFFFF;
			}

			return;
		}

		xWidth = GetSystemMetrics(SM_CXSCREEN); //for systems that support only primary monitor.
		yWidth = GetSystemMetrics(SM_CYSCREEN);
		if (xWidth - 24 < x || yWidth - 24 < y || y < (GetSystemMetrics(SM_CYCAPTION) / 2 * -1))
		{
			x = 0xFFFFFF;
			y = 0xFFFFFF;
		}
	}
}


static void setEditorPosition(int port, int x, int y)
{
	HKEY hKey;

	long result = RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\VSTi Driver", 0, KEY_READ | KEY_WRITE, &hKey);
	if (result == NO_ERROR)
	{
		DWORD size = 4;
		if (!port)
		{
			RegSetValueEx(hKey, L"PortAWinPosX", NULL, REG_DWORD, (LPBYTE)&x, size);
			RegSetValueEx(hKey, L"PortAWinPosY", NULL, REG_DWORD, (LPBYTE)&y, size);
		}
		else
		{
			RegSetValueEx(hKey, L"PortBWinPosX", NULL, REG_DWORD, (LPBYTE)&x, size);
			RegSetValueEx(hKey, L"PortBWinPosY", NULL, REG_DWORD, (LPBYTE)&y, size);

		}

		RegCloseKey(hKey);
	}
}

static void sendSysExEvent(char* sysExBytes, int size)
{
	static VstMidiSysexEvent syxEvent = { 0 };
	static VstEvents events = { 0 };

	syxEvent.byteSize = sizeof(syxEvent);
	syxEvent.dumpBytes = size;
	syxEvent.sysexDump = sysExBytes;
	syxEvent.type = kVstSysExType;

	events.events[0] = (VstEvent*)&syxEvent;
	events.numEvents = 1;

	pEffect[0]->dispatcher(pEffect[0], effProcessEvents, 0, 0, &events, 0);
	pEffect[1]->dispatcher(pEffect[1], effProcessEvents, 0, 0, &events, 0);

}

static void InitSimpleResetEvents()
{
	resetEvents.numEvents = resetEventCount;
	resetEvents.reserved = 0;

	for (int i = 0; i <= 15; i++)
	{
		DWORD msg, index;		

		msg = (0xB0 | i) | (0x40 << 8); //Sustain off		
		index = i * 4;
		memcpy(&resetMidiEvents[index].midiData, &msg, 3);
		resetMidiEvents[index].type = kVstMidiType;
		resetMidiEvents[index].byteSize = sizeof(VstMidiEvent);
		resetMidiEvents[index].flags = VstMidiEventFlags::kVstMidiEventIsRealtime;		
		resetEvents.events[index] = (VstEvent*)&resetMidiEvents[index];

		msg = (0xB0 | i) | (0x7B << 8); //All Notes off
		index = i * 4 + 1;
		memcpy(&resetMidiEvents[index].midiData, &msg, 3);
		resetMidiEvents[index].type = kVstMidiType;
		resetMidiEvents[index].byteSize = sizeof(VstMidiEvent);
		resetMidiEvents[index].flags = VstMidiEventFlags::kVstMidiEventIsRealtime;		
		resetEvents.events[index] = (VstEvent*)&resetMidiEvents[index];

		msg = (0xB0 | i) | (0x79 << 8);  //All Controllers off
		index = i * 4 + 2;
		memcpy(&resetMidiEvents[index].midiData, &msg, 3);
		resetMidiEvents[index].type = kVstMidiType;
		resetMidiEvents[index].byteSize = sizeof(VstMidiEvent);
		resetMidiEvents[index].flags = VstMidiEventFlags::kVstMidiEventIsRealtime;		
		resetEvents.events[index] = (VstEvent*)&resetMidiEvents[index];

		msg = (0xB0 | i) | (0x78 << 8);  //All Sounds off
		index = i * 4 + 3;
		memcpy(&resetMidiEvents[index].midiData, &msg, 3);
		resetMidiEvents[index].type = kVstMidiType;
		resetMidiEvents[index].byteSize = sizeof(VstMidiEvent);
		resetMidiEvents[index].flags = VstMidiEventFlags::kVstMidiEventIsRealtime;		
		resetEvents.events[index] = (VstEvent*)&resetMidiEvents[index];
	}

}

static void sendSimpleResetEvents()
{
	pEffect[0]->dispatcher(pEffect[0], effProcessEvents, 0, 0, &resetEvents, 0);
	pEffect[1]->dispatcher(pEffect[1], effProcessEvents, 0, 0, &resetEvents, 0);		
}

struct MyDLGTEMPLATE : DLGTEMPLATE
{
	WORD ext[3];
	MyDLGTEMPLATE()
	{
		memset(this, 0, sizeof(*this));
	};
};

INT_PTR CALLBACK EditorProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{

	AEffect* effect;
	static VstInt16 extraHeight[2] = { 0, 0 };
	static bool sameThread = true;
	static HWND checkBoxWnd[2] = { NULL, NULL };
	static HWND buttonWnd[2] = { NULL, NULL };
	static HFONT hFont[2] = { NULL, NULL };
	static float dpiMul;
	static int timerPeriodMS = 30;

	int portNum = 0;

	switch (msg)
	{
	case WM_INITDIALOG:
		{			
			effect = (AEffect*)lParam;

			if (effect)
			{
				/*
				falco:
				DPI adjustment for NT4/W2K/XP that do not use DPI virtualization but can use large font (125% - 120 DPI) and actually all other DPI settings.
				96 DPI is 100%. There is no separate DPI for X/Y on Windows.
				Since we have no DPI Aware manifest on Vista+ DPI virtualization is used.
				It's better than setting DPI awareness to true since allmost all VST 2.0 Editors know nothing about DPI scaling. So we let modern Windows handle this.
				*/
				dialogMutex.Enter();				

				HDC screen = GetDC(0);
				dpiMul = (float)(GetDeviceCaps(screen, LOGPIXELSY)) / 96.0f;
				ReleaseDC(0, screen);

				portNum = *(int*)effect->user;

				isPortActive[portNum] = true;
				editorHandle[portNum] = hwnd;

				SetWindowLongPtr(hwnd, GWLP_USERDATA, lParam);

				wchar_t wText[18] = L"VST Editor port ";
				wchar_t intCnst[] = { 'A' + portNum };
				wcsncat(wText, intCnst, 1);

				SetWindowText(hwnd, wText);

				if (GetCurrentThreadId() != MainThreadId)
				{
					extraHeight[portNum] = (int)(24 * dpiMul);
					sameThread = false;
				}				

				SetTimer(hwnd, 1, timerPeriodMS, 0);
				ERect* eRect = NULL;
				effect->dispatcher(effect, effEditGetRect, 0, 0, &eRect, 0); // dummy call in order S-YXG50 debug panel mode to work...
				effect->dispatcher(effect, effEditOpen, 0, 0, hwnd, 0);
				effect->dispatcher(effect, effSetEditKnobMode, 0, 2, NULL, 0); // Set knob mode to linear(2)				
				effect->dispatcher(effect, effEditGetRect, 0, 0, &eRect, 0);
				if (eRect)
				{
					int width = eRect->right - eRect->left;
					int height = eRect->bottom - eRect->top + extraHeight[portNum];
					if (width < (int)(200 * dpiMul))
						width = (int)(200 * dpiMul);
					if (height < (int)(50 * dpiMul))
						height = (int)(50 * dpiMul);
					RECT wRect;
					SetRect(&wRect, 0, 0, width, height);
					AdjustWindowRectEx(&wRect, GetWindowLong(hwnd, GWL_STYLE), FALSE, GetWindowLong(hwnd, GWL_EXSTYLE));
					width = wRect.right - wRect.left;
					height = wRect.bottom - wRect.top;

					int xPos = 0xFFFFFF; //16M not likely to be real window position
					int yPos = 0xFFFFFF;
					getEditorPosition(portNum, xPos, yPos);

					if (xPos == 0xFFFFFF || yPos == 0xFFFFFF)
						SetWindowPos(hwnd, HWND_TOP, 0, 0, width, height, SWP_NOMOVE);
					else
						SetWindowPos(hwnd, HWND_TOP, xPos, yPos, width, height, 0);


					if (!sameThread)
					{
						LOGFONT lf = { 0 };

						checkBoxWnd[portNum] = CreateWindowEx(NULL, L"BUTTON", L"Always on Top", WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX, (int)(5 * dpiMul), eRect->bottom - eRect->top + (int)(3 * dpiMul), (int)(100 * dpiMul), (int)(20 * dpiMul), hwnd, NULL, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
						SendMessage(checkBoxWnd[portNum], BM_SETCHECK, BST_CHECKED, 0);

						buttonWnd[portNum] = CreateWindowEx(NULL, L"BUTTON", L"Save Settings", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, width - (int)(90 * dpiMul), eRect->bottom - eRect->top + (int)(2 * dpiMul), (int)(80 * dpiMul), (int)(20 * dpiMul), hwnd, NULL, ((LPCREATESTRUCT)lParam)->hInstance, NULL);

						GetObject(GetStockObject(DEFAULT_GUI_FONT), sizeof(LOGFONT), &lf);
						hFont[portNum] = CreateFontIndirect(&lf);
						SendMessage(checkBoxWnd[portNum], WM_SETFONT, (WPARAM)hFont[portNum], TRUE);
						SendMessage(buttonWnd[portNum], WM_SETFONT, (WPARAM)hFont[portNum], TRUE);

						SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
					}
					else
					{
						effect->dispatcher(effect, effSetSampleRate, 0, 0, 0, float(sample_rate));
						effect->dispatcher(effect, effSetBlockSize, 0, (int)(sample_rate * timerPeriodMS * 0.001), 0, 0);
						sample_pos = 0;
					}
				}

				dialogMutex.Leave();
			}
			SetForegroundWindow(hwnd);
		}
		break;
	case WM_SIZE: //Fixes SC-VA display bug after parts section opened/closed
		effect = (AEffect*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
		portNum = *(int*)effect->user;

		if (effect && wParam == SIZE_RESTORED)
		{
			dialogMutex.Enter();
			ERect* eRect = 0;
			effect->dispatcher(effect, effEditGetRect, 0, 0, &eRect, 0);
			if (eRect)
			{
				int width = eRect->right - eRect->left;
				int height = eRect->bottom - eRect->top + extraHeight[portNum];
				if (width < (int)(200 * dpiMul))
					width = (int)(200 * dpiMul);
				if (height < (int)(50 * dpiMul))
					height = (int)(50 * dpiMul);
				RECT wRect;
				SetRect(&wRect, 0, 0, width, height);
				AdjustWindowRectEx(&wRect, GetWindowLong(hwnd, GWL_STYLE), FALSE, GetWindowLong(hwnd, GWL_EXSTYLE));
				width = wRect.right - wRect.left;
				height = wRect.bottom - wRect.top;
				SetWindowPos(hwnd, HWND_TOP, 0, 0, width, height, SWP_NOMOVE);

				if (!sameThread)
				{
					SetWindowPos(checkBoxWnd[portNum], NULL, (int)(5 * dpiMul), eRect->bottom - eRect->top + (int)(3 * dpiMul), (int)(100 * dpiMul), (int)(20 * dpiMul), SWP_NOZORDER);
					if (SendMessage(checkBoxWnd[portNum], BM_GETCHECK, 0, 0) == BST_CHECKED)
						SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);

					SetWindowPos(buttonWnd[portNum], NULL, width - (int)(90 * dpiMul), eRect->bottom - eRect->top + (int)(2 * dpiMul), (int)(80 * dpiMul), (int)(20 * dpiMul), SWP_NOZORDER);
				}
			}			
			dialogMutex.Leave();
		}
		break;
	case WM_TIMER:
		effect = (AEffect*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
		if (effect)
		{
			portNum = *(int*)effect->user;
			if (sameThread)
			{
				int sampleFrames = (int)(sample_rate * timerPeriodMS * 0.001);
				float** samples = (float**)malloc(sizeof(float**) * effect->numOutputs);
				for (int channel = 0; channel < effect->numOutputs; channel++) {
					samples[channel] = (float*)malloc(sizeof(float*) * sampleFrames);
				}

				effect->processReplacing(effect, samples, samples, sampleFrames); //ADLPlug's editor is frozen without this.
				sample_pos += sampleFrames;

				for (int channel = 0; channel < effect->numOutputs; channel++) {
					free(samples[channel]);
				}

				free(samples);
			}

			effect->dispatcher(effect, effEditIdle, 0, 0, 0, 0);
		}
		break;
	case WM_COMMAND:
		effect = (AEffect*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
		portNum = *(int*)effect->user;

		if (HIWORD(wParam) == BN_CLICKED && lParam == (LPARAM)checkBoxWnd[portNum])
		{
			if (SendMessage(checkBoxWnd[portNum], BM_GETCHECK, 0, 0) == BST_CHECKED)
				SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
			else
				SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);

			return 0;
		}
		else if (HIWORD(wParam) == BN_CLICKED && lParam == (LPARAM)buttonWnd[portNum])
		{
			if (!settings_save(effect)) MessageBox(hwnd, L"Cannot save plugin settings!\r\nMaybe you do not have permission to write plugin's folder \r\nor the plugin has nothing to save.", L"VST MIDI Driver", MB_OK | MB_ICONERROR);
			else MessageBox(hwnd, L"Plugin settings have been saved successfully!", L"VST MIDI Driver", MB_OK | MB_ICONINFORMATION);

			return 0;
		}
		break;
	case WM_CLOSE:		
		if ((wParam == 66 && lParam == 66) || sameThread) //because of JUCE framework editors prevent real closing except when driver quits. 
		{		
			effect = (AEffect*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
			portNum = *(int*)effect->user;

			RECT rect;
			GetWindowRect(hwnd, &rect);
			setEditorPosition(portNum, rect.left, rect.top);

			KillTimer(hwnd, 1);
			if (effect)
			{
				effect->dispatcher(effect, effEditClose, 0, 0, 0, 0);
				editorHandle[portNum] = 0;
			}

			if (hFont[portNum]) DeleteObject(hFont[portNum]);
			EndDialog(hwnd, IDOK);
		}
		else 
		{
			ShowWindow(hwnd, SW_HIDE);
			return 0;
		}

		break;
	case WM_DESTROY:
		if (!sameThread) PostQuitMessage(0);
		break;
	}

	return 0;
}

struct audioMasterData
{
	VstIntPtr effect_number;
};

static VstIntPtr VSTCALLBACK audioMaster(AEffect* effect, VstInt32 opcode, VstInt32 index, VstIntPtr value, void* ptr, float opt)
{
	audioMasterData* data = NULL;
	if (effect) data = (audioMasterData*)effect->user;

	switch (opcode)
	{
	case audioMasterVersion:
		return 2400;

	case audioMasterCurrentId:
		if (data) return data->effect_number;
		break;

	case audioMasterGetTime: //Some VST requires this. E.g. Genny throws AV without this.  
		vstTimeInfo.flags = kVstTransportPlaying | kVstNanosValid | kVstTempoValid | kVstTimeSigValid;
		vstTimeInfo.samplePos = sample_pos;
		vstTimeInfo.sampleRate = sample_rate;
		vstTimeInfo.timeSigNumerator = 4;
		vstTimeInfo.timeSigDenominator = 4;
		vstTimeInfo.tempo = 120;
		vstTimeInfo.nanoSeconds = (double)timeGetTime() * 1000000.0L;

		return (VstIntPtr)&vstTimeInfo;

	case audioMasterGetSampleRate:
		return sample_rate;

	case audioMasterGetBlockSize:
		return BUFFER_SIZE;
	
	case audioMasterCanDo:
		if (_stricmp((char*)ptr, "supplyidle") == 0
			|| _stricmp((char*)ptr, "sendvstevents") == 0
			|| _stricmp((char*)ptr, "sendvstmidievent") == 0
			|| _stricmp((char*)ptr, "sendvsttimeinfo") == 0
			|| _stricmp((char*)ptr, "sizewindow") == 0
			|| _stricmp((char*)ptr, "startstopprocess") == 0)
		{
			return 1;
		}
		else
		{
			return -1;
		}
		break;

	case audioMasterGetVendorString:
		strcpy((char*)ptr, "VST MIDI Driver"); //full 64 char freezes some plugins. E.g. Kondor. 
		//strncpy((char *)ptr, "YAMAHA", 64);
		break;

	case audioMasterGetProductString:
		strcpy((char*)ptr, "VST Host Bridge"); //full 64 char freezes some plugins. E.g. Kondor. 
		//strncpy((char *)ptr, "SOL/SQ01", 64);
		break;

	case audioMasterGetVendorVersion:
		return VERSION_MAJOR * 1000 + VERSION_MINOR * 100 + VERSION_PATCH * 10 + VERSION_BUILD;

	case audioMasterGetLanguage:
		return kVstLangEnglish;

	case audioMasterVendorSpecific:
		/* Steinberg HACK */
		if (ptr)
		{
			uint32_t* blah = (uint32_t*)(((char*)ptr) - 4);
			if (*blah == 0x0737bb68)
			{
				*blah ^= 0x5CC8F349;
				blah[2] = 0x19E;
				return 0x1E7;
			}
		}
		break;

	case audioMasterGetDirectory:
		return (VstIntPtr)dll_dir;

		/* More crap */
	case DECLARE_VST_DEPRECATED(audioMasterNeedIdle):
		need_idle = true;
		return 0;
	}

	return 0;
}

#ifdef MINIDUMP
/*****************************************************************************/
/* LoadMiniDump : tries to load minidump functionality                       */
/*****************************************************************************/

#include <dbghelp.h>
#include "vsthost.h"
typedef BOOL(__stdcall* PMiniDumpWriteDump)(IN HANDLE hProcess, IN DWORD ProcessId, IN HANDLE hFile, IN MINIDUMP_TYPE DumpType, IN CONST PMINIDUMP_EXCEPTION_INFORMATION ExceptionParam, OPTIONAL IN CONST PMINIDUMP_USER_STREAM_INFORMATION UserStreamParam, OPTIONAL IN CONST PMINIDUMP_CALLBACK_INFORMATION CallbackParam OPTIONAL);
static PMiniDumpWriteDump pMiniDumpWriteDump = NULL;
static TCHAR szExeName[_MAX_PATH] = _T("");
bool LoadMiniDump()
{
	static HMODULE hmodDbgHelp = NULL;

	if (!pMiniDumpWriteDump && !hmodDbgHelp)
	{
		TCHAR szBuf[_MAX_PATH];
		szBuf[0] = _T('\0');
		::GetEnvironmentVariable(_T("ProgramFiles"), szBuf, MAX_PATH);
		UINT omode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
		TCHAR* p = szBuf + lstrlen(szBuf);
		if (*szBuf && !hmodDbgHelp)
		{
#ifdef _M_IX86
			lstrcpy(p, _T("\\Debugging Tools for Windows (x86)\\dbghelp.dll"));
#elif defined(_M_X64)
			lstrcpy(p, _T("\\Debugging Tools for Windows (x64)\\dbghelp.dll"));
#endif
			if (!(hmodDbgHelp = LoadLibrary(szBuf))) *p = '\0';
		}
		if (*szBuf && !hmodDbgHelp)
		{
			lstrcpy(p, _T("\\Debugging Tools for Windows\\dbghelp.dll"));
			if (!(hmodDbgHelp = LoadLibrary(szBuf))) *p = '\0';
		}
#if defined _M_X64
		if (*szBuf && !hmodDbgHelp)       /* still not found?                  */
		{                               /* try 64-bit version                */
			lstrcpy(p, _T("\\Debugging Tools for Windows 64-Bit\\dbghelp.dll"));
			if (!(hmodDbgHelp = LoadLibrary(szBuf))) *p = '\0';
		}
#endif
		if (!hmodDbgHelp)      // if not already loaded, try to load a default-one
			hmodDbgHelp = LoadLibrary(_T("dbghelp.dll"));
		SetErrorMode(omode);

		if (!hmodDbgHelp)                 /* if STILL not loaded,              */
			return false;                   /* return with error                 */

		pMiniDumpWriteDump = (PMiniDumpWriteDump)GetProcAddress(hmodDbgHelp, "MiniDumpWriteDump");
	}
	if (!pMiniDumpWriteDump)
		return false;

	if (!*szExeName)
	{
		TCHAR szPath[_MAX_PATH];
		TCHAR* lbsl = NULL, * ldot = NULL;
		lstrcpy(szExeName, _T("vsthost"));
		GetModuleFileName(NULL, szPath, _MAX_PATH);
		for (TCHAR* p = szPath; *p; p++)
			if (*p == _T('\\'))
				lbsl = p;
			else if (*p == _T('.'))
				ldot = p;
		if (lbsl && ldot && ldot > lbsl + 1)
		{
			TCHAR* s, * t;
			for (s = lbsl + 1, t = szExeName; s != ldot; s++)
				*t++ = *s;
			*t = _T('\0');
		}
	}

	return true;
}

/*****************************************************************************/
/* MiniDump : writes out a minidump                                          */
/*****************************************************************************/

void MiniDump(EXCEPTION_POINTERS* ExceptionInfo)
{
	// all variables static so they're preallocated - stack might be damaged!
	// doesn't catch everything, but should get most.
	static TCHAR szPath[_MAX_PATH];
	if (!pMiniDumpWriteDump)
	{
		Log(_T("MiniDump impossible!\n"));
		return;
	}

	static DWORD dwExeLen;
	dwExeLen = (DWORD)lstrlen(szExeName);
	if (!(dwExeLen = GetTempPath(_countof(szPath) - dwExeLen - 21, szPath)) ||
		(dwExeLen > _countof(szPath) - dwExeLen - 22))
	{
		Log(_T("TempPath allocation error 1\n"));
		return;
	}
	static SYSTEMTIME st;
	GetLocalTime(&st);
	lstrcpy(szPath + dwExeLen, szExeName);
	dwExeLen += (DWORD)lstrlen(szExeName);
	wsprintf(szPath + dwExeLen, _T(".%04d%02d%02d-%02d%02d%02d.mdmp"),
		szExeName,
		st.wYear, st.wMonth, st.wDay,
		st.wHour, st.wMinute, st.wSecond);
	static HANDLE hFile;                    /* create the minidump file          */
	Log(_T("Dumping to %s ...\n"), szPath);
	hFile = ::CreateFile(szPath,
		GENERIC_WRITE, FILE_SHARE_WRITE,
		NULL, CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile != INVALID_HANDLE_VALUE)
	{
		static _MINIDUMP_EXCEPTION_INFORMATION ExInfo = { 0 };
		ExInfo.ThreadId = ::GetCurrentThreadId();
		ExInfo.ExceptionPointers = ExceptionInfo;
		pMiniDumpWriteDump(GetCurrentProcess(),
			GetCurrentProcessId(),
			hFile,
			MiniDumpNormal,
			&ExInfo,
			NULL, NULL);
		::CloseHandle(hFile);
		Log(_T("Dumped to %s\n"), szPath);
	}
	else
		Log(_T("Could not dump to %s!\n"), szPath);
}
#endif

LONG __stdcall myExceptFilterProc(LPEXCEPTION_POINTERS param)
{
	if (IsDebuggerPresent())
	{
		return UnhandledExceptionFilter(param);
	}
	else
	{
#ifdef MINIDUMP
		Log(_T("Dumping! pMiniDumpWriteDump=%p\nszExeName=%s\n"), pMiniDumpWriteDump, szExeName);
		MiniDump(param);
#endif
		TerminateProcess(GetCurrentProcess(), 0);
		return 0;// never reached
	}
}

#pragma comment(lib, "Winmm")

static unsigned __stdcall EditorThread(void* threadparam)
{
	MyDLGTEMPLATE vstiEditor;
	AEffect* pEffect = (AEffect*)threadparam;
	vstiEditor.style = WS_POPUPWINDOW | WS_DLGFRAME | WS_MINIMIZEBOX | WS_SYSMENU | WS_CAPTION | DS_MODALFRAME | DS_CENTER;
	
	if (highDpiMode && SetThreadDpiAwarenessContext) SetThreadDpiAwarenessContext(highDpiMode);

	CoInitialize(NULL);
	DialogBoxIndirectParam(0, &vstiEditor, 0, (DLGPROC)EditorProc, (LPARAM)pEffect);
	CoUninitialize();
	
	_endthreadex(0);
	return 0;
}

void showVstEditor(uint32_t portnum)
{
	if (editorHandle[portnum])
	{
		if (!IsWindowVisible(editorHandle[portnum]) || IsIconic(editorHandle[portnum]))
			ShowWindow(editorHandle[portnum], SW_SHOWNORMAL);
		else
			ShowWindow(editorHandle[portnum], SW_HIDE); //JUCE framework editors have problems when really closed and re-opened.

	}
	else if (pEffect[portnum]->flags & VstAEffectFlags::effFlagsHasEditor)
	{
		threadHandle[portnum] = (HANDLE)_beginthreadex(NULL, 16384, &EditorThread, pEffect[portnum], 0, NULL);
	}
}

LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	static NOTIFYICONDATA nIconData = { 0 };
	static HMENU trayMenu = NULL;

	switch (msg)
	{
	case WM_CREATE:
		{
			trayMenu = CreatePopupMenu();
			AppendMenu(trayMenu, MF_STRING | MF_ENABLED, 0, L"Port A VST Dialog");
			AppendMenu(trayMenu, MF_STRING | MF_ENABLED, 1, L"Port B VST Dialog");
			AppendMenu(trayMenu, MF_SEPARATOR, 0, L"");
			AppendMenu(trayMenu, MF_STRING | MF_ENABLED, 11, L"Send GM Reset");
			AppendMenu(trayMenu, MF_STRING | MF_ENABLED, 12, L"Send GS Reset");
			AppendMenu(trayMenu, MF_STRING | MF_ENABLED, 13, L"Send XG Reset");
			AppendMenu(trayMenu, MF_STRING | MF_ENABLED, 14, L"Send GM2 Reset");
			AppendMenu(trayMenu, MF_STRING | MF_ENABLED, 15, L"All Notes/CC Off");
			AppendMenu(trayMenu, MF_SEPARATOR, 0, L"");
			AppendMenu(trayMenu, MF_STRING | MF_ENABLED, 21, L"Info...");


			nIconData.cbSize = sizeof(NOTIFYICONDATA);
			nIconData.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
			nIconData.hWnd = hwnd;
			nIconData.uID = WM_ICONMSG;
			nIconData.uCallbackMessage = WM_ICONMSG;
			nIconData.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(32512));
			lstrcpyn(nIconData.szTip, trayTip, _countof(nIconData.szTip));
			Shell_NotifyIcon(NIM_ADD, &nIconData);

			InitSimpleResetEvents();

			return 0;
		}
		break;

	case WM_DESTROY:
		{
			Shell_NotifyIcon(NIM_DELETE, &nIconData);
			DestroyMenu(trayMenu);
			PostQuitMessage(0);
			return 0;
		}
		break;

	case WM_ICONMSG:
		{
			if (wParam == WM_ICONMSG && (lParam == WM_RBUTTONDOWN || lParam == WM_LBUTTONDOWN))
			{
				POINT cursorPoint;
				bool hasEditor = pEffect[0]->flags & VstAEffectFlags::effFlagsHasEditor;
				GetCursorPos(&cursorPoint);
				CheckMenuItem(trayMenu, 0, MF_BYCOMMAND | (editorHandle[0] != 0 && IsWindowVisible(editorHandle[0]) ? MF_CHECKED : MF_UNCHECKED));
				CheckMenuItem(trayMenu, 1, MF_BYCOMMAND | (editorHandle[1] != 0 && IsWindowVisible(editorHandle[1]) ? MF_CHECKED : MF_UNCHECKED));
				EnableMenuItem(trayMenu, 0, MF_BYCOMMAND | (isPortActive[0] && hasEditor ? MF_ENABLED : MF_GRAYED));
				EnableMenuItem(trayMenu, 1, MF_BYCOMMAND | (isPortActive[1] && hasEditor ? MF_ENABLED : MF_GRAYED));

				SetForegroundWindow(trayWndHandle);
				TrackPopupMenu(trayMenu, TPM_RIGHTBUTTON | (GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN), cursorPoint.x, cursorPoint.y, 0, hwnd, NULL);
				PostMessage(trayWndHandle, WM_NULL, 0, 0);
			}

			return 0;
		}
		break;
	case WM_HELP:
		{
			wchar_t tmpPath[MAX_PATH] = { 0 };			

			GetWindowsDirectory(tmpPath, MAX_PATH);
			lstrcat(tmpPath, L"\\SysWOW64\\vstmididrv\\Help\\Readme.html");
			if(GetFileAttributes(tmpPath) == INVALID_FILE_ATTRIBUTES)
			{
				GetWindowsDirectory(tmpPath, MAX_PATH);
				lstrcat(tmpPath, L"\\System32\\vstmididrv\\Help\\Readme.html");
			}	
						
			ShellExecute(hwnd, NULL, tmpPath, NULL, NULL, SW_SHOWNORMAL);
		}
		break;
	case WM_COMMAND:
		{
			wchar_t tempBuff[MAX_PATH] = {0};
			wchar_t versionBuff[MAX_PATH] = L"MIDI client: ";
			MSGBOXPARAMS params = {0};

			switch (wParam)
			{
			case 0:
			case 1:
				showVstEditor((uint32_t)wParam);
				return 0;
			case 11:
				sendSysExEvent((char*)gmReset, sizeof(gmReset));
				return 0;
			case 12:
				sendSysExEvent((char*)gsReset, sizeof(gsReset));
				return 0;
			case 13:
				sendSysExEvent((char*)xgReset, sizeof(xgReset));
				return 0;
			case 14:
				sendSysExEvent((char*)gm2Reset, sizeof(gm2Reset));
				return 0;
			case 15:
				sendSimpleResetEvents();
				return 0;
			case 21:
				if (aboutBoxResult == 255) return 0;

				lstrcat(versionBuff, midiClient);
				lstrcat(versionBuff, L" "); 
				lstrcat(versionBuff, clientBitnessStr);
				lstrcat(versionBuff, L"\r\nPlugin: ");
				mbstowcs(tempBuff, product_string, 64);
				lstrcat(versionBuff, tempBuff);
				lstrcat(versionBuff, bitnessStr);
				lstrcat(versionBuff, L"\r\nDriver mode: ");
				lstrcat(versionBuff, outputModeStr);
				
				lstrcat(versionBuff, L"\r\n \r\n");
				
				lstrcat(versionBuff, L"Synth driver ");				
				GetSystemDirectory(tempBuff, MAX_PATH);
				lstrcat(tempBuff, L"\\vstmididrv.dll");
				GetFileVersion(tempBuff, versionBuff);
				
				lstrcat(versionBuff, L"\r\nHost bridge ");
				GetFileVersion(NULL, versionBuff);
                
				params.cbSize = sizeof(params);
				params.dwStyle = MB_OK | MB_USERICON | MB_TOPMOST | MB_HELP;
				params.hInstance = GetModuleHandle(NULL);
				params.hwndOwner = hwnd;
				params.lpszCaption = L"VST MIDI Synth (Falcomod)";
				params.lpszText = versionBuff;
				params.lpszIcon = MAKEINTRESOURCE(32512);

				aboutBoxResult = 255;
				aboutBoxResult = MessageBoxIndirect(&params);				
				
				return 0;
			}
		}
		break;

	default:
		return DefWindowProc(hwnd, msg, wParam, lParam);
	}
	return 0;

}

static unsigned __stdcall TrayThread(void* threadparam)
{
	MSG message;
	WNDCLASS windowClass = { 0 };
	windowClass.hInstance = GetModuleHandle(NULL);
	windowClass.lpfnWndProc = TrayWndProc;
	windowClass.lpszClassName = L"VSTHostUtilWindow";
	
	if (SetThreadDpiAwarenessContext) SetThreadDpiAwarenessContext((HANDLE) -2); //System aware

	RegisterClass(&windowClass);
	trayWndHandle = CreateWindowEx(WS_EX_TOOLWINDOW, windowClass.lpszClassName, L"VSTTray", WS_POPUP, 0, 0, 0, 0, 0, 0, GetModuleHandle(NULL), NULL);

	//ShowWindow(trayWndHandle, SW_SHOWNORMAL);

	while (GetMessage(&message, 0, 0, 0)) {
		TranslateMessage(&message);
		DispatchMessage(&message);
	}

	_endthreadex(0);
	return 0;
}

int CALLBACK _tWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPTSTR lpCmdLine, int nCmdShow)
{
	int argc = 0;
	LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

	if (argv == NULL || argc != 6) return Error::InvalidCommandLineArguments;

	wchar_t* end_char = 0;
	unsigned in_sum = wcstoul(argv[2], &end_char, 16);
	if (end_char == argv[2] || *end_char) return Error::MalformedChecksum;

	unsigned test_sum = 0;
	end_char = argv[1];
	while (*end_char)
	{
		test_sum += (TCHAR)(*end_char++ * 820109);
	}

#ifdef NDEBUG
	if (test_sum != in_sum) return  Error::ChecksumMismatch;
#endif

	unsigned code = Response::NoError;

	if (IsWinNT4())
		lstrcpy(trayTip, L"VST Midi Synth - ");
	else
		lstrcpy(trayTip, L"VST Midi Synth \r\n");

	wcsncat(trayTip, argv[3], _countof(trayTip) - wcslen(trayTip));
	wcsncpy(midiClient, argv[3], _countof(midiClient));    
	wcsncpy(clientBitnessStr, argv[4], _countof(clientBitnessStr)); 
	
	wcsncpy(outputModeStr, wcscmp(argv[5], L"W") ? L"ASIO": L"WaveOut", _countof(outputModeStr));

	HMODULE hDll = NULL;
	main_func pMain = NULL;

	audioMasterData effectData[2] = { { 0 }, { 1 } };

	std::vector<uint8_t> blState;

	std::vector<uint8_t> chunk;
	std::vector<float> sample_buffer;
	unsigned int samples_buffered = 0;

	MainThreadId = GetCurrentThreadId();

	user32 = GetModuleHandle(L"user32.dll");
	if (user32)	LOADUSER32FUNCTION(SetThreadDpiAwarenessContext);

#if(_WIN32_WINNT < 0x0500) 
	if (user32)	LOADUSER32FUNCTION(AllowSetForegroundWindow);
#endif	

	null_file = CreateFile(_T("NUL"), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);

	pipe_in = GetStdHandle(STD_INPUT_HANDLE);
	pipe_out = GetStdHandle(STD_OUTPUT_HANDLE);

	SetStdHandle(STD_INPUT_HANDLE, null_file);
	SetStdHandle(STD_OUTPUT_HANDLE, null_file);

	{
		INITCOMMONCONTROLSEX icc = { 0 };
		icc.dwSize = sizeof(icc);
		icc.dwICC = ICC_WIN95_CLASSES;
		if (!InitCommonControlsEx(&icc)) InitCommonControls();
		//InitCommonControlsEx can fail on Win 2000/XP depending on service packs. It's rude to exit in case of failing since this is not essentiall at all.
	}

	if (FAILED(CoInitialize(NULL))) return Error::ComInitializationFailed;

#ifndef _DEBUG
#ifdef MINIDUMP
	LoadMiniDump();
	Log(_T("pMiniDumpWriteDump=%p\nszExeName=%s\n"), pMiniDumpWriteDump, szExeName);
#endif
	SetUnhandledExceptionFilter(myExceptFilterProc);
#endif

	size_t dll_name_len = wcslen(argv[1]);
	dll_dir = (char*)malloc(dll_name_len + 1);
	wcstombs(dll_dir, argv[1], dll_name_len);
	dll_dir[dll_name_len] = '\0';
	char* slash = strrchr(dll_dir, '\\');
	*slash = '\0';

	hDll = LoadLibraryW(argv[1]);
	if (!hDll)
	{
		code = Response::CannotLoadVstiDll;
		goto exit;
	}

	pMain = (main_func)GetProcAddress(hDll, "VSTPluginMain");
	if (!pMain)
		pMain = (main_func)GetProcAddress(hDll, "main");
	if (!pMain)
	{
		code = Response::CannotGetProcAddress;
		goto exit;
	}

#if 0
	MessageBox(GetDesktopWindow(), argv[1], _T("HUUUURRRRRR"), 0);
#endif

	pEffect[0] = pMain(&audioMaster);

	if (!pEffect[0] || pEffect[0]->magic != kEffectMagic)
	{
		code = Response::NotAVsti;
		goto exit;
	}

	pEffect[0]->user = &effectData[0];

	pEffect[0]->dispatcher(pEffect[0], effOpen, 0, 0, 0, 0);

	if (pEffect[0]->dispatcher(pEffect[0], effGetPlugCategory, 0, 0, 0, 0) != kPlugCategSynth ||
		pEffect[0]->dispatcher(pEffect[0], effCanDo, 0, 0, "receiveVstMidiEvent", 0) < 1)
	{
		code = Response::VstiIsNotAMidiSynth;
		goto exit;
	}

	pEffect[1] = pMain(&audioMaster);
	if (!pEffect[1])
	{
		code = Response::NotAVsti;
		goto exit;
	}

	pEffect[1]->user = &effectData[1];
	pEffect[1]->dispatcher(pEffect[1], effOpen, 0, 0, 0, 0);

	{
		char name_string[256] = { 0 };
		char vendor_string[256] = { 0 };		
		uint32_t name_string_length;
		uint32_t vendor_string_length;
		uint32_t product_string_length;
		uint32_t vendor_version;
		uint32_t unique_id;

		pEffect[0]->dispatcher(pEffect[0], effGetEffectName, 0, 0, &name_string, 0);
		pEffect[0]->dispatcher(pEffect[0], effGetVendorString, 0, 0, &vendor_string, 0);
		pEffect[0]->dispatcher(pEffect[0], effGetProductString, 0, 0, &product_string, 0);
        		
		name_string_length = (uint32_t)strlen(name_string);
		vendor_string_length = (uint32_t)strlen(vendor_string);
		product_string_length = (uint32_t)strlen(product_string);
		vendor_version = (uint32_t)pEffect[0]->dispatcher(pEffect[0], effGetVendorVersion, 0, 0, 0, 0);
		unique_id = pEffect[0]->uniqueID;

		if (unique_id == (uint32_t)'scva') isSCVA = true;

		put_code(Response::NoError);
		put_code(name_string_length);
		put_code(vendor_string_length);
		put_code(product_string_length);
		put_code(vendor_version);
		put_code(unique_id);
		put_code(num_outputs_per_port);
		if (name_string_length) put_bytes(name_string, name_string_length);
		if (vendor_string_length) put_bytes(vendor_string, vendor_string_length);
		if (product_string_length) put_bytes(product_string, product_string_length);
	}

	float** float_list_in;
	float** float_list_out;
	float* float_null;
	float* float_out;

	dialogMutex.Init();
	
	for (;;)
	{
		uint32_t command = get_code();
		if (!command) break;

		Log(_T("Processing command %d\n"), (int)command);

		switch (command)
		{
		case Command::GetChunkData: // Get Chunk
			{
				getChunk(pEffect[0], chunk);

				put_code(Response::NoError);
				put_code((uint32_t)chunk.size());
#if	(defined(_MSC_VER) && (_MSC_VER < 1600))
				if (chunk.size()) put_bytes(&chunk.front(), (uint32_t)chunk.size());
#else
				if (chunk.size()) put_bytes(chunk.data(), (uint32_t)chunk.size());
#endif
			}
			break;

		case Command::SetChunkData: // Set Chunk
			{
				uint32_t size = get_code();
				chunk.resize(size);
#if (defined(_MSC_VER) && (_MSC_VER < 1600))
				if (size) get_bytes(&chunk.front(), size);
#else
				if (size) get_bytes(chunk.data(), size);
#endif

				setChunk(pEffect[0], chunk);
				setChunk(pEffect[1], chunk);

				put_code(Response::NoError);
			}
			break;

		case Command::HasEditor: // Has Editor
			{
				uint32_t has_editor = (pEffect[0]->flags & effFlagsHasEditor) ? 1 : 0;

				put_code(Response::NoError);
				put_code(has_editor);
			}
			break;

		case Command::DisplayEditorModal: // Display Editor Modal
			{
				if (pEffect[0]->flags & effFlagsHasEditor)
				{
					MyDLGTEMPLATE t;
					t.style = WS_POPUPWINDOW | WS_DLGFRAME | DS_MODALFRAME | DS_CENTER;
					t.dwExtendedStyle = WS_EX_TOPMOST;

					if (highDpiMode && SetThreadDpiAwarenessContext) SetThreadDpiAwarenessContext(highDpiMode);

					DialogBoxIndirectParam(0, &t, GetDesktopWindow(), (DLGPROC)EditorProc, (LPARAM)(pEffect[0]));
					//getChunk(pEffect[0], chunk);
					//setChunk(pEffect[1], chunk);

#if(_WIN32_WINNT < 0x0500) 
					if (AllowSetForegroundWindow) AllowSetForegroundWindow(ASFW_ANY); //allows drivercfg to get back focus 
#else	
					::AllowSetForegroundWindow(ASFW_ANY); //allows drivercfg to get back focus 
#endif					
					
				}

				put_code(Response::NoError);
			}
			break;

		case Command::DisplayEditorModalThreaded: // Display Editor Modal in separate thread
			{
				uint32_t port_num = get_code();

				showVstEditor(port_num);

				put_code(Response::NoError);
			}
			break;

		case Command::InitSysTray:
			{
				_beginthreadex(NULL, 16384, &TrayThread, pEffect, 0, NULL);

				put_code(Response::NoError);

			}
			break;

		case Command::SetSampleRate: // Set Sample Rate
			{
				uint32_t size = get_code();
				if (size != sizeof(sample_rate))
				{
					code = Response::CannotSetSampleRate;
					goto exit;
				}

				sample_rate = get_code();

				put_code(Response::NoError);
			}
			break;

		case Command::SetHighDpiMode: // Set DPI awareness mode for plugin's editor
			{
				highDpiMode = (HANDLE)(int)get_code();
				put_code(Response::NoError);
			}
			break;

		case Command::SetSinglePort32ChMode:
			{
				isSinglePort32Ch = true;
				put_code(Response::NoError);
			}
			break;

		case Command::Reset: // Reset 
			{				
				if (isSCVA) sendSysExEvent((char*)gsReset, sizeof(gsReset));

				uint32_t port_num = get_code();

				if (isSinglePort32Ch)
				{
					if (editorHandle[0] != 0) ShowWindow(editorHandle[0], SW_HIDE);
					if (editorHandle[1] != 0) ShowWindow(editorHandle[1], SW_HIDE);
					isPortActive[0] = false;
					isPortActive[1] = false;

					if (pEffect[0])
					{
						if (blState.size()) pEffect[0]->dispatcher(pEffect[0], effStopProcess, 0, 0, 0, 0);						
					}
					if (pEffect[1])
					{
						if (blState.size()) pEffect[1]->dispatcher(pEffect[1], effStopProcess, 0, 0, 0, 0);						
					}							
				}
				else
				{
					if (editorHandle[port_num] != 0) ShowWindow(editorHandle[port_num], SW_HIDE);
					isPortActive[port_num] = false;

					if (pEffect[port_num])
					{
						if (blState.size()) pEffect[port_num]->dispatcher(pEffect[port_num], effStopProcess, 0, 0, 0, 0);						
					}									

				}
				
				blState.resize(0);
				freeChain();	

				put_code(Response::NoError);
			}
			break;

		case Command::SendMidiEvent: // Send MIDI Event
			{
				myVstEvent* ev = (myVstEvent*)calloc(sizeof(myVstEvent), 1);
				if (evTail) evTail->next = ev;
				evTail = ev;
				if (!evChain) evChain = ev;

				uint32_t b = get_code();

				uint32_t port = ((b & 0x7F000000) >> 24) & 1;
				isPortActive[port] = true;

				ev->port = port;
				//if (ev->port > 1) ev->port = 1;
				ev->ev.midiEvent.type = kVstMidiType;
				ev->ev.midiEvent.byteSize = sizeof(ev->ev.midiEvent);
				memcpy(&ev->ev.midiEvent.midiData, &b, 3);

				put_code(Response::NoError);
			}
			break;

		case Command::SendMidiSysExEvent: // Send System Exclusive Event
			{
				myVstEvent* ev = (myVstEvent*)calloc(sizeof(myVstEvent), 1);
				if (evTail) evTail->next = ev;
				evTail = ev;
				if (!evChain) evChain = ev;

				uint32_t size = get_code();

				uint32_t port = (size >> 24) & 1;
				isPortActive[port] = true;

				size &= 0xFFFFFF;

				ev->port = port;
				//if (ev->port > 1) ev->port = 1;
				ev->ev.sysexEvent.type = kVstSysExType;
				ev->ev.sysexEvent.byteSize = sizeof(ev->ev.sysexEvent);
				ev->ev.sysexEvent.dumpBytes = size;
				ev->ev.sysexEvent.sysexDump = (char*)malloc(size);

				get_bytes(ev->ev.sysexEvent.sysexDump, size);

				put_code(Response::NoError);
			}
			break;

		case Command::RenderAudioSamples: // Render Samples
			{
				if (!blState.size())
				{
					pEffect[0]->dispatcher(pEffect[0], effSetSampleRate, 0, 0, 0, float(sample_rate));
					pEffect[0]->dispatcher(pEffect[0], effSetBlockSize, 0, BUFFER_SIZE, 0, 0);
					pEffect[0]->dispatcher(pEffect[0], effMainsChanged, 0, 1, 0, 0);
					pEffect[0]->dispatcher(pEffect[0], effStartProcess, 0, 0, 0, 0);

					pEffect[1]->dispatcher(pEffect[1], effSetSampleRate, 0, 0, 0, float(sample_rate));
					pEffect[1]->dispatcher(pEffect[1], effSetBlockSize, 0, BUFFER_SIZE, 0, 0);
					pEffect[1]->dispatcher(pEffect[1], effMainsChanged, 0, 1, 0, 0);
					pEffect[1]->dispatcher(pEffect[1], effStartProcess, 0, 0, 0, 0);

					size_t buffer_size = sizeof(float*) * (pEffect[0]->numInputs + pEffect[0]->numOutputs * 2); // float lists
					buffer_size += sizeof(float) * BUFFER_SIZE;                                // null input
					buffer_size += sizeof(float) * BUFFER_SIZE * pEffect[0]->numOutputs * 2;          // outputs

					blState.resize(buffer_size);

#if (defined(_MSC_VER) && (_MSC_VER < 1600))
					float_list_in = (float**)(blState.size() ? &blState.front() : NULL);
#else
					float_list_in = (float**)blState.data();
#endif
					float_list_out = float_list_in + pEffect[0]->numInputs;
					float_null = (float*)(float_list_out + pEffect[0]->numOutputs * 2);
					float_out = float_null + BUFFER_SIZE;

					for (VstInt32 i = 0; i < pEffect[0]->numInputs; ++i)      float_list_in[i] = float_null;
					for (VstInt32 i = 0; i < pEffect[0]->numOutputs * 2; ++i) float_list_out[i] = float_out + BUFFER_SIZE * i;

					memset(float_null, 0, sizeof(float) * BUFFER_SIZE);

					sample_buffer.resize((4096 + BUFFER_SIZE) * num_outputs_per_port);
				}

				if (need_idle)
				{
					pEffect[0]->dispatcher(pEffect[0], DECLARE_VST_DEPRECATED(effIdle), 0, 0, 0, 0);
					pEffect[1]->dispatcher(pEffect[1], DECLARE_VST_DEPRECATED(effIdle), 0, 0, 0, 0);

					if (!idle_started)
					{
						unsigned idle_run = BUFFER_SIZE * 200;

						while (idle_run)
						{
							unsigned count_to_do = min(idle_run, BUFFER_SIZE);
							unsigned num_outputs = min(pEffect[0]->numOutputs, 2);

							pEffect[0]->processReplacing(pEffect[0], float_list_in, float_list_out, count_to_do);
							pEffect[1]->processReplacing(pEffect[1], float_list_in, float_list_out + num_outputs, count_to_do);

							pEffect[0]->dispatcher(pEffect[0], DECLARE_VST_DEPRECATED(effIdle), 0, 0, 0, 0);
							pEffect[1]->dispatcher(pEffect[1], DECLARE_VST_DEPRECATED(effIdle), 0, 0, 0, 0);


							idle_run -= count_to_do;
						}
					}
				}

				VstEvents* events[2] = { 0 };

				if (evChain)
				{
					unsigned event_count[2] = { 0 };
					myVstEvent* ev = evChain;
					while (ev)
					{
						event_count[ev->port]++;
						ev = ev->next;
					}

					if (event_count[0])
					{
						events[0] = (VstEvents*)malloc(sizeof(VstInt32) + sizeof(VstIntPtr) + sizeof(VstEvent*) * event_count[0]);

						events[0]->numEvents = event_count[0];
						events[0]->reserved = 0;

						ev = evChain;

						for (unsigned i = 0; ev; )
						{
							if (!ev->port) events[0]->events[i++] = (VstEvent*)&ev->ev;
							ev = ev->next;
						}

						pEffect[0]->dispatcher(pEffect[0], effProcessEvents, 0, 0, events[0], 0);
					}

					if (event_count[1])
					{
						events[1] = (VstEvents*)malloc(sizeof(VstInt32) + sizeof(VstIntPtr) + sizeof(VstEvent*) * event_count[1]);

						events[1]->numEvents = event_count[1];
						events[1]->reserved = 0;

						ev = evChain;

						for (unsigned i = 0; ev; )
						{
							if (ev->port == 1) events[1]->events[i++] = (VstEvent*)&ev->ev;
							ev = ev->next;
						}

						pEffect[1]->dispatcher(pEffect[1], effProcessEvents, 0, 0, events[1], 0);
					}

				}


				if (need_idle)
				{
					pEffect[0]->dispatcher(pEffect[0], DECLARE_VST_DEPRECATED(effIdle), 0, 0, 0, 0);
					pEffect[1]->dispatcher(pEffect[1], DECLARE_VST_DEPRECATED(effIdle), 0, 0, 0, 0);


					if (!idle_started)
					{
						if (events[0]) pEffect[0]->dispatcher(pEffect[0], effProcessEvents, 0, 0, events[0], 0);
						if (events[1]) pEffect[1]->dispatcher(pEffect[1], effProcessEvents, 0, 0, events[1], 0);

						idle_started = true;
					}
				}

				uint32_t count = get_code();
				sample_pos += count;

				put_code(Response::NoError);

				while (count)
				{
					unsigned count_to_do = min(count, BUFFER_SIZE);
					unsigned num_outputs = min(pEffect[0]->numOutputs, 2);

					pEffect[0]->processReplacing(pEffect[0], float_list_in, float_list_out, count_to_do);
					pEffect[1]->processReplacing(pEffect[1], float_list_in, float_list_out + num_outputs, count_to_do);

#if (defined(_MSC_VER) && (_MSC_VER < 1600))
					float* out = &sample_buffer.front() + samples_buffered * num_outputs_per_port;
#else
					float* out = sample_buffer.data() + samples_buffered * num_outputs_per_port;
#endif

					if (num_outputs == 2)
					{
						for (unsigned i = 0; i < count_to_do; ++i)
						{
							float sample = (float_out[i] + float_out[i + BUFFER_SIZE * num_outputs]);
							out[0] = sample;
							sample = (float_out[i + BUFFER_SIZE] + float_out[i + BUFFER_SIZE + BUFFER_SIZE * num_outputs]);
							out[1] = sample;
							out += 2;
						}
					}
					else
					{
						for (unsigned i = 0; i < count_to_do; ++i)
						{
							float sample = (float_out[i] + float_out[i + BUFFER_SIZE * num_outputs]);
							out[0] = sample;
							out[1] = sample;
							out += 2;
						}
					}

#if (defined(_MSC_VER) && (_MSC_VER < 1600))
					put_bytes(&sample_buffer.front(), count_to_do * sizeof(float) * num_outputs_per_port);
#else
					put_bytes(sample_buffer.data(), count_to_do * sizeof(float) * num_outputs_per_port);
#endif

					count -= count_to_do;
				}

				if (events[0]) free(events[0]);
				if (events[1]) free(events[1]);

				freeChain();
			}
			break;

		case Command::RenderAudioSamples4channel: // Render Samples for 4 channel mode
			{
				if (!blState.size())
				{
					pEffect[0]->dispatcher(pEffect[0], effSetSampleRate, 0, 0, 0, float(sample_rate));
					pEffect[0]->dispatcher(pEffect[0], effSetBlockSize, 0, BUFFER_SIZE, 0, 0);
					pEffect[0]->dispatcher(pEffect[0], effMainsChanged, 0, 1, 0, 0);
					pEffect[0]->dispatcher(pEffect[0], effStartProcess, 0, 0, 0, 0);

					pEffect[1]->dispatcher(pEffect[1], effSetSampleRate, 0, 0, 0, float(sample_rate));
					pEffect[1]->dispatcher(pEffect[1], effSetBlockSize, 0, BUFFER_SIZE, 0, 0);
					pEffect[1]->dispatcher(pEffect[1], effMainsChanged, 0, 1, 0, 0);
					pEffect[1]->dispatcher(pEffect[1], effStartProcess, 0, 0, 0, 0);

					size_t buffer_size = sizeof(float*) * (pEffect[0]->numInputs + pEffect[0]->numOutputs * 2 * 2); // float lists
					buffer_size += sizeof(float) * BUFFER_SIZE;                                // null input
					buffer_size += sizeof(float) * BUFFER_SIZE * pEffect[0]->numOutputs * 2 * 2;          // outputs

					blState.resize(buffer_size);

#if (defined(_MSC_VER) && (_MSC_VER < 1600))
					float_list_in = (float**)(blState.size() ? &blState.front() : NULL);
#else
					float_list_in = (float**)blState.data();
#endif
					float_list_out = float_list_in + pEffect[0]->numInputs;
					float_null = (float*)(float_list_out + pEffect[0]->numOutputs * 2 * 2);
					float_out = float_null + BUFFER_SIZE;

					for (VstInt32 i = 0; i < pEffect[0]->numInputs; ++i)      float_list_in[i] = float_null;
					for (VstInt32 i = 0; i < pEffect[0]->numOutputs * 2 * 2; ++i) float_list_out[i] = float_out + BUFFER_SIZE * i;

					memset(float_null, 0, sizeof(float) * BUFFER_SIZE);

					sample_buffer.resize((4096 + BUFFER_SIZE) * num_outputs_per_port * 2);
				}

				if (need_idle)
				{
					pEffect[0]->dispatcher(pEffect[0], DECLARE_VST_DEPRECATED(effIdle), 0, 0, 0, 0);
					pEffect[1]->dispatcher(pEffect[1], DECLARE_VST_DEPRECATED(effIdle), 0, 0, 0, 0);

					if (!idle_started)
					{
						unsigned idle_run = BUFFER_SIZE * 200;

						while (idle_run)
						{
							unsigned count_to_do = min(idle_run, BUFFER_SIZE);
							unsigned num_outputs = min(pEffect[0]->numOutputs, 2);

							pEffect[0]->processReplacing(pEffect[0], float_list_in, float_list_out, count_to_do);
							pEffect[1]->processReplacing(pEffect[1], float_list_in, float_list_out + num_outputs, count_to_do);

							pEffect[0]->dispatcher(pEffect[0], DECLARE_VST_DEPRECATED(effIdle), 0, 0, 0, 0);
							pEffect[1]->dispatcher(pEffect[1], DECLARE_VST_DEPRECATED(effIdle), 0, 0, 0, 0);


							idle_run -= count_to_do;
						}
					}
				}

				VstEvents* events[2] = { 0 };

				if (evChain)
				{
					unsigned event_count[2] = { 0 };
					myVstEvent* ev = evChain;
					while (ev)
					{
						event_count[ev->port]++;
						ev = ev->next;
					}

					if (event_count[0])
					{
						events[0] = (VstEvents*)malloc(sizeof(VstInt32) + sizeof(VstIntPtr) + sizeof(VstEvent*) * event_count[0]);

						events[0]->numEvents = event_count[0];
						events[0]->reserved = 0;

						ev = evChain;

						for (unsigned i = 0; ev; )
						{
							if (!ev->port) events[0]->events[i++] = (VstEvent*)&ev->ev;
							ev = ev->next;
						}

						pEffect[0]->dispatcher(pEffect[0], effProcessEvents, 0, 0, events[0], 0);
					}

					if (event_count[1])
					{
						events[1] = (VstEvents*)malloc(sizeof(VstInt32) + sizeof(VstIntPtr) + sizeof(VstEvent*) * event_count[1]);

						events[1]->numEvents = event_count[1];
						events[1]->reserved = 0;

						ev = evChain;

						for (unsigned i = 0; ev; )
						{
							if (ev->port == 1) events[1]->events[i++] = (VstEvent*)&ev->ev;
							ev = ev->next;
						}

						pEffect[1]->dispatcher(pEffect[1], effProcessEvents, 0, 0, events[1], 0);
					}

				}


				if (need_idle)
				{
					pEffect[0]->dispatcher(pEffect[0], DECLARE_VST_DEPRECATED(effIdle), 0, 0, 0, 0);
					pEffect[1]->dispatcher(pEffect[1], DECLARE_VST_DEPRECATED(effIdle), 0, 0, 0, 0);


					if (!idle_started)
					{
						if (events[0]) pEffect[0]->dispatcher(pEffect[0], effProcessEvents, 0, 0, events[0], 0);
						if (events[1]) pEffect[1]->dispatcher(pEffect[1], effProcessEvents, 0, 0, events[1], 0);

						idle_started = true;
					}
				}

				uint32_t count = get_code();
				sample_pos += count;

				put_code(Response::NoError);

				while (count)
				{
					unsigned count_to_do = min(count, BUFFER_SIZE);
					unsigned num_outputs = min(pEffect[0]->numOutputs, 2);

					pEffect[0]->processReplacing(pEffect[0], float_list_in, float_list_out, count_to_do);
					pEffect[1]->processReplacing(pEffect[1], float_list_in, float_list_out + num_outputs, count_to_do);

#if (defined(_MSC_VER) && (_MSC_VER < 1600))
					float* out = &sample_buffer.front() + samples_buffered * num_outputs_per_port;
#else
					float* out = sample_buffer.data() + samples_buffered * num_outputs_per_port;
#endif

					if (num_outputs == 2)
					{
						for (unsigned i = 0; i < count_to_do; ++i)
						{
							float sample = float_out[i];
							out[0] = sample;
							sample = float_out[i + BUFFER_SIZE];
							out[1] = sample;

							sample = float_out[i + BUFFER_SIZE * num_outputs];
							out[2] = sample;
							sample = float_out[i + BUFFER_SIZE + BUFFER_SIZE * num_outputs];
							out[3] = sample;

							out += 4;
						}
					}
					else
					{
						for (unsigned i = 0; i < count_to_do; ++i)
						{
							float sample = float_out[i];
							out[0] = sample;
							out[1] = sample;

							sample = float_out[i + BUFFER_SIZE * num_outputs];
							out[2] = sample;
							out[3] = sample;

							out += 4;
						}
					}

#if (defined(_MSC_VER) && (_MSC_VER < 1600))
					put_bytes(&sample_buffer.front(), count_to_do * sizeof(float) * num_outputs_per_port * 2);
#else
					put_bytes(sample_buffer.data(), count_to_do * sizeof(float) * num_outputs_per_port * 2);
#endif

					count -= count_to_do;
				}

				if (events[0]) free(events[0]);
				if (events[1]) free(events[1]);

				freeChain();
			}
			break;

		default:
			code = Response::CommandUnknown;;
			goto exit;
			break;
		}

		Log(_T("Command %d done\n"), (int)command);

	}

exit:
	if (editorHandle[0] != NULL) SendMessageTimeout(editorHandle[0], WM_CLOSE, 66, 66, SMTO_ABORTIFHUNG | SMTO_NORMAL, 1000, NULL);
	if (editorHandle[1] != NULL) SendMessageTimeout(editorHandle[1], WM_CLOSE, 66, 66, SMTO_ABORTIFHUNG | SMTO_NORMAL, 1000, NULL);
	if (trayWndHandle != NULL) SendMessageTimeout(trayWndHandle, WM_CLOSE, 0, 0, SMTO_ABORTIFHUNG | SMTO_NORMAL, 1000, NULL);

	if (threadHandle[0] != NULL) CloseHandle(threadHandle[0]);
	if (threadHandle[1] != NULL) CloseHandle(threadHandle[1]);	
	
	if (pEffect[1])
	{
		if (blState.size()) pEffect[1]->dispatcher(pEffect[1], effStopProcess, 0, 0, 0, 0);
		pEffect[1]->dispatcher(pEffect[1], effClose, 0, 0, 0, 0);
	}
	if (pEffect[0])
	{
		if (blState.size()) pEffect[0]->dispatcher(pEffect[0], effStopProcess, 0, 0, 0, 0);
		pEffect[0]->dispatcher(pEffect[0], effClose, 0, 0, 0, 0);
	}
	freeChain();
	if (hDll) FreeLibrary(hDll);
	CoUninitialize();
	if (dll_dir) free(dll_dir);
	if (argv) LocalFree(argv);

	put_code(code);

	if (null_file)
	{
		CloseHandle(null_file);

		SetStdHandle(STD_INPUT_HANDLE, pipe_in);
		SetStdHandle(STD_OUTPUT_HANDLE, pipe_out);
	}

	dialogMutex.Close();
	Log(_T("Exit with code %d\n"), code);

	return code;
}
