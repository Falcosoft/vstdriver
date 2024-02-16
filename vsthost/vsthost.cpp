// vsthost.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include <process.h>
#include "../version.h"

// #define LOG_EXCHANGE
// #define LOG
// #define MINIDUMP

#define WIN32DEF(f) (WINAPI *f)
#define LOADUSER32FUNCTION(f) *((void**)&f)=GetProcAddress(user32,#f)

#if(_WIN32_WINNT < 0x0500) 
	#define SM_CXVIRTUALSCREEN  78
	#define SM_CYVIRTUALSCREEN  79
	#define ASFW_ANY    ((DWORD)-1)
#endif 

#define WM_ICONMSG WM_APP + 1
#define MAX_PLUGINS 10
#define RESET_MENU_OFFSET 10
#define PORT_MENU_OFFSET 100
#define RESET_MIDIMSG_COUNT 64 //4 messages for 16 channels. These can be useful if a synth does not support any SysEx reset messages. 
#define BUFFER_SIZE 4800  //matches better for typical 48/96/192 kHz
#define MAX_INPUTEVENT_COUNT 1024  //it's impossible to get more events in 1 cycle since midiStream also uses this size.
#define PORT_ALL 0xFFFF

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
		ResetRequest = 255,
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

static struct ResetVstEvent
{
	VstInt32 numEvents;
	VstIntPtr reserved;
	VstEvent* events[RESET_MIDIMSG_COUNT];

} resetVstEvents = { 0 };

static struct RenderVstEvent
{
	VstInt32 numEvents;
	VstIntPtr reserved;
	VstEvent* events[MAX_INPUTEVENT_COUNT];

}renderEvents[2] = { 0 };

static struct InputEventList
{
	unsigned int position;
	unsigned int portMessageCount[2];
	struct InputEvent
	{
		unsigned port;
		union
		{
			VstMidiEvent midiEvent;
			VstMidiSysexEvent sysexEvent;
		} ev;

	} events[MAX_INPUTEVENT_COUNT];

}inputEventList = { 0 };

static const unsigned char gmReset[] = { 0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7 };
static const unsigned char gsReset[] = { 0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7 };
static const unsigned char xgReset[] = { 0xF0, 0x43, 0x10, 0x4C, 0x00, 0x00, 0x7E, 0x00, 0xF7 };
static const unsigned char gm2Reset[] = { 0xF0, 0x7E, 0x7F, 0x09, 0x03, 0xF7 };

static VstMidiEvent resetMidiEvents[RESET_MIDIMSG_COUNT] = { 0 };


#ifdef WIN64
	static TCHAR bitnessStr[8] =_T(" 64-bit"); 
#else
	static TCHAR bitnessStr[8] = _T(" 32-bit");
#endif

static NOTIFYICONDATA nIconData = { 0 };
static HMENU trayMenu = NULL;
static HMENU pluginMenu = NULL;

static TCHAR clientBitnessStr[8] = { 0 };
static TCHAR outputModeStr[8] = { 0 };

static volatile int lastUsedSysEx = RESET_MENU_OFFSET + 5;
static volatile bool doOwnReset = false;
static bool driverResetRequested = false;
static bool need_idle = false;
static bool idle_started = false;

static HINSTANCE user32 = NULL;
static HINSTANCE kernel32 = NULL;
static BOOL WIN32DEF(DynGetModuleHandleEx)(DWORD dwFlags, LPCTSTR lpModuleName, HMODULE* phModule) = NULL;

#if(_WIN32_WINNT < 0x0500) 
	static BOOL WIN32DEF(AllowSetForegroundWindow)(DWORD dwProcessId) = NULL;
#endif

static HANDLE WIN32DEF(SetThreadDpiAwarenessContext)(HANDLE dpiContext) = NULL;
static HANDLE highDpiMode = NULL;

static HWND editorHandle[2] = { NULL };
static HANDLE threadHandle[2] = { NULL };
static bool isPortActive[2] = { false };
static volatile DWORD destPort = PORT_ALL;

static bool isSinglePort32Ch = false;

static uint32_t num_outputs_per_port = 2;
static uint32_t sample_rate = 48000;
static uint32_t sample_pos = 0;

static DWORD MainThreadId;
static char* dll_dir = NULL;

static char product_string[256] = { 0 };
static TCHAR midiClient[64] = { 0 };
static TCHAR trayTip[MAX_PATH] = { 0 };

static HANDLE null_file = NULL;
static HANDLE pipe_in = NULL;
static HANDLE pipe_out = NULL;

static AEffect* pEffect[2] = { NULL };

static VstTimeInfo vstTimeInfo = { 0 };

static volatile HWND trayWndHandle = NULL;
static volatile int aboutBoxResult = 0;

static Win32Lock dialogLock(true);

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
		_tcscat_s(logfile, _T("\\vsthost.log"));
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

#ifdef MINIDUMP
/*****************************************************************************/
/* LoadMiniDump : tries to load minidump functionality                       */
/*****************************************************************************/

#include <dbghelp.h>
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
			_tcscpy(p, _T("\\Debugging Tools for Windows (x86)\\dbghelp.dll"));
#elif defined(_M_X64)
			_tcscpy(p, _T("\\Debugging Tools for Windows (x64)\\dbghelp.dll"));
#endif
			if (!(hmodDbgHelp = LoadLibrary(szBuf))) *p = '\0';
		}
		if (*szBuf && !hmodDbgHelp)
		{
			_tcscpy(p, _T("\\Debugging Tools for Windows\\dbghelp.dll"));
			if (!(hmodDbgHelp = LoadLibrary(szBuf))) *p = '\0';
		}
#if defined _M_X64
		if (*szBuf && !hmodDbgHelp)       /* still not found?                  */
		{                               /* try 64-bit version                */
			_tcscpy(p, _T("\\Debugging Tools for Windows 64-Bit\\dbghelp.dll"));
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
		_tcscpy_s(szExeName, _T("vsthost"));
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
	_tcscpy(szPath + dwExeLen, szExeName);
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

static void InvalidParamHandler(const wchar_t* expression, const wchar_t* function, const wchar_t* file, unsigned int line, uintptr_t pReserved)
{
	if (MessageBox(0, _T("An unexpected invalid parameter error occured.\r\nDo you want to try to continue?"), _T("VST Host Bridge Error"), MB_YESNO | MB_ICONERROR | MB_SYSTEMMODAL) == IDNO)
		TerminateProcess(GetCurrentProcess(), 1);
}

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
		if (!trayMenu) return 1; // Do not disturb users with error messages caused by faulty plugins when driver is closing anyway

		static TCHAR buffer[MAX_PATH] = { 0 };

		if (DynGetModuleHandleEx)
		{
			static TCHAR titleBuffer[MAX_PATH / 2] = { 0 };
			HMODULE hFaultyModule;
			DynGetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCTSTR)param->ExceptionRecord->ExceptionAddress, &hFaultyModule);
			GetModuleFileName(hFaultyModule, buffer, MAX_PATH);
			GetFileTitle(buffer, titleBuffer, MAX_PATH / 2);
			wsprintf(buffer, _T("An unexpected error 0x%X occured in %ls.\r\nVST host bridge now exits."), param->ExceptionRecord->ExceptionCode, titleBuffer);
		}
		else
		{
			wsprintf(buffer, _T("An unexpected error 0x%X occured.\r\nVST host bridge now exits."), param->ExceptionRecord->ExceptionCode);
		}

		MessageBox(0, buffer, _T("VST Midi Driver"), MB_OK | MB_SYSTEMMODAL | MB_ICONERROR);
		Shell_NotifyIcon(NIM_DELETE, &nIconData);
		DestroyMenu(trayMenu);
		TerminateProcess(GetCurrentProcess(), 1);
		return 1;
	}
}


#pragma comment(lib,"Version.lib") 
static TCHAR* GetFileVersion(TCHAR* filePath, TCHAR* result, unsigned int buffSize)
{
	DWORD               dwSize = 0;
	BYTE* pVersionInfo = NULL;
	VS_FIXEDFILEINFO* pFileInfo = NULL;
	UINT                pLenFileInfo = 0;
	TCHAR tmpBuff[MAX_PATH];

	if(!filePath)
		GetModuleFileName(NULL, tmpBuff, MAX_PATH);
	else
		_tcscpy_s(tmpBuff, filePath);

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

	_tcscat_s(result, buffSize, _T("version: "));
	_ultot_s((pFileInfo->dwFileVersionMS >> 16) & 0xffff, tmpBuff, MAX_PATH, 10);
	_tcscat_s(result, buffSize, tmpBuff);
	_tcscat_s(result, buffSize, _T("."));
	_ultot_s((pFileInfo->dwFileVersionMS) & 0xffff, tmpBuff, MAX_PATH, 10);
	_tcscat_s(result, buffSize, tmpBuff);
	_tcscat_s(result, buffSize, _T("."));
	_ultot_s((pFileInfo->dwFileVersionLS >> 16) & 0xffff, tmpBuff, MAX_PATH, 10);
	_tcscat_s(result, buffSize, tmpBuff);
	//_tcscat_s(result, buffSize, _T("."));
	//_tcscat_s(result, buffSize, _ultot((pFileInfo->dwFileVersionLS) & 0xffff, tmpBuff, 10));

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

void resetInputEvents()
{
	for (unsigned int i = 0; i < inputEventList.position; i++)
		if (inputEventList.events[i].ev.sysexEvent.type == kVstSysExType) free(inputEventList.events[i].ev.sysexEvent.sysexDump);
	
	inputEventList.position = 0;
	inputEventList.portMessageCount[0] = 0;
	inputEventList.portMessageCount[1] = 0;
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
	TCHAR vst_path[MAX_PATH] = { 0 };
	ULONG size;
	
	lResult = RegOpenKeyEx(HKEY_CURRENT_USER, _T("Software\\VSTi Driver"), 0, KEY_READ, &hKey);
	if (lResult == ERROR_SUCCESS)
	{
		TCHAR szValueName[20] = _T("plugin");
		DWORD selIndex = 0;
		size = sizeof(selIndex);
		lResult = RegQueryValueEx(hKey, _T("SelectedPlugin"), NULL, &dwType, (LPBYTE)&selIndex, &size);
		if (lResult == ERROR_SUCCESS && selIndex)
		{
			TCHAR szPostfix[12] = { 0 };
			_tcscat_s(szValueName, _itot(selIndex, szPostfix, 10));
		}

		lResult = RegQueryValueEx(hKey, szValueName, NULL, &dwType, NULL, &size);
		if (lResult == ERROR_SUCCESS && dwType == REG_SZ)
		{
			RegQueryValueEx(hKey, szValueName, NULL, &dwType, (LPBYTE)vst_path, &size);
			TCHAR* chrP = _tcsrchr(vst_path, '.'); // removes extension
			if (chrP) chrP[0] = 0;
			_tcscat_s(vst_path, _T(".set"));

			HANDLE fileHandle = CreateFile(vst_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

			if (fileHandle != INVALID_HANDLE_VALUE)
			{
				std::vector<uint8_t> chunk;
				if (pEffect) getChunk(pEffect, chunk);
#if (defined(_MSC_VER) && (_MSC_VER < 1600))

				if (chunk.size() >= (2 * sizeof(uint32_t) + sizeof(bool))) retResult = WriteFile(fileHandle, &chunk.front(), (DWORD)chunk.size(), &size, NULL);
#else

				if (chunk.size() >= (2 * sizeof(uint32_t) + sizeof(bool))) retResult = WriteFile(fileHandle, chunk.data(), (DWORD)chunk.size(), &size, NULL);
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

	long result = RegOpenKeyEx(HKEY_CURRENT_USER, _T("Software\\VSTi Driver"), 0, KEY_READ, &hKey);
	if (result == NO_ERROR)
	{
		DWORD size = 4;
		if (!port)
		{
			RegQueryValueEx(hKey, _T("PortAWinPosX"), NULL, NULL, (LPBYTE)&x, &size);
			RegQueryValueEx(hKey, _T("PortAWinPosY"), NULL, NULL, (LPBYTE)&y, &size);
		}
		else
		{
			RegQueryValueEx(hKey, _T("PortBWinPosX"), NULL, NULL, (LPBYTE)&x, &size);
			RegQueryValueEx(hKey, _T("PortBWinPosY"), NULL, NULL, (LPBYTE)&y, &size);

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

	long result = RegOpenKeyEx(HKEY_CURRENT_USER, _T("Software\\VSTi Driver"), 0, KEY_READ | KEY_WRITE, &hKey);
	if (result == NO_ERROR)
	{
		DWORD size = 4;
		if (!port)
		{
			RegSetValueEx(hKey, _T("PortAWinPosX"), NULL, REG_DWORD, (LPBYTE)&x, size);
			RegSetValueEx(hKey, _T("PortAWinPosY"), NULL, REG_DWORD, (LPBYTE)&y, size);
		}
		else
		{
			RegSetValueEx(hKey, _T("PortBWinPosX"), NULL, REG_DWORD, (LPBYTE)&x, size);
			RegSetValueEx(hKey, _T("PortBWinPosY"), NULL, REG_DWORD, (LPBYTE)&y, size);

		}

		RegCloseKey(hKey);
	}
}

static void InitSimpleResetEvents()
{
	resetVstEvents.numEvents = RESET_MIDIMSG_COUNT;
	resetVstEvents.reserved = 0;
	const int ChannelCount = 16;

	for (int i = 0; i < ChannelCount; i++)
	{
		DWORD msg, index;		

		msg = (0xB0 | i) | (0x40 << 8); //Sustain off		
		index = i * 4;
		memcpy(&resetMidiEvents[index].midiData, &msg, 3);
		resetMidiEvents[index].type = kVstMidiType;
		resetMidiEvents[index].byteSize = sizeof(VstMidiEvent);
		resetMidiEvents[index].flags = VstMidiEventFlags::kVstMidiEventIsRealtime;		
		resetVstEvents.events[index] = (VstEvent*)&resetMidiEvents[index];

		msg = (0xB0 | i) | (0x7B << 8); //All Notes off
		index = i * 4 + 1;
		memcpy(&resetMidiEvents[index].midiData, &msg, 3);
		resetMidiEvents[index].type = kVstMidiType;
		resetMidiEvents[index].byteSize = sizeof(VstMidiEvent);
		resetMidiEvents[index].flags = VstMidiEventFlags::kVstMidiEventIsRealtime;		
		resetVstEvents.events[index] = (VstEvent*)&resetMidiEvents[index];

		msg = (0xB0 | i) | (0x79 << 8);  //All Controllers off
		index = i * 4 + 2;
		memcpy(&resetMidiEvents[index].midiData, &msg, 3);
		resetMidiEvents[index].type = kVstMidiType;
		resetMidiEvents[index].byteSize = sizeof(VstMidiEvent);
		resetMidiEvents[index].flags = VstMidiEventFlags::kVstMidiEventIsRealtime;		
		resetVstEvents.events[index] = (VstEvent*)&resetMidiEvents[index];

		msg = (0xB0 | i) | (0x78 << 8);  //All Sounds off
		index = i * 4 + 3;
		memcpy(&resetMidiEvents[index].midiData, &msg, 3);
		resetMidiEvents[index].type = kVstMidiType;
		resetMidiEvents[index].byteSize = sizeof(VstMidiEvent);
		resetMidiEvents[index].flags = VstMidiEventFlags::kVstMidiEventIsRealtime;		
		resetVstEvents.events[index] = (VstEvent*)&resetMidiEvents[index];
	}
}

static void InsertSysExEvent(char* sysExBytes, int size, DWORD portNum)
{
	InputEventList::InputEvent* ev = &inputEventList.events[inputEventList.position];
	inputEventList.position++;
	inputEventList.position &= (MAX_INPUTEVENT_COUNT - 1);

	memset(ev, 0, sizeof(InputEventList::InputEvent));	

	uint32_t port = (int)portNum;
	isPortActive[port] = true;
	inputEventList.portMessageCount[port]++;	

	ev->port = port;	
	ev->ev.sysexEvent.type = kVstSysExType;
	ev->ev.sysexEvent.byteSize = sizeof(ev->ev.sysexEvent);
	ev->ev.sysexEvent.dumpBytes = size;
	ev->ev.sysexEvent.sysexDump = (char*)malloc(size);

	if(ev->ev.sysexEvent.sysexDump) 
		memcpy(ev->ev.sysexEvent.sysexDump, sysExBytes, size);
}

static void InsertOwnReset(DWORD portNum)
{
	switch (lastUsedSysEx)
	{
	case RESET_MENU_OFFSET + 1:
		InsertSysExEvent((char*)gmReset, sizeof(gmReset), portNum);
		break;
	case RESET_MENU_OFFSET + 2:
		InsertSysExEvent((char*)gsReset, sizeof(gsReset), portNum);
		break;
	case RESET_MENU_OFFSET + 3:
		InsertSysExEvent((char*)xgReset, sizeof(xgReset), portNum);
		break;
	case RESET_MENU_OFFSET + 4:
		InsertSysExEvent((char*)gm2Reset, sizeof(gm2Reset), portNum);
		break;	
	}	
}

static void sendSysExEvent(char* sysExBytes, int size, DWORD portNum)
{
	static VstMidiSysexEvent syxEvent = { 0 };
	static VstEvents vstEvents = { 0 };

	syxEvent.byteSize = sizeof(syxEvent);
	syxEvent.dumpBytes = size;
	syxEvent.sysexDump = sysExBytes;
	syxEvent.type = kVstSysExType;

	vstEvents.events[0] = (VstEvent*)&syxEvent;
	vstEvents.numEvents = 1;

	if (!portNum || portNum == PORT_ALL)
		pEffect[0]->dispatcher(pEffect[0], effProcessEvents, 0, 0, &vstEvents, 0);
	
	if (portNum)
		pEffect[1]->dispatcher(pEffect[1], effProcessEvents, 0, 0, &vstEvents, 0);
}

static void sendSimpleResetEvents(DWORD portNum)
{	
	if (!portNum || portNum == PORT_ALL)
		pEffect[0]->dispatcher(pEffect[0], effProcessEvents, 0, 0, &resetVstEvents, 0);
	
	if (portNum)
		pEffect[1]->dispatcher(pEffect[1], effProcessEvents, 0, 0, &resetVstEvents, 0);
}

static void SendOwnReset(DWORD portNum)
{	
	switch (lastUsedSysEx)
	{	
	case RESET_MENU_OFFSET + 1:
		sendSysExEvent((char*)gmReset, sizeof(gmReset), portNum);
		break;
	case RESET_MENU_OFFSET + 2:
		sendSysExEvent((char*)gsReset, sizeof(gsReset), portNum);
		break;
	case RESET_MENU_OFFSET + 3:
		sendSysExEvent((char*)xgReset, sizeof(xgReset), portNum);
		break;
	case RESET_MENU_OFFSET + 4:
		sendSysExEvent((char*)gm2Reset, sizeof(gm2Reset), portNum);
		break;	
	case RESET_MENU_OFFSET + 5:
		sendSimpleResetEvents(portNum);
		break;
	}

	doOwnReset = false;
}

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
				ScopeLock<Win32Lock> scopeLock(&dialogLock);
				
				/*
				falco:
				DPI adjustment for NT4/W2K/XP that do not use DPI virtualization but can use large font (125% - 120 DPI) and actually all other DPI settings.
				96 DPI is 100%. There is no separate DPI for X/Y on Windows.
				Since we have no DPI Aware manifest on Vista+ DPI virtualization is used.
				It's better than setting DPI awareness to true since allmost all VST 2.0 Editors know nothing about DPI scaling. So we let modern Windows handle this.
				*/
				HDC screen = GetDC(0);
				dpiMul = (float)(GetDeviceCaps(screen, LOGPIXELSY)) / 96.0f;
				ReleaseDC(0, screen);

				portNum = *(int*)effect->user;

				isPortActive[portNum] = true;
				editorHandle[portNum] = hwnd;

				SetWindowLongPtr(hwnd, GWLP_USERDATA, lParam);

				TCHAR wText[18] = _T("VST Editor port ");
				TCHAR intCnst[] = { 'A' + portNum };
				_tcsncat_s(wText, intCnst, 1);

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

						checkBoxWnd[portNum] = CreateWindowEx(NULL, _T("BUTTON"), _T("Always on Top"), WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX, (int)(5 * dpiMul), eRect->bottom - eRect->top + (int)(3 * dpiMul), (int)(100 * dpiMul), (int)(20 * dpiMul), hwnd, NULL, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
						SendMessage(checkBoxWnd[portNum], BM_SETCHECK, BST_CHECKED, 0);

						buttonWnd[portNum] = CreateWindowEx(NULL, _T("BUTTON"), _T("Save Settings"), WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, width - (int)(90 * dpiMul), eRect->bottom - eRect->top + (int)(2 * dpiMul), (int)(80 * dpiMul), (int)(20 * dpiMul), hwnd, NULL, ((LPCREATESTRUCT)lParam)->hInstance, NULL);

						GetObject(GetStockObject(DEFAULT_GUI_FONT), sizeof(LOGFONT), &lf);
						hFont[portNum] = CreateFontIndirect(&lf);
						SendMessage(checkBoxWnd[portNum], WM_SETFONT, (WPARAM)hFont[portNum], TRUE);
						SendMessage(buttonWnd[portNum], WM_SETFONT, (WPARAM)hFont[portNum], TRUE);

						SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
					}
					else
					{
						effect->dispatcher(effect, effSetSampleRate, 0, 0, 0, float(sample_rate));
						effect->dispatcher(effect, effSetBlockSize, 0, (VstIntPtr)(sample_rate * timerPeriodMS * 0.001), 0, 0);
						sample_pos = 0;
					}					
				}
			}
			SetForegroundWindow(hwnd);
		}
		break;
	case WM_SIZE: //Fixes SC-VA display bug after parts section opened/closed
		effect = (AEffect*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
		portNum = *(int*)effect->user;

		if (effect && wParam == SIZE_RESTORED)
		{
			ScopeLock<Win32Lock> scopeLock(&dialogLock);
			
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
			if (!settings_save(effect)) MessageBox(hwnd, _T("Cannot save plugin settings!\r\nMaybe you do not have permission to write plugin's folder \r\nor the plugin has nothing to save."), _T("VST MIDI Driver"), MB_OK | MB_ICONERROR);
			else MessageBox(hwnd, _T("Plugin settings have been saved successfully!"), _T("VST MIDI Driver"), MB_OK | MB_ICONINFORMATION);

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
		vstTimeInfo.flags = kVstTransportPlaying | kVstNanosValid | kVstTempoValid | kVstTimeSigValid | kVstPpqPosValid;
		vstTimeInfo.samplePos = sample_pos;
		vstTimeInfo.sampleRate = sample_rate;
		vstTimeInfo.timeSigNumerator = 4;
		vstTimeInfo.timeSigDenominator = 4;
		vstTimeInfo.tempo = 120;
		vstTimeInfo.nanoSeconds = (double)timeGetTime() * 1000000.0;
		vstTimeInfo.ppqPos = (double)sample_pos / sample_rate * 2.0;

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
		strcpy_s((char*)ptr, 16, "VST MIDI Driver"); //full 64 char freezes some plugins. E.g. Kondor. 		
		break;

	case audioMasterGetProductString:
		strcpy_s((char*)ptr, 16, "VST Host Bridge"); //full 64 char freezes some plugins. E.g. Kondor.		
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

struct MyDLGTEMPLATE : DLGTEMPLATE
{
	WORD ext[3];
	MyDLGTEMPLATE()
	{
		memset(this, 0, sizeof(*this));
	};
};

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

bool getPluginMenuItem(int itemIndex, TCHAR* result, unsigned int buffSize)
{
	long lResult;
	DWORD dwType = REG_SZ;
	HKEY hKey;
	TCHAR vst_path[MAX_PATH] = { 0 };
	TCHAR vst_title[MAX_PATH - 4] = { 0 };
	ULONG size;

	lResult = RegOpenKeyEx(HKEY_CURRENT_USER, _T("Software\\VSTi Driver"), 0, KEY_READ, &hKey);
	if (lResult == ERROR_SUCCESS)
	{
		TCHAR szValueName[8] = _T("plugin");
		TCHAR szPluginNum[2] = { 0 };
		
		if (itemIndex)
		{			
			_tcscat_s(szValueName, _itot(itemIndex, szPluginNum, 10));
		}

		lResult = RegQueryValueEx(hKey, szValueName, NULL, &dwType, NULL, &size);
		if (lResult == ERROR_SUCCESS && dwType == REG_SZ && size > 2)
		{
			lResult =  RegQueryValueEx(hKey, szValueName, NULL, &dwType, (LPBYTE)vst_path, &size);
			if (lResult == ERROR_SUCCESS) 
			{
				RegCloseKey(hKey);
				
				GetFileTitle(vst_path, vst_title, MAX_PATH - 4);				
				_tcscpy_s(result, buffSize, _itot(itemIndex, szPluginNum, 10));
				_tcscat_s(result, buffSize, _T(". "));
				_tcscat_s(result, buffSize, vst_title);
				return true;
			}

		}
		RegCloseKey(hKey);
	}

	return false;

}

int getSelectedPluginIndex() 
{
	HKEY hKey;
	int retRes = 0;

	long result = RegOpenKeyEx(HKEY_CURRENT_USER, _T("Software\\VSTi Driver"), 0, KEY_READ | KEY_WRITE, &hKey);
	if (result == NO_ERROR)
	{
		DWORD size = sizeof(DWORD);
		RegQueryValueEx(hKey, _T("SelectedPlugin"), NULL, NULL, (LPBYTE)&retRes, &size);

		RegCloseKey(hKey);
	}

	return retRes;
}

void setSelectedPluginIndex(int index) 
{
	HKEY hKey;

	long result = RegOpenKeyEx(HKEY_CURRENT_USER, _T("Software\\VSTi Driver"), 0, KEY_READ | KEY_WRITE, &hKey);
	if (result == NO_ERROR)
	{
		DWORD size = sizeof(DWORD);
		RegSetValueEx(hKey, _T("SelectedPlugin"), NULL, REG_DWORD, (LPBYTE)&index, size);

		RegCloseKey(hKey);
	}

	driverResetRequested = true;
}

int getSelectedSysExIndex()
{
	HKEY hKey;
	int retRes = RESET_MENU_OFFSET + 5;

	long result = RegOpenKeyEx(HKEY_CURRENT_USER, _T("Software\\VSTi Driver"), 0, KEY_READ | KEY_WRITE, &hKey);
	if (result == NO_ERROR)
	{
		DWORD size = sizeof(DWORD);
		RegQueryValueEx(hKey, _T("SelectedSysEx"), NULL, NULL, (LPBYTE)&retRes, &size);

		RegCloseKey(hKey);
	}

	return retRes;
}

void setSelectedSysExIndex(int index)
{
	HKEY hKey;

	long result = RegOpenKeyEx(HKEY_CURRENT_USER, _T("Software\\VSTi Driver"), 0, KEY_READ | KEY_WRITE, &hKey);
	if (result == NO_ERROR)
	{
		DWORD size = sizeof(DWORD);
		RegSetValueEx(hKey, _T("SelectedSysEx"), NULL, REG_DWORD, (LPBYTE)&index, size);

		RegCloseKey(hKey);
	}

}


LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{		
	TCHAR tmpPath[MAX_PATH] = { 0 };
	const int STILLRUNNING = 255;

	switch (msg)
	{
	case WM_CREATE:
		{			
			pluginMenu = CreatePopupMenu();

			for (int i = 0; i < MAX_PLUGINS; i++)
				if (getPluginMenuItem(i, tmpPath, MAX_PATH)) {
					if (getSelectedPluginIndex() == i)
						AppendMenu(pluginMenu, MF_STRING | MF_ENABLED | MF_CHECKED, i, tmpPath);
					else
						AppendMenu(pluginMenu, MF_STRING | MF_ENABLED, i, tmpPath);
				}
			
			trayMenu = CreatePopupMenu();
			AppendMenu(trayMenu, MF_STRING | MF_ENABLED, PORT_MENU_OFFSET, _T("Port A VST Dialog"));
			AppendMenu(trayMenu, MF_STRING | MF_ENABLED, PORT_MENU_OFFSET + 1, _T("Port B VST Dialog"));
			AppendMenu(trayMenu, MF_SEPARATOR, 0, _T(""));
			AppendMenu(trayMenu, MF_POPUP, (UINT)pluginMenu, _T("Switch Plugin"));
			AppendMenu(trayMenu, MF_SEPARATOR, 0, _T(""));
			AppendMenu(trayMenu, MF_STRING | MF_ENABLED, RESET_MENU_OFFSET + 1, _T("Send GM Reset"));
			AppendMenu(trayMenu, MF_STRING | MF_ENABLED, RESET_MENU_OFFSET + 2, _T("Send GS Reset"));
			AppendMenu(trayMenu, MF_STRING | MF_ENABLED, RESET_MENU_OFFSET + 3, _T("Send XG Reset"));
			AppendMenu(trayMenu, MF_STRING | MF_ENABLED, RESET_MENU_OFFSET + 4, _T("Send GM2 Reset"));
			AppendMenu(trayMenu, MF_STRING | MF_ENABLED, RESET_MENU_OFFSET + 5, _T("All Notes/CC Off"));
			AppendMenu(trayMenu, MF_SEPARATOR, 0, _T(""));
			AppendMenu(trayMenu, MF_STRING | MF_ENABLED, 2 * RESET_MENU_OFFSET + 1, _T("Info..."));


			nIconData.cbSize = sizeof(NOTIFYICONDATA);
			nIconData.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
			nIconData.hWnd = hwnd;
			nIconData.uID = WM_ICONMSG;
			nIconData.uCallbackMessage = WM_ICONMSG;
			nIconData.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(32512));
			_tcsncpy_s(nIconData.szTip, trayTip, _countof(nIconData.szTip));
			Shell_NotifyIcon(NIM_ADD, &nIconData);				
			
			return 0;
		}
		break;

	case WM_DESTROY:
		{
			setSelectedSysExIndex(lastUsedSysEx);

			Shell_NotifyIcon(NIM_DELETE, &nIconData);
			DestroyMenu(trayMenu);
			trayMenu = NULL;
			trayWndHandle = NULL;
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
				CheckMenuItem(trayMenu, PORT_MENU_OFFSET, editorHandle[0] != 0 && IsWindowVisible(editorHandle[0]) ? MF_CHECKED : MF_UNCHECKED);
				CheckMenuItem(trayMenu, PORT_MENU_OFFSET + 1, editorHandle[1] != 0 && IsWindowVisible(editorHandle[1]) ? MF_CHECKED : MF_UNCHECKED);
				EnableMenuItem(trayMenu, PORT_MENU_OFFSET,  isPortActive[0] && hasEditor ? MF_ENABLED : MF_GRAYED);
				EnableMenuItem(trayMenu, PORT_MENU_OFFSET + 1, isPortActive[1] && hasEditor ? MF_ENABLED : MF_GRAYED);
				EnableMenuItem(trayMenu, 3, isPortActive[0] || isPortActive[1] ?  MF_BYPOSITION | MF_ENABLED : MF_BYPOSITION | MF_GRAYED);

				CheckMenuItem(trayMenu, RESET_MENU_OFFSET + 1, lastUsedSysEx == RESET_MENU_OFFSET + 1 ? MF_CHECKED : MF_UNCHECKED);
				CheckMenuItem(trayMenu, RESET_MENU_OFFSET + 2, lastUsedSysEx == RESET_MENU_OFFSET + 2 ? MF_CHECKED : MF_UNCHECKED);
				CheckMenuItem(trayMenu, RESET_MENU_OFFSET + 3, lastUsedSysEx == RESET_MENU_OFFSET + 3 ? MF_CHECKED : MF_UNCHECKED);
				CheckMenuItem(trayMenu, RESET_MENU_OFFSET + 4, lastUsedSysEx == RESET_MENU_OFFSET + 4 ? MF_CHECKED : MF_UNCHECKED);
				CheckMenuItem(trayMenu, RESET_MENU_OFFSET + 5, lastUsedSysEx == RESET_MENU_OFFSET + 5 ? MF_CHECKED : MF_UNCHECKED);

				SetForegroundWindow(hwnd);
				TrackPopupMenu(trayMenu, TPM_RIGHTBUTTON | (GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN), cursorPoint.x, cursorPoint.y, 0, hwnd, NULL);
				PostMessage(hwnd, WM_NULL, 0, 0);
			}

			return 0;
		}
		break;
	case WM_HELP:
		{
			TCHAR tmpPath[MAX_PATH] = { 0 };			

			GetWindowsDirectory(tmpPath, MAX_PATH);
			_tcscat_s(tmpPath, _T("\\SysWOW64\\vstmididrv\\Help\\Readme.html"));
			if(GetFileAttributes(tmpPath) == INVALID_FILE_ATTRIBUTES)
			{
				GetWindowsDirectory(tmpPath, MAX_PATH);
				_tcscat_s(tmpPath, _T("\\System32\\vstmididrv\\Help\\Readme.html"));
			}	
						
			ShellExecute(hwnd, NULL, tmpPath, NULL, NULL, SW_SHOWNORMAL);
		}
		break;
	case WM_COMMAND:
		{
			TCHAR tempBuff[MAX_PATH] = { 0 };
			TCHAR versionBuff[MAX_PATH] = _T("MIDI client: ");
			MSGBOXPARAMS params = {0};

			if (wParam >= 0 && wParam < MAX_PLUGINS)
			{
				setSelectedPluginIndex((int)wParam);
				return 0;
			}

			switch (wParam)
			{			
			case PORT_MENU_OFFSET:
			case PORT_MENU_OFFSET + 1:
				showVstEditor((uint32_t)wParam - PORT_MENU_OFFSET);
				return 0;
			
			case RESET_MENU_OFFSET + 1:			
			case RESET_MENU_OFFSET + 2:				
			case RESET_MENU_OFFSET + 3:				
			case RESET_MENU_OFFSET + 4:				
			case RESET_MENU_OFFSET + 5:
				lastUsedSysEx = (int)wParam;				
				destPort = PORT_ALL;							
				doOwnReset = true;				
				return 0;

			case 2 * RESET_MENU_OFFSET + 1:
				if (aboutBoxResult == STILLRUNNING) return 0;

				_tcscat_s(versionBuff, midiClient);
				_tcscat_s(versionBuff, _T(" "));
				_tcscat_s(versionBuff, clientBitnessStr);
				_tcscat_s(versionBuff, _T("\r\nPlugin: "));
#ifdef UNICODE  
				MultiByteToWideChar(CP_ACP, 0, product_string, 64, tempBuff, 64);
				_tcscat_s(versionBuff, tempBuff);
#else
				strncat_s(versionBuff, product_string, 64);
#endif			
				_tcscat_s(versionBuff, bitnessStr);
				_tcscat_s(versionBuff, _T("\r\nDriver mode: "));
				_tcscat_s(versionBuff, outputModeStr);
				
				_tcscat_s(versionBuff, _T("\r\n \r\n"));
				
				_tcscat_s(versionBuff, _T("Synth driver "));
				GetSystemDirectory(tempBuff, MAX_PATH);
				_tcscat_s(tempBuff, _T("\\vstmididrv.dll"));
				GetFileVersion(tempBuff, versionBuff, _countof(versionBuff));
				
				_tcscat_s(versionBuff, _T("\r\nHost bridge "));
				GetFileVersion(NULL, versionBuff, _countof(versionBuff));
                
				params.cbSize = sizeof(params);
				params.dwStyle = MB_OK | MB_USERICON | MB_TOPMOST | MB_HELP;
				params.hInstance = GetModuleHandle(NULL);
				params.hwndOwner = hwnd;
				params.lpszCaption = _T("VST MIDI Synth (Falcomod)");
				params.lpszText = versionBuff;
				params.lpszIcon = MAKEINTRESOURCE(32512);

				aboutBoxResult = STILLRUNNING;
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
	windowClass.lpszClassName = _T("VSTHostUtilWindow");
	
	if (SetThreadDpiAwarenessContext) SetThreadDpiAwarenessContext((HANDLE) -2); //System aware

	RegisterClass(&windowClass);
	trayWndHandle = CreateWindowEx(WS_EX_TOOLWINDOW, windowClass.lpszClassName, _T("VSTTray"), WS_POPUP, 0, 0, 0, 0, 0, 0, GetModuleHandle(NULL), NULL);

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
	_set_invalid_parameter_handler(InvalidParamHandler);
	int argc = __argc;

#ifdef UNICODE 
	//LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	LPTSTR* argv = __wargv;
#else
	LPTSTR* argv = __argv;
#endif		

	if (argv == NULL || argc != 6) return Error::InvalidCommandLineArguments;

	TCHAR* end_char = 0;
	unsigned in_sum = _tcstoul(argv[2], &end_char, 16);
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
		_tcscpy_s(trayTip, _T("VST Midi Synth - "));
	else
		_tcscpy_s(trayTip, _T("VST Midi Synth \r\n"));

	_tcsncat_s(trayTip, argv[3], _countof(trayTip) - _tcslen(trayTip));
	_tcsncpy_s(midiClient, argv[3], _countof(midiClient));    
	_tcsncpy_s(clientBitnessStr, argv[4], _countof(clientBitnessStr)); 
	
	_tcsncpy_s(outputModeStr, !_tcscmp(argv[5], _T("S")) ? _T("WASAPI") : !_tcscmp(argv[5], _T("A")) ? _T("ASIO") : _T("WaveOut"), _countof(outputModeStr));

	HMODULE hDll = NULL;
	main_func pMain = NULL;

	audioMasterData effectData[2] = { { 0 }, { 1 } };

	std::vector<uint8_t> blState;

	std::vector<uint8_t> chunk;
	std::vector<float> sample_buffer;	

	MainThreadId = GetCurrentThreadId();

	user32 = GetModuleHandle(_T("user32.dll"));
	if (user32)	LOADUSER32FUNCTION(SetThreadDpiAwarenessContext);

#if(_WIN32_WINNT < 0x0500) 
	if (user32)	LOADUSER32FUNCTION(AllowSetForegroundWindow);
#endif
	

	kernel32 = GetModuleHandle(_T("kernel32.dll"));
#ifdef UNICODE  
	if (kernel32) *((void**)&DynGetModuleHandleEx) = GetProcAddress(kernel32, "GetModuleHandleExW"); //unfortunately in case of newer VS versions GetModuleHandleExW is defined unconditionally in libloaderapi.h despite it is available only from WinXP...
#else
	if (kernel32) *((void**)&DynGetModuleHandleEx) = GetProcAddress(kernel32, "GetModuleHandleExA"); //unfortunately in case of newer VS versions GetModuleHandleExW is defined unconditionally in libloaderapi.h despite it is available only from WinXP...
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

	size_t dll_name_len = _tcslen(argv[1]);
	dll_dir = (char*)malloc(dll_name_len + 1);

#ifdef UNICODE  
	WideCharToMultiByte(CP_ACP, 0, argv[1], -1, dll_dir, (int)dll_name_len + 1, NULL, NULL); //does not depent on C locale (problematic on some VC++ verisons).
#else
	strncpy(dll_dir, argv[1], dll_name_len);
#endif
	
	dll_dir[dll_name_len] = '\0';
	char* slash = strrchr(dll_dir, '\\');
	*slash = '\0';

	hDll = LoadLibrary(argv[1]);
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

		if (!vendor_string_length) 
		{
			strcpy_s(vendor_string, "Unknown");
			vendor_string_length = (uint32_t)strlen(vendor_string);
		}

		if (!product_string_length)
		{
			
#ifdef UNICODE 
			TCHAR product_wstring[256] = { 0 };
			GetFileTitle(argv[1], product_wstring, 256);
			WideCharToMultiByte(CP_ACP, 0, (LPCWSTR)product_wstring, -1, (char*)product_string, 256, NULL, NULL);
#else
			GetFileTitle(argv[1], product_string, 256);
#endif
			
			product_string_length = (uint32_t)strlen(product_string);
		}

		if (!name_string_length)
		{
#ifdef UNICODE 
			TCHAR name_wstring[256] = { 0 };
			GetFileTitle(argv[1], name_wstring, 256);
			WideCharToMultiByte(CP_ACP, 0, (LPCWSTR)name_wstring, -1, (char*)name_string, 256, NULL, NULL);
#else
			GetFileTitle(argv[1], name_string, 256);
#endif
			
			name_string_length = (uint32_t)strlen(name_string);
		}		
		
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

				put_code(Response::NoError);

				int loops = 0;
				while (!trayWndHandle && loops < 1000)
				{
					Sleep(1);
					loops++;
				}
				if(trayWndHandle)
					PostMessage(trayWndHandle, WM_COMMAND, (WPARAM)PORT_MENU_OFFSET + port_num, 0);		
								
			}
			break;

		case Command::InitSysTray:
			{
				_beginthreadex(NULL, 16384, &TrayThread, pEffect, 0, NULL);				

				InitSimpleResetEvents();
				lastUsedSysEx = getSelectedSysExIndex();
				
				//Insert SysEx resets to the beginning of the event list to prevent all piano problems at start.
				InsertOwnReset(0);
				InsertOwnReset(1);

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
				uint32_t port_num = get_code();				
				
				destPort = port_num;
				doOwnReset = true;

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
				resetInputEvents();	

				put_code(Response::NoError);
			}
			break;

		case Command::SendMidiEvent: // Send MIDI Event
			{				
				InputEventList::InputEvent* ev = &inputEventList.events[inputEventList.position];				
				inputEventList.position++;
				inputEventList.position &= (MAX_INPUTEVENT_COUNT - 1); 
				
				memset(ev, 0, sizeof(InputEventList::InputEvent));
					
				uint32_t b = get_code();

				uint32_t port = ((b & 0x7F000000) >> 24) & 1;
				isPortActive[port] = true;
				inputEventList.portMessageCount[port]++;

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
				InputEventList::InputEvent* ev = &inputEventList.events[inputEventList.position];
				inputEventList.position ++;
				inputEventList.position &= (MAX_INPUTEVENT_COUNT - 1);
				
				memset(ev, 0, sizeof(InputEventList::InputEvent));

				uint32_t size = get_code();

				uint32_t port = (size >> 24) & 1;
				isPortActive[port] = true;
				inputEventList.portMessageCount[port]++;

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
				bool tmpDoOwnReset = doOwnReset;

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

					size_t buffer_size = sizeof(float*) * (size_t)(pEffect[0]->numInputs + pEffect[0]->numOutputs * 2); // float lists
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

					sample_buffer.resize((size_t)BUFFER_SIZE * num_outputs_per_port);
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

				if (inputEventList.position)
				{	
					if (inputEventList.portMessageCount[0])
					{				
						renderEvents[0].numEvents = inputEventList.portMessageCount[0];
						renderEvents[0].reserved = 0;						

						for (unsigned int i = 0, j = 0; i < inputEventList.position && j < inputEventList.portMessageCount[0]; i++)
						{
							if (!inputEventList.events[i].port) renderEvents[0].events[j++] = (VstEvent*)&inputEventList.events[i].ev;							
						}

						pEffect[0]->dispatcher(pEffect[0], effProcessEvents, 0, 0, &renderEvents[0], 0);
					}					

					if (inputEventList.portMessageCount[1])
					{					
						renderEvents[1].numEvents = inputEventList.portMessageCount[1];
						renderEvents[1].reserved = 0;						

						for (unsigned int i = 0, j = 0; i < inputEventList.position && j < inputEventList.portMessageCount[1]; i++)
						{
							if (inputEventList.events[i].port) renderEvents[1].events[j++] = (VstEvent*)&inputEventList.events[i].ev;							
						}

						pEffect[1]->dispatcher(pEffect[1], effProcessEvents, 0, 0, &renderEvents[1], 0);
					}
				}

				if (tmpDoOwnReset) SendOwnReset(destPort);

				if (need_idle)
				{
					pEffect[0]->dispatcher(pEffect[0], DECLARE_VST_DEPRECATED(effIdle), 0, 0, 0, 0);
					pEffect[1]->dispatcher(pEffect[1], DECLARE_VST_DEPRECATED(effIdle), 0, 0, 0, 0);


					if (!idle_started)
					{
						if (inputEventList.portMessageCount[0]) pEffect[0]->dispatcher(pEffect[0], effProcessEvents, 0, 0, &renderEvents[0], 0);
						if (inputEventList.portMessageCount[1]) pEffect[1]->dispatcher(pEffect[1], effProcessEvents, 0, 0, &renderEvents[1], 0);
						if (tmpDoOwnReset) SendOwnReset(destPort);
						idle_started = true;
					}
				}

				uint32_t count = get_code();
				sample_pos += count;

				if (!driverResetRequested)
					put_code(Response::NoError);
				else
					put_code(Response::ResetRequest);


				while (count)
				{
					unsigned count_to_do = min(count, BUFFER_SIZE);
					unsigned num_outputs = min(pEffect[0]->numOutputs, 2);

					pEffect[0]->processReplacing(pEffect[0], float_list_in, float_list_out, count_to_do);
					pEffect[1]->processReplacing(pEffect[1], float_list_in, float_list_out + num_outputs, count_to_do);

#if (defined(_MSC_VER) && (_MSC_VER < 1600))
					float* out = &sample_buffer.front();
#else
					float* out = sample_buffer.data();
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

				resetInputEvents();
				break;						
			}			

		case Command::RenderAudioSamples4channel: // Render Samples for 4 channel mode
			{				
				bool tmpDoOwnReset = doOwnReset;

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

					size_t buffer_size = sizeof(float*) * (size_t)(pEffect[0]->numInputs + pEffect[0]->numOutputs * 2 * 2); // float lists
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

					sample_buffer.resize((size_t)BUFFER_SIZE * num_outputs_per_port * 2);
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
				
				if (inputEventList.position)
				{	
					if (inputEventList.portMessageCount[0])
					{						
						renderEvents[0].numEvents = inputEventList.portMessageCount[0];
						renderEvents[0].reserved = 0;						

						for (unsigned int i = 0, j = 0; i < inputEventList.position && j < inputEventList.portMessageCount[0]; i++)
						{
							if (!inputEventList.events[i].port) renderEvents[0].events[j++] = (VstEvent*)&inputEventList.events[i].ev;							
						}

						pEffect[0]->dispatcher(pEffect[0], effProcessEvents, 0, 0, &renderEvents[0], 0);
					}					

					if (inputEventList.portMessageCount[1])
					{					
						renderEvents[1].numEvents = inputEventList.portMessageCount[1];
						renderEvents[1].reserved = 0;						

						for (unsigned int i = 0, j = 0; i < inputEventList.position && j < inputEventList.portMessageCount[1]; i++)
						{
							if (inputEventList.events[i].port) renderEvents[1].events[j++] = (VstEvent*)&inputEventList.events[i].ev;							
						}

						pEffect[1]->dispatcher(pEffect[1], effProcessEvents, 0, 0, &renderEvents[1], 0);
					}
				}

				if (tmpDoOwnReset) SendOwnReset(destPort);

				if (need_idle)
				{
					pEffect[0]->dispatcher(pEffect[0], DECLARE_VST_DEPRECATED(effIdle), 0, 0, 0, 0);
					pEffect[1]->dispatcher(pEffect[1], DECLARE_VST_DEPRECATED(effIdle), 0, 0, 0, 0);


					if (!idle_started)
					{
						if (inputEventList.portMessageCount[0]) pEffect[0]->dispatcher(pEffect[0], effProcessEvents, 0, 0, &renderEvents[0], 0);
						if (inputEventList.portMessageCount[1]) pEffect[1]->dispatcher(pEffect[1], effProcessEvents, 0, 0, &renderEvents[1], 0);
						if (tmpDoOwnReset) SendOwnReset(destPort);
						idle_started = true;
					}
				}

				uint32_t count = get_code();
				sample_pos += count;

				if (!driverResetRequested)
					put_code(Response::NoError);
				else
					put_code(Response::ResetRequest);

				while (count)
				{
					unsigned count_to_do = min(count, BUFFER_SIZE);
					unsigned num_outputs = min(pEffect[0]->numOutputs, 2);

					pEffect[0]->processReplacing(pEffect[0], float_list_in, float_list_out, count_to_do);
					pEffect[1]->processReplacing(pEffect[1], float_list_in, float_list_out + num_outputs, count_to_do);

#if (defined(_MSC_VER) && (_MSC_VER < 1600))
					float* out = &sample_buffer.front();
#else
					float* out = sample_buffer.data();
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
				
				resetInputEvents();			
				break;
			}			

		default:
			code = Response::CommandUnknown;
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
	resetInputEvents();
	if (hDll) FreeLibrary(hDll);
	CoUninitialize();
	if (dll_dir) free(dll_dir);	
		
	
	//if (argv) LocalFree(argv); only needed when CommandLineToArgvW() was called.

	put_code(code);

	if (null_file)
	{
		CloseHandle(null_file);

		SetStdHandle(STD_INPUT_HANDLE, pipe_in);
		SetStdHandle(STD_OUTPUT_HANDLE, pipe_out);
	}
	
	Log(_T("Exit with code %d\n"), code);

	return code;
}
