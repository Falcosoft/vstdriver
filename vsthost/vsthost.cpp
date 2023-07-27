// vsthost.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include <process.h>

// #define LOG_EXCHANGE
// #define LOG
// #define MINIDUMP

enum
{
	BUFFER_SIZE = 4096
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


bool need_idle = false;
bool idle_started = false;

HWND editorHandle[2] = { 0, 0 };

static char* dll_dir = NULL;

static HANDLE null_file = NULL;
static HANDLE pipe_in = NULL;
static HANDLE pipe_out = NULL;


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

static class DialogMutexWin32
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
		memcpy(&out[chunk_size], chunk, size);
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
	switch (msg)
	{
	case WM_INITDIALOG:
		{	
			effect = (AEffect*)lParam;
			editorHandle[*(int*)effect->user] = hwnd;

			SetWindowLongPtr(hwnd, GWLP_USERDATA, lParam);

			wchar_t wText[18] = L"VST Editor port ";
			wchar_t intCnst[] = { 'A' + *(int*)effect->user };
			wcsncat(wText, intCnst, 1);

			SetWindowText(hwnd, wText);

			if (effect)
			{
				dialogMutex.Enter();

				SetTimer(hwnd, 1, 20, 0);
				effect->dispatcher(effect, effEditOpen, 0, 0, hwnd, 0);
				ERect* eRect = 0;
				effect->dispatcher(effect, effEditGetRect, 0, 0, &eRect, 0);
				if (eRect)
				{
					int width = eRect->right - eRect->left;
					int height = eRect->bottom - eRect->top;
					if (width < 50)
						width = 50;
					if (height < 50)
						height = 50;
					RECT wRect;
					SetRect(&wRect, 0, 0, width, height);
					AdjustWindowRectEx(&wRect, GetWindowLong(hwnd, GWL_STYLE), FALSE, GetWindowLong(hwnd, GWL_EXSTYLE));
					width = wRect.right - wRect.left;
					height = wRect.bottom - wRect.top;
					SetWindowPos(hwnd, HWND_TOP, 0, 0, width, height, SWP_NOMOVE);
				}

				dialogMutex.Leave();
			}
			SetForegroundWindow(hwnd);		
		}
		break;
	case WM_SIZE: //Fixes SC-VA display bug after parts section opened/closed
		effect = (AEffect*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
		if (effect && wParam == SIZE_RESTORED)
		{
			dialogMutex.Enter();
			ERect* eRect = 0;
			effect->dispatcher(effect, effEditGetRect, 0, 0, &eRect, 0);
			if (eRect)
			{
				int width = eRect->right - eRect->left;
				int height = eRect->bottom - eRect->top;
				if (width < 50)
					width = 50;
				if (height < 50)
					height = 50;
				RECT wRect;
				SetRect(&wRect, 0, 0, width, height);
				AdjustWindowRectEx(&wRect, GetWindowLong(hwnd, GWL_STYLE), FALSE, GetWindowLong(hwnd, GWL_EXSTYLE));
				width = wRect.right - wRect.left;
				height = wRect.bottom - wRect.top;
				SetWindowPos(hwnd, HWND_TOP, 0, 0, width, height, SWP_NOMOVE);
			}
			dialogMutex.Leave();
		}
		break;
	case WM_TIMER:
		effect = (AEffect*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
		if (effect)
			effect->dispatcher(effect, effEditIdle, 0, 0, 0, 0);
		break;
	case WM_CLOSE:
		{
			effect = (AEffect*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
			KillTimer(hwnd, 1);
			if (effect)
			{
				effect->dispatcher(effect, effEditClose, 0, 0, 0, 0);
				editorHandle[*(int*)effect->user] = 0;
			}

			EndDialog(hwnd, IDOK);
		}
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

	case audioMasterGetVendorString:
		strncpy((char*)ptr, "NoWork, Inc.", 64);
		//strncpy((char *)ptr, "YAMAHA", 64);
		break;

	case audioMasterGetProductString:
		strncpy((char*)ptr, "VSTi Host Bridge", 64);
		//strncpy((char *)ptr, "SOL/SQ01", 64);
		break;

	case audioMasterGetVendorVersion:
		return 1000;

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

static void EditorThread(void* threadparam)
{
	MyDLGTEMPLATE vstiEditor;
	AEffect* pEffect = (AEffect*)threadparam;
	vstiEditor.style = WS_POPUPWINDOW | WS_DLGFRAME | WS_MINIMIZEBOX | WS_SYSMENU | WS_CAPTION | DS_MODALFRAME | DS_CENTER;
	DialogBoxIndirectParam(0, &vstiEditor, 0, (DLGPROC)EditorProc, (LPARAM)pEffect);
	_endthread();
}

int CALLBACK _tWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPTSTR lpCmdLine, int nCmdShow)
{
	int argc = 0;
	LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

	if (argv == NULL || argc != 3) return Error::InvalidCommandLineArguments;

	wchar_t* end_char = 0;
	unsigned in_sum = wcstoul(argv[2], &end_char, 16);
	if (end_char == argv[2] || *end_char) return Error::MalformedChecksum;;

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

	HMODULE hDll = NULL;
	main_func pMain = NULL;
	AEffect* pEffect[2] = { 0, 0 };

	audioMasterData effectData[2] = { { 0 }, { 1 } };

	std::vector<uint8_t> blState;

	uint32_t max_num_outputs = 2;
	uint32_t sample_rate = 48000;
	uint32_t port_num = 0;

	std::vector<uint8_t> chunk;
	std::vector<float> sample_buffer;
	unsigned int samples_buffered = 0;

	null_file = CreateFile(_T("NUL"), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);

	pipe_in = GetStdHandle(STD_INPUT_HANDLE);
	pipe_out = GetStdHandle(STD_OUTPUT_HANDLE);

	SetStdHandle(STD_INPUT_HANDLE, null_file);
	SetStdHandle(STD_OUTPUT_HANDLE, null_file);

	{
		INITCOMMONCONTROLSEX icc;
		icc.dwSize = sizeof(icc);
		icc.dwICC = ICC_WIN95_CLASSES;
		if (!InitCommonControlsEx(&icc)) InitCommonControls();
		//InitCommonControlsEx can fail on Win 2000/XP without service packs. It's rude to exit in case of failing since this is not essentiall at all.
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

	max_num_outputs = min(pEffect[0]->numOutputs, 2);

	{
		char name_string[256] = { 0 };
		char vendor_string[256] = { 0 };
		char product_string[256] = { 0 };
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

		put_code(Response::NoError);
		put_code(name_string_length);
		put_code(vendor_string_length);
		put_code(product_string_length);
		put_code(vendor_version);
		put_code(unique_id);
		put_code(max_num_outputs);
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
					DialogBoxIndirectParam(0, &t, GetDesktopWindow(), (DLGPROC)EditorProc, (LPARAM)(pEffect[0]));
					getChunk(pEffect[0], chunk);
					setChunk(pEffect[1], chunk);
				}

				put_code(Response::NoError);
			}
			break;

		case Command::DisplayEditorModalThreaded: // Display Editor Modal in separate thread
			{
				port_num = get_code();

				if (pEffect[0]->flags & VstAEffectFlags::effFlagsHasEditor)
				{			
					_beginthread(EditorThread, 16384, pEffect[port_num]);
					//Sleep(100);
				}

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

		case Command::Reset: // Reset
			{
				port_num = get_code();
				if (editorHandle[port_num] != 0) SendMessage(editorHandle[port_num], WM_CLOSE, 0, 0);

				if (pEffect[port_num])
				{
					if (blState.size()) pEffect[port_num]->dispatcher(pEffect[port_num], effStopProcess, 0, 0, 0, 0);
					pEffect[port_num]->dispatcher(pEffect[port_num], effClose, 0, 0, 0, 0);
					pEffect[port_num] = NULL;
				}
				//if ( blState.size() ) pEffect[ 0 ]->dispatcher( pEffect[ 0 ], effStopProcess, 0, 0, 0, 0 );
				//pEffect[ 0 ]->dispatcher( pEffect[ 0 ], effClose, 0, 0, 0, 0 );

				blState.resize(0);

				freeChain();

				pEffect[port_num] = pMain(&audioMaster);
				if (!pEffect[port_num])
				{
					code = Response::CannotReset;
					goto exit;
				}
				pEffect[port_num]->user = &effectData[port_num];
				pEffect[port_num]->dispatcher(pEffect[port_num], effOpen, 0, 0, 0, 0);
				setChunk(pEffect[port_num], chunk);

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

				ev->port = (b & 0x7F000000) >> 24;
				if (ev->port > 1) ev->port = 1;
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
				uint32_t port = size >> 24;
				size &= 0xFFFFFF;

				ev->port = port;
				if (ev->port > 1) ev->port = 1;
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

					sample_buffer.resize((4096 + BUFFER_SIZE) * max_num_outputs);
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
							unsigned num_outputs = pEffect[0]->numOutputs;

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
					unsigned event_count[3] = { 0 };
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

				put_code(Response::NoError);

				while (count)
				{
					unsigned count_to_do = min(count, BUFFER_SIZE);
					unsigned num_outputs = pEffect[0]->numOutputs;

					pEffect[0]->processReplacing(pEffect[0], float_list_in, float_list_out, count_to_do);
					pEffect[1]->processReplacing(pEffect[1], float_list_in, float_list_out + num_outputs, count_to_do);

#if (defined(_MSC_VER) && (_MSC_VER < 1600))
					float* out = &sample_buffer.front() + samples_buffered * max_num_outputs;
#else
					float* out = sample_buffer.data() + samples_buffered * max_num_outputs;
#endif

					if (max_num_outputs == 2)
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
							out++;
						}
					}

#if (defined(_MSC_VER) && (_MSC_VER < 1600))
					put_bytes(&sample_buffer.front(), count_to_do * sizeof(float) * max_num_outputs);
#else
					put_bytes(sample_buffer.data(), count_to_do * sizeof(float) * max_num_outputs);
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

					sample_buffer.resize((4096 + BUFFER_SIZE) * max_num_outputs * 2);
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
							unsigned num_outputs = pEffect[0]->numOutputs;

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
					unsigned event_count[3] = { 0 };
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

				put_code(Response::NoError);

				while (count)
				{
					unsigned count_to_do = min(count, BUFFER_SIZE);
					unsigned num_outputs = pEffect[0]->numOutputs;

					pEffect[0]->processReplacing(pEffect[0], float_list_in, float_list_out, count_to_do);
					pEffect[1]->processReplacing(pEffect[1], float_list_in, float_list_out + num_outputs, count_to_do);

#if (defined(_MSC_VER) && (_MSC_VER < 1600))
					float* out = &sample_buffer.front() + samples_buffered * max_num_outputs;
#else
					float* out = sample_buffer.data() + samples_buffered * max_num_outputs;
#endif

					if (max_num_outputs == 2)
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
							float sample = (float_out[i] + float_out[i + BUFFER_SIZE * num_outputs]);
							out[0] = sample;
							out++;
						}
					}

#if (defined(_MSC_VER) && (_MSC_VER < 1600))
					put_bytes(&sample_buffer.front(), count_to_do * sizeof(float) * max_num_outputs * 2);
#else
					put_bytes(sample_buffer.data(), count_to_do * sizeof(float) * max_num_outputs * 2);
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
	if (editorHandle[0] != 0) SendMessage(editorHandle[0], WM_CLOSE, 0, 0);
	if (editorHandle[1] != 0) SendMessage(editorHandle[1], WM_CLOSE, 0, 0);

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
