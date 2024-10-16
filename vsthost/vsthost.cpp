// vsthost.cpp : Defines the entry point for the VST host application.
// Copyright (C) 2011 Chris Moeller, Brad Miller
// Copyright (C) 2023 Zoltan Bacsko - Falcosoft

#include "stdafx.h"
#include <process.h>
#include "../version.h"
#include "resource.h"
#include "wavewriter.h"
#include "log.h"

#ifdef _DEBUG
#define _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>
#endif

#pragma region Definitions/Variables

#define WIN32DEF(f) (WINAPI *f)
#define LOADUSER32FUNCTION(f) *((void**)&f)=GetProcAddress(user32,#f)

static BOOL WIN32DEF(DynGetModuleHandleEx)(DWORD dwFlags, LPCTSTR lpModuleName, HMODULE* phModule) = NULL;
static BOOL WIN32DEF(DynAllowSetForegroundWindow)(DWORD dwProcessId) = NULL;
static HANDLE WIN32DEF(SetThreadDpiAwarenessContext)(HANDLE dpiContext) = NULL;

#if(_WIN32_WINNT < 0x0500)
#define SM_CXVIRTUALSCREEN  78
#define SM_CYVIRTUALSCREEN  79
#define ASFW_ANY    ((DWORD)-1)
#endif

#define WM_ICONMSG WM_APP + 1
#define MAX_PORTS 2
#define MAX_OUTPUTS 2
#define MAX_PLUGINS 10

#define RESET_MENU_OFFSET 10
#define PORT_MENU_OFFSET 100
#define OTHER_MENU_OFFSET 20
#define WAVEWRITE_START_STOP OTHER_MENU_OFFSET + 1
#define WAVEWRITE_ERROR OTHER_MENU_OFFSET + 2
#define SHOW_INFO OTHER_MENU_OFFSET + 3

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

static const unsigned char gmReset[] = { 0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7 };
static const unsigned char gsReset[] = { 0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7 };
static const unsigned char xgReset[] = { 0xF0, 0x43, 0x10, 0x4C, 0x00, 0x00, 0x7E, 0x00, 0xF7 };
static const unsigned char gm2Reset[] = { 0xF0, 0x7E, 0x7F, 0x09, 0x03, 0xF7 };

static struct ResetVstEvent
{
	VstInt32 numEvents;
	VstIntPtr reserved;
	VstEvent* events[RESET_MIDIMSG_COUNT];

} resetVstEvents = { 0 };

static VstMidiEvent resetMidiEvents[RESET_MIDIMSG_COUNT] = { 0 };

static struct InputEventList
{
	unsigned position;
	unsigned portMessageCount[MAX_PORTS];
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

static struct PortState
{
	HWND editorHandle;
	bool isPortActive;
	bool alwaysUseHostEditor;
	bool alwaysOnTop;
	int effectData;
	AEffect* pEffect;
	struct RenderVstEvent
	{
		VstInt32 numEvents;
		VstIntPtr reserved;
		VstEvent* events[MAX_INPUTEVENT_COUNT];

	}renderEvents;

	PortState() :
		editorHandle(),
		isPortActive(),
		alwaysUseHostEditor(),
		effectData(),
		pEffect(),
		renderEvents(),
		alwaysOnTop(true) {}

} portState[MAX_PORTS];

LPTSTR* argv = NULL;

static NOTIFYICONDATA nIconData = { 0 };
static HMENU trayMenu = NULL;

static volatile int lastUsedSysEx = RESET_MENU_OFFSET + 5;
static volatile bool doOwnReset = false;
static bool driverResetRequested = false;
static bool need_idle = false;
static bool isYamahaPlugin = false;

static volatile bool is4channelMode = false;

static HANDLE highDpiMode = NULL;

static volatile DWORD destPort = PORT_ALL;

static uint32_t sample_rate = 48000;
static uint32_t sample_pos = 0;

static DWORD MainThreadId;
static char* dll_dir = NULL;

static char product_string[256] = { 0 };

static HANDLE pipe_in = NULL;
static HANDLE pipe_out = NULL;

static volatile HWND trayWndHandle = NULL;

static Win32Lock dialogLock(true);

static std::vector<uint8_t> blState;

static WaveWriter waveWriter;

#pragma endregion Global definitions and variables

#pragma region Utility_Functions
static void InvalidParamHandler(const wchar_t* expression, const wchar_t* function, const wchar_t* file, unsigned line, uintptr_t pReserved)
{
	if (MessageBox(NULL, _T("An unexpected invalid parameter error occured.\r\nDo you want to try to continue?"), _T("VST Host Bridge Error"), MB_YESNO | MB_ICONERROR | MB_SYSTEMMODAL) == IDNO)
		TerminateProcess(GetCurrentProcess(), 1);
}

LONG __stdcall myExceptFilterProc(LPEXCEPTION_POINTERS param)
{
	void setSelectedSysExIndex(int index); //forward declaration...

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

		MessageBox(NULL, buffer, _T("VST Midi Driver"), MB_OK | MB_SYSTEMMODAL | MB_ICONERROR);
		lastUsedSysEx = 15;
		setSelectedSysExIndex(lastUsedSysEx);
		Shell_NotifyIcon(NIM_DELETE, &nIconData);
		DestroyMenu(trayMenu);
		TerminateProcess(GetCurrentProcess(), 1);
		return EXCEPTION_EXECUTE_HANDLER;
	}
}


#pragma comment(lib,"Version.lib") 
static TCHAR* GetRunTimeFileVersion(TCHAR* filePath, TCHAR* result, unsigned buffSize)
{
	DWORD dwSize = 0;
	BYTE* pVersionInfo = NULL;
	VS_FIXEDFILEINFO* pFileInfo = NULL;
	UINT pLenFileInfo = 0;
	TCHAR tmpBuff[MAX_PATH] = { 0 };

	if (!filePath)
		GetModuleFileName(NULL, tmpBuff, MAX_PATH);
	else
		_tcscpy_s(tmpBuff, filePath);

	dwSize = GetFileVersionInfoSize(tmpBuff, NULL);
	if (dwSize == 0)
	{
		return NULL;
	}

	pVersionInfo = (BYTE*)calloc(dwSize, sizeof(BYTE));
	if (!pVersionInfo) return NULL;

	if (!GetFileVersionInfo(tmpBuff, 0, dwSize, pVersionInfo))
	{
		free(pVersionInfo);
		return NULL;
	}

	if (!VerQueryValue(pVersionInfo, TEXT("\\"), (LPVOID*)&pFileInfo, &pLenFileInfo))
	{
		free(pVersionInfo);
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

	free(pVersionInfo);
	return result;
}

void resetInputEvents()
{
	for (unsigned i = 0; i < inputEventList.position; i++)
		if (inputEventList.events[i].ev.sysexEvent.type == kVstSysExType && inputEventList.events[i].ev.sysexEvent.sysexDump)
		{
			free(inputEventList.events[i].ev.sysexEvent.sysexDump);
			inputEventList.events[i].ev.sysexEvent.sysexDump = NULL;
		}

	inputEventList.position = 0;
	inputEventList.portMessageCount[0] = 0;
	inputEventList.portMessageCount[1] = 0;
}

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
	uint32_t num_programs = pEffect->numPrograms;
	uint32_t num_params = pEffect->numParams;
	uint32_t orgProgramIndex = (uint32_t)pEffect->dispatcher(pEffect, effGetProgram, 0, 0, NULL, 0.0);

	out.resize(0);
	uint32_t unique_id = pEffect->uniqueID;
	append_be(out, unique_id);
	bool type_chunked = !!(pEffect->flags & effFlagsProgramChunks);
	append_be(out, type_chunked);
	if (!type_chunked)
	{
		if (num_programs > 1 && orgProgramIndex > 0)
			pEffect->dispatcher(pEffect, effSetProgram, 0, 0, NULL, 0.0);

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

	if (num_programs > 1)
	{
		append_be(out, (uint32_t)'VstD'); //From now on in case of more programs we save all programs. These new markers are for file version check. 
		append_be(out, (uint32_t)'Ver2');
		append_be(out, num_programs);
		append_be(out, orgProgramIndex);
		if (!type_chunked)
		{
			for (unsigned j = 1; j < num_programs; ++j)
			{
				pEffect->dispatcher(pEffect, effSetProgram, 0, j, NULL, 0.0);
				for (unsigned i = 0; i < num_params; ++i)
				{
					float parameter = pEffect->getParameter(pEffect, i);
					append_be(out, parameter);
				}
			}
			pEffect->dispatcher(pEffect, effSetProgram, 0, orgProgramIndex, NULL, 0.0);
		}
	}
}

void setChunk(AEffect* pEffect, std::vector<uint8_t> const& in)
{
	unsigned size = (unsigned)in.size();
	if (pEffect && size)
	{
		const uint8_t* inc = &in.front();
		
		uint32_t num_programs = pEffect->numPrograms;
		uint32_t orgProgramIndex = (uint32_t)pEffect->dispatcher(pEffect, effGetProgram, 0, 0, NULL, 0.0);
		uint32_t num_params;

		uint32_t effect_id;
		retrieve_be(effect_id, inc, size);
		if (effect_id != pEffect->uniqueID) return;
		bool type_chunked;
		retrieve_be(type_chunked, inc, size);
		if (type_chunked != !!(pEffect->flags & effFlagsProgramChunks)) return;
		if (!type_chunked)
		{
			retrieve_be(num_params, inc, size);
			if (num_params != pEffect->numParams) return;

			if (num_programs > 1 && orgProgramIndex > 0)
				pEffect->dispatcher(pEffect, effSetProgram, 0, 0, NULL, 0.0);

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
			size -= chunk_size;
			inc += chunk_size;
		}

		if (num_programs > 1 && size >= (sizeof(uint32_t) * 4))
		{
			uint32_t verMarker;
			uint32_t nPrograms;
			uint32_t orgIndex;

			retrieve_be(verMarker, inc, size);
			if (verMarker != (uint32_t)'VstD') return;
			retrieve_be(verMarker, inc, size);
			if (verMarker != (uint32_t)'Ver2') return;
			retrieve_be(nPrograms, inc, size);
			if (nPrograms != num_programs) return;
			retrieve_be(orgIndex, inc, size);

			if (!type_chunked)
			{
				for (unsigned j = 1; j < num_programs; ++j)
				{
					pEffect->dispatcher(pEffect, effSetProgram, 0, j, NULL, 0.0);
					for (unsigned i = 0; i < num_params; ++i)
					{
						float parameter;
						retrieve_be(parameter, inc, size);
						pEffect->setParameter(pEffect, i, parameter);
					}
				}
			}

			pEffect->dispatcher(pEffect, effSetProgram, 0, orgIndex, NULL, 0.0);
		}

	}
}

BOOL settings_save(AEffect* pEffect)
{
	BOOL retResult = FALSE;

	TCHAR vst_path[MAX_PATH] = { 0 };
	_tcscpy_s(vst_path, argv[1]);
	TCHAR* chrP = _tcsrchr(vst_path, '.'); // removes extension
	if (chrP) chrP[0] = 0;
	_tcscat_s(vst_path, _T(".set"));

	HANDLE fileHandle = CreateFile(vst_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

	if (fileHandle != INVALID_HANDLE_VALUE)
	{
		DWORD size;
		std::vector<uint8_t> chunk;
		if (pEffect) getChunk(pEffect, chunk);

		if (chunk.size() >= (2 * sizeof(uint32_t) + sizeof(bool))) retResult = WriteFile(fileHandle, &chunk.front(), (DWORD)chunk.size(), &size, NULL);

		CloseHandle(fileHandle);
	}

	return retResult;
}

void getEditorPosition(int port, int& x, int& y)
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

void setEditorPosition(int port, int x, int y)
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

void InitSimpleResetEvents()
{
	resetVstEvents.numEvents = RESET_MIDIMSG_COUNT;
	resetVstEvents.reserved = 0;
	const int ChannelCount = 16;
	const unsigned char ControllerStatus = 0xB0U;

	for (int i = 0; i < ChannelCount; i++)
	{
		DWORD msg, index;

		msg = (ControllerStatus | i) | (0x40 << 8); //Sustain off		
		index = i * 4;
		memcpy(&resetMidiEvents[index].midiData, &msg, 3);
		resetMidiEvents[index].type = kVstMidiType;
		resetMidiEvents[index].byteSize = sizeof(VstMidiEvent);
		resetMidiEvents[index].flags = VstMidiEventFlags::kVstMidiEventIsRealtime;
		resetVstEvents.events[index] = (VstEvent*)&resetMidiEvents[index];

		msg = (ControllerStatus | i) | (0x7B << 8); //All Notes off
		index = i * 4 + 1;
		memcpy(&resetMidiEvents[index].midiData, &msg, 3);
		resetMidiEvents[index].type = kVstMidiType;
		resetMidiEvents[index].byteSize = sizeof(VstMidiEvent);
		resetMidiEvents[index].flags = VstMidiEventFlags::kVstMidiEventIsRealtime;
		resetVstEvents.events[index] = (VstEvent*)&resetMidiEvents[index];

		msg = (ControllerStatus | i) | (0x79 << 8);  //All Controllers off
		index = i * 4 + 2;
		memcpy(&resetMidiEvents[index].midiData, &msg, 3);
		resetMidiEvents[index].type = kVstMidiType;
		resetMidiEvents[index].byteSize = sizeof(VstMidiEvent);
		resetMidiEvents[index].flags = VstMidiEventFlags::kVstMidiEventIsRealtime;
		resetVstEvents.events[index] = (VstEvent*)&resetMidiEvents[index];

		msg = (ControllerStatus | i) | (0x78 << 8);  //All Sounds off
		index = i * 4 + 3;
		memcpy(&resetMidiEvents[index].midiData, &msg, 3);
		resetMidiEvents[index].type = kVstMidiType;
		resetMidiEvents[index].byteSize = sizeof(VstMidiEvent);
		resetMidiEvents[index].flags = VstMidiEventFlags::kVstMidiEventIsRealtime;
		resetVstEvents.events[index] = (VstEvent*)&resetMidiEvents[index];
	}
}

void InsertSysExEvent(const char* sysExBytes, int size, DWORD portNum)
{
	InputEventList::InputEvent* ev = &inputEventList.events[inputEventList.position];
	inputEventList.position++;
	inputEventList.position &= (MAX_INPUTEVENT_COUNT - 1);

	memset(ev, 0, sizeof(InputEventList::InputEvent));

	inputEventList.portMessageCount[portNum]++;

	ev->port = portNum;
	ev->ev.sysexEvent.type = kVstSysExType;
	ev->ev.sysexEvent.byteSize = sizeof(ev->ev.sysexEvent);
	ev->ev.sysexEvent.dumpBytes = size;
	ev->ev.sysexEvent.sysexDump = (char*)malloc(size);

	if (ev->ev.sysexEvent.sysexDump)
		memcpy(ev->ev.sysexEvent.sysexDump, sysExBytes, size);
}

void InsertOwnReset(DWORD portNum)
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

void sendSysExEvent(char* sysExBytes, int size, DWORD portNum)
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
		portState[0].pEffect->dispatcher(portState[0].pEffect, effProcessEvents, 0, 0, &vstEvents, 0);

	if (portNum)
		portState[1].pEffect->dispatcher(portState[1].pEffect, effProcessEvents, 0, 0, &vstEvents, 0);
}

void sendSimpleResetEvents(DWORD portNum)
{
	if (!portNum || portNum == PORT_ALL)
		portState[0].pEffect->dispatcher(portState[0].pEffect, effProcessEvents, 0, 0, &resetVstEvents, 0);

	if (portNum)
		portState[1].pEffect->dispatcher(portState[1].pEffect, effProcessEvents, 0, 0, &resetVstEvents, 0);
}

void SendOwnReset(DWORD portNum)
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

bool getPluginMenuItem(int itemIndex, TCHAR* result, unsigned buffSize)
{
	long lResult;
	DWORD dwType = REG_SZ;
	HKEY hKey;
	ULONG size;

	lResult = RegOpenKeyEx(HKEY_CURRENT_USER, _T("Software\\VSTi Driver"), 0, KEY_READ, &hKey);
	if (lResult == ERROR_SUCCESS)
	{
		TCHAR szValueName[12] = _T("plugin");
		TCHAR szPluginNum[4] = { 0 };

		if (itemIndex)
		{
			_itot_s(itemIndex, szPluginNum, 10);
			_tcsncat_s(szValueName, szPluginNum, _countof(szPluginNum));
		}

		lResult = RegQueryValueEx(hKey, szValueName, NULL, &dwType, NULL, &size);
		if (lResult == ERROR_SUCCESS && dwType == REG_SZ && size > 2)
		{
			TCHAR vst_path[MAX_PATH] = { 0 };
			lResult = RegQueryValueEx(hKey, szValueName, NULL, &dwType, (LPBYTE)vst_path, &size);
			if (lResult == ERROR_SUCCESS)
			{
				TCHAR vst_title[MAX_PATH - 6] = { 0 };
				RegCloseKey(hKey);

				GetFileTitle(vst_path, vst_title, MAX_PATH - 6);
				_itot_s(itemIndex, szPluginNum, 10);
				_tcscpy_s(result, buffSize, szPluginNum);
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

void renderAudiosamples(int channelCount)
{
	static bool idle_started = false;
	static std::vector<float> sample_buffer;
	static float** float_list_in;
	static float** float_list_out;
	static float* float_null;
	static float* float_out;

	bool tmpDoOwnReset = doOwnReset;

	unsigned channelMult = (unsigned)channelCount / 2;

	if (blState.empty())
	{
		portState[0].pEffect->dispatcher(portState[0].pEffect, effSetSampleRate, 0, 0, NULL, float(sample_rate));
		portState[0].pEffect->dispatcher(portState[0].pEffect, effSetBlockSize, 0, BUFFER_SIZE, NULL, 0);
		portState[0].pEffect->dispatcher(portState[0].pEffect, effMainsChanged, 0, 1, NULL, 0);
		portState[0].pEffect->dispatcher(portState[0].pEffect, effStartProcess, 0, 0, NULL, 0);

		portState[1].pEffect->dispatcher(portState[1].pEffect, effSetSampleRate, 0, 0, NULL, float(sample_rate));
		portState[1].pEffect->dispatcher(portState[1].pEffect, effSetBlockSize, 0, BUFFER_SIZE, NULL, 0);
		portState[1].pEffect->dispatcher(portState[1].pEffect, effMainsChanged, 0, 1, NULL, 0);
		portState[1].pEffect->dispatcher(portState[1].pEffect, effStartProcess, 0, 0, NULL, 0);

		size_t buffer_size = sizeof(float*) * (size_t)(portState[0].pEffect->numInputs + portState[0].pEffect->numOutputs * channelCount); // float lists
		buffer_size += sizeof(float) * BUFFER_SIZE;                                // null input
		buffer_size += sizeof(float) * BUFFER_SIZE * portState[0].pEffect->numOutputs * channelCount;          // outputs

		blState.resize(buffer_size);

		float_list_in = (float**)(blState.size() ? &blState.front() : NULL);

		float_list_out = float_list_in + portState[0].pEffect->numInputs;
		float_null = (float*)(float_list_out + portState[0].pEffect->numOutputs * channelCount);
		float_out = float_null + BUFFER_SIZE;

		for (VstInt32 i = 0; i < portState[0].pEffect->numInputs; ++i)      float_list_in[i] = float_null;
		for (VstInt32 i = 0; i < portState[0].pEffect->numOutputs * channelCount; ++i) float_list_out[i] = float_out + BUFFER_SIZE * i;

		memset(float_null, 0, sizeof(float) * BUFFER_SIZE);

		sample_buffer.resize((size_t)BUFFER_SIZE * MAX_OUTPUTS * channelMult);
	}

	if (need_idle)
	{
		portState[0].pEffect->dispatcher(portState[0].pEffect, DECLARE_VST_DEPRECATED(effIdle), 0, 0, NULL, 0);
		portState[1].pEffect->dispatcher(portState[1].pEffect, DECLARE_VST_DEPRECATED(effIdle), 0, 0, NULL, 0);

		if (!idle_started)
		{
			unsigned idle_run = BUFFER_SIZE * 200;

			while (idle_run)
			{
				unsigned count_to_do = min(idle_run, BUFFER_SIZE);
				unsigned num_outputs = min(portState[0].pEffect->numOutputs, MAX_OUTPUTS);

				portState[0].pEffect->processReplacing(portState[0].pEffect, float_list_in, float_list_out, count_to_do);
				portState[1].pEffect->processReplacing(portState[1].pEffect, float_list_in, float_list_out + num_outputs, count_to_do);

				portState[0].pEffect->dispatcher(portState[0].pEffect, DECLARE_VST_DEPRECATED(effIdle), 0, 0, NULL, 0);
				portState[1].pEffect->dispatcher(portState[1].pEffect, DECLARE_VST_DEPRECATED(effIdle), 0, 0, NULL, 0);

				idle_run -= count_to_do;
			}
		}
	}

	if (inputEventList.position)
	{
		if (inputEventList.portMessageCount[0])
		{
			portState[0].renderEvents.numEvents = inputEventList.portMessageCount[0];
			portState[0].renderEvents.reserved = 0;

			for (unsigned i = 0, j = 0; i < inputEventList.position && j < inputEventList.portMessageCount[0]; i++)
			{
				if (!inputEventList.events[i].port) portState[0].renderEvents.events[j++] = (VstEvent*)&inputEventList.events[i].ev;
			}

			portState[0].pEffect->dispatcher(portState[0].pEffect, effProcessEvents, 0, 0, &portState[0].renderEvents, 0);
		}

		if (inputEventList.portMessageCount[1])
		{
			portState[1].renderEvents.numEvents = inputEventList.portMessageCount[1];
			portState[1].renderEvents.reserved = 0;

			for (unsigned i = 0, j = 0; i < inputEventList.position && j < inputEventList.portMessageCount[1]; i++)
			{
				if (inputEventList.events[i].port) portState[1].renderEvents.events[j++] = (VstEvent*)&inputEventList.events[i].ev;
			}

			portState[1].pEffect->dispatcher(portState[1].pEffect, effProcessEvents, 0, 0, &portState[1].renderEvents, 0);
		}
	}

	if (tmpDoOwnReset) SendOwnReset(destPort);

	if (need_idle)
	{
		portState[0].pEffect->dispatcher(portState[0].pEffect, DECLARE_VST_DEPRECATED(effIdle), 0, 0, NULL, 0);
		portState[1].pEffect->dispatcher(portState[1].pEffect, DECLARE_VST_DEPRECATED(effIdle), 0, 0, NULL, 0);


		if (!idle_started)
		{
			if (inputEventList.portMessageCount[0]) portState[0].pEffect->dispatcher(portState[0].pEffect, effProcessEvents, 0, 0, &portState[0].renderEvents, 0);
			if (inputEventList.portMessageCount[1]) portState[1].pEffect->dispatcher(portState[1].pEffect, effProcessEvents, 0, 0, &portState[1].renderEvents, 0);
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
		unsigned num_outputs = min(portState[0].pEffect->numOutputs, MAX_OUTPUTS);

		portState[0].pEffect->processReplacing(portState[0].pEffect, float_list_in, float_list_out, count_to_do);
		portState[1].pEffect->processReplacing(portState[1].pEffect, float_list_in, float_list_out + num_outputs, count_to_do);

		float* out = &sample_buffer.front();

		if (channelCount == 2)
		{
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

		}
		else
		{
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
		}

		uint32_t byteSize = count_to_do * sizeof(float) * MAX_OUTPUTS * channelMult;        

		put_bytes(&sample_buffer.front(), byteSize);
		
		if (waveWriter.getIsRecordingStarted())
		{			
			waveWriter.WriteData(&sample_buffer.front(), byteSize);
		}
				
		count -= count_to_do;
	}
}
#pragma endregion Utility functions

#pragma region VST_Windows

struct MyDLGTEMPLATE : DLGTEMPLATE
{
	WORD ext[3];
	MyDLGTEMPLATE()
	{
		memset(this, 0, sizeof(*this));
	};
};

struct DialogState
{
	HWND checkBoxWnd;
	HWND buttonWnd;
	HWND button2Wnd;
	HWND savedHandle;
	HFONT hFont;

	DialogState() :
		checkBoxWnd(),
		buttonWnd(),
		button2Wnd(),
		savedHandle(),
		hFont() {}
};

INT_PTR CALLBACK GeneralUiProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	AEffect* effect;
	int portNum = 0;

	static bool sameThread = true;
	static DialogState dialogState[MAX_PORTS];

	HWND parametersListBox = GetDlgItem(hwnd, IDC_LIST1);
	HWND programsListBox = GetDlgItem(hwnd, IDC_LIST2);
	HWND valueSlider = GetDlgItem(hwnd, IDC_SLIDER_VAL);
	HWND valueText = GetDlgItem(hwnd, IDC_STATIC_VAL);

	switch (msg)
	{
	case WM_INITDIALOG:
		effect = reinterpret_cast<AEffect*>(lParam);
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
			HDC screen = GetDC(NULL);
			float dpiMul = (float)(GetDeviceCaps(screen, LOGPIXELSY)) / 96.0f;
			ReleaseDC(NULL, screen);

			portNum = *(int*)effect->user;

			portState[portNum].isPortActive = true;
			dialogState[portNum].savedHandle = portState[portNum].editorHandle;
			portState[portNum].editorHandle = hwnd;
			SetWindowLongPtr(hwnd, GWLP_USERDATA, lParam);

			VstInt16 extraHeight = 0;
			if (GetCurrentThreadId() != MainThreadId)
			{
				extraHeight = (int)(16 * dpiMul);
				sameThread = false;
			}

			TCHAR wText[28] = { 0 };
			if (sameThread)
			{
				SetWindowLongPtr(hwnd, GWL_STYLE, WS_POPUPWINDOW | WS_DLGFRAME | DS_MODALFRAME | DS_CENTER);
				_tcscpy_s(wText, _T("VST General Editor"));
			}
			else
			{
#pragma warning(disable:4838) //fake warning even after casting
				TCHAR portSign[] = { _T('A') + static_cast<TCHAR>(portNum) };
#pragma warning(default:4838)
				_tcscpy_s(wText, _T("VST General Editor port "));				
				_tcsncat_s(wText, portSign, 1);
			}			

			SetWindowText(hwnd, wText);					

			int xPos = 0xFFFFFF; //16M not likely to be real window position
			int yPos = 0xFFFFFF;
			getEditorPosition(portNum, xPos, yPos);

			RECT eRect;
			GetWindowRect(hwnd, &eRect);
			int width = eRect.right - eRect.left;
			int height = eRect.bottom - eRect.top + extraHeight;

			if (xPos == 0xFFFFFF || yPos == 0xFFFFFF)
				SetWindowPos(hwnd, HWND_TOP, 0, 0, width, height, SWP_NOMOVE);
			else
				SetWindowPos(hwnd, HWND_TOP, xPos, yPos, width, height, 0);

			if (!sameThread)
			{
				LOGFONT lf = { 0 };

				GetClientRect(hwnd, &eRect);
				width = eRect.right - eRect.left;
				height = eRect.bottom - eRect.top;

				dialogState[portNum].checkBoxWnd = CreateWindowEx(NULL, _T("BUTTON"), _T("Always on Top"), WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX, (int)(5 * dpiMul), height - (int)(23 * dpiMul), (int)(90 * dpiMul), (int)(20 * dpiMul), hwnd, NULL, ((LPCREATESTRUCT)lParam)->hInstance, NULL);

				dialogState[portNum].buttonWnd = CreateWindowEx(NULL, _T("BUTTON"), _T("Save Settings"), WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, width - (int)(84 * dpiMul), height - (int)(24 * dpiMul), (int)(80 * dpiMul), (int)(20 * dpiMul), hwnd, NULL, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
				dialogState[portNum].button2Wnd = CreateWindowEx(NULL, _T("BUTTON"), _T("Switch UI"), WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, (int)(100 * dpiMul), height - (int)(24 * dpiMul), (int)(68 * dpiMul), (int)(20 * dpiMul), hwnd, NULL, ((LPCREATESTRUCT)lParam)->hInstance, NULL);

				GetObject(GetStockObject(DEFAULT_GUI_FONT), sizeof(LOGFONT), &lf);
				dialogState[portNum].hFont = CreateFontIndirect(&lf);
				SendMessage(dialogState[portNum].checkBoxWnd, WM_SETFONT, (WPARAM)dialogState[portNum].hFont, TRUE);
				SendMessage(dialogState[portNum].buttonWnd, WM_SETFONT, (WPARAM)dialogState[portNum].hFont, TRUE);
				SendMessage(dialogState[portNum].button2Wnd, WM_SETFONT, (WPARAM)dialogState[portNum].hFont, TRUE);
				if (!(effect->flags & effFlagsHasEditor)) EnableWindow(dialogState[portNum].button2Wnd, FALSE);
			}

			////								
			int numProgs = effect->numPrograms;
			int numParams = effect->numParams;
			int orgProgramIndex = (int)effect->dispatcher(effect, effGetProgram, 0, 0, NULL, 0.0);
			char tmpName[48] = { 0 };

			SetWindowTextA(GetDlgItem(hwnd, IDC_STATIC_PLUGIN), product_string);

			SendMessage(parametersListBox, WM_SETREDRAW, FALSE, 0); //speeds up ListBox drawing
			for (int i = 0; i < numParams; i++)
			{
				effect->dispatcher(effect, effGetParamName, i, 0, tmpName, 0.0);

				char tmpBuff[56] = { 0 };
				char prefix[6] = { 0 };
				_itoa_s(i, prefix, 10);
				strncpy_s(tmpBuff, prefix, sizeof(prefix));
				strncat_s(tmpBuff, ". ", 2);
				strncat_s(tmpBuff, tmpName, sizeof(tmpName));

				SendMessageA(parametersListBox, LB_ADDSTRING, 0, (LPARAM)tmpBuff);
			}
			SendMessage(parametersListBox, WM_SETREDRAW, TRUE, 0);

			//////////			

			SendMessage(programsListBox, WM_SETREDRAW, FALSE, 0); //speeds up ListBox drawing
			if (effect->dispatcher(effect, effGetProgramNameIndexed, 0, 0, tmpName, 0.0) != 0)
			{
				char tmpBuff[56] = { 0 };
				char prefix[6] = { 0 };

				strncpy_s(tmpBuff, "0. ", 3);
				strncat_s(tmpBuff, tmpName, sizeof(tmpName));

				SendMessageA(programsListBox, LB_ADDSTRING, 0, (LPARAM)tmpBuff);

				for (int i = 1; i < numProgs; i++)
				{
					effect->dispatcher(effect, effGetProgramNameIndexed, i, 0, tmpName, 0.0);
					_itoa_s(i, prefix, 10);
					strncpy_s(tmpBuff, prefix, sizeof(prefix));
					strncat_s(tmpBuff, ". ", 2);
					strncat_s(tmpBuff, tmpName, sizeof(tmpName));

					SendMessageA(programsListBox, LB_ADDSTRING, 0, (LPARAM)tmpBuff);
				}
			}
			else
			{
				for (int i = 0; i < numProgs; i++)
				{
					effect->dispatcher(effect, effSetProgram, 0, i, NULL, 0.0);
					effect->dispatcher(effect, effGetProgramName, 0, 0, tmpName, 0.0);

					char tmpBuff[56] = { 0 };
					char prefix[6] = { 0 };
					_itoa_s(i, prefix, 10);
					strncpy_s(tmpBuff, prefix, sizeof(prefix));
					strncat_s(tmpBuff, ". ", 2);
					strncat_s(tmpBuff, tmpName, sizeof(tmpName));

					SendMessageA(programsListBox, LB_ADDSTRING, 0, (LPARAM)tmpBuff);
				}
			}
			SendMessage(programsListBox, WM_SETREDRAW, TRUE, 0);

			effect->dispatcher(effect, effSetProgram, 0, orgProgramIndex, NULL, 0.0);
			SendMessage(programsListBox, LB_SETCURSEL, orgProgramIndex, 0);

			int sliderMax = 1000;
			SendMessage(valueSlider, TBM_SETRANGEMIN, FALSE, 0);
			SendMessage(valueSlider, TBM_SETRANGEMAX, FALSE, sliderMax);
			SendMessage(valueSlider, TBM_SETTICFREQ, sliderMax / 2, 0);
			SendMessage(valueSlider, TBM_SETLINESIZE, 0, 1);
			SendMessage(valueSlider, TBM_SETPAGESIZE, 0, 10);
			SendMessage(valueSlider, TBM_SETPOS, TRUE, sliderMax / 2);
		}
		return TRUE;			
	case WM_SHOWWINDOW:
		effect = reinterpret_cast<AEffect*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
		if (effect && wParam)
		{
			portNum = *(int*)effect->user;
			if (dialogState[portNum].checkBoxWnd) SendMessage(dialogState[portNum].checkBoxWnd, BM_SETCHECK, portState[portNum].alwaysOnTop ? BST_CHECKED : BST_UNCHECKED, 0);
			SetWindowPos(hwnd, portState[portNum].alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
			SetForegroundWindow(hwnd);
		}
		break;
	case WM_VSCROLL:
		effect = reinterpret_cast<AEffect*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
		if (effect && lParam == (LPARAM)valueSlider)
		{
			//portNum = *(int*)effect->user;
			int selectedIndex = (int)SendMessage(parametersListBox, LB_GETCURSEL, 0, 0);
			if (selectedIndex >= 0)
			{
				int selectedValue = (int)SendMessage(valueSlider, TBM_GETPOS, 0, 0);
				effect->setParameter(effect, selectedIndex, (1000 - selectedValue) / 1000.0f);
				PostMessage(hwnd, WM_COMMAND, LBN_SELCHANGE << 16, (LPARAM)parametersListBox);
			}
		}

		break;
	case WM_COMMAND:
		if (wParam == IDCANCEL)
		{
			PostMessage(hwnd, WM_CLOSE, 0, 0);
			return TRUE;
		}

		effect = reinterpret_cast<AEffect*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
		if (effect)
		{
			portNum = *(int*)effect->user;			

			if (HIWORD(wParam) == LBN_SELCHANGE && lParam == (LPARAM)programsListBox)
			{
				int selectedIndex = (int)SendMessage(programsListBox, LB_GETCURSEL, 0, 0);
				if (selectedIndex >= 0)
				{
					effect->dispatcher(effect, effSetProgram, 0, selectedIndex, NULL, 0.0);
					PostMessage(hwnd, WM_COMMAND, LBN_SELCHANGE << 16 | IDC_LIST1, (LPARAM)parametersListBox);
				}
			}
			else if (HIWORD(wParam) == LBN_SELCHANGE && lParam == (LPARAM)parametersListBox)
			{
				int selectedIndex = (int)SendMessage(parametersListBox, LB_GETCURSEL, 0, 0);
				if (selectedIndex >= 0)
				{
					char dispBuff[64] = { 0 };
					char unitBuff[32] = { 0 };

					float param = effect->getParameter(effect, selectedIndex);
					if (LOWORD(wParam) == IDC_LIST1) SendMessage(valueSlider, TBM_SETPOS, TRUE, (LPARAM)(1000 - int(param * 1000)));

					effect->dispatcher(effect, effGetParamDisplay, selectedIndex, 0, (void*)dispBuff, 0.0);
					effect->dispatcher(effect, effGetParamLabel, selectedIndex, 0, (void*)unitBuff, 0.0);

					strcat_s(dispBuff, " ");
					strcat_s(dispBuff, unitBuff);
					if (dispBuff[0] == '\0' || dispBuff[0] == ' ') wsprintfA(dispBuff, "%d", int(param * 1000));
					SetWindowTextA(valueText, dispBuff);
				}

			}

			if (sameThread) break;

			if (HIWORD(wParam) == BN_CLICKED && lParam == (LPARAM)dialogState[portNum].checkBoxWnd)
			{
				bool checked = SendMessage(dialogState[portNum].checkBoxWnd, BM_GETCHECK, 0, 0) == BST_CHECKED;
				portState[portNum].alwaysOnTop = checked;
				SetWindowPos(hwnd, checked ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
			}

			else if (HIWORD(wParam) == BN_CLICKED && lParam == (LPARAM)dialogState[portNum].buttonWnd)
			{
				if (!settings_save(effect)) MessageBox(hwnd, _T("Cannot save plugin settings!\r\nMaybe you do not have permission to write plugin's folder \r\nor the plugin has nothing to save."), _T("VST MIDI Driver"), MB_OK | MB_ICONERROR);
				else MessageBox(hwnd, _T("Plugin settings have been saved successfully!"), _T("VST MIDI Driver"), MB_OK | MB_ICONINFORMATION);
			}

			else if (HIWORD(wParam) == BN_CLICKED && lParam == (LPARAM)dialogState[portNum].button2Wnd)
			{
				RECT rect;
				GetWindowRect(hwnd, &rect);
				setEditorPosition(portNum, rect.left, rect.top);

				if (dialogState[portNum].hFont) DeleteObject(dialogState[portNum].hFont);
				portState[portNum].editorHandle = dialogState[portNum].savedHandle;
				EndDialog(hwnd, IDOK);

				portState[portNum].alwaysUseHostEditor = !portState[portNum].alwaysUseHostEditor;
				PostMessage(trayWndHandle, WM_COMMAND, (WPARAM)(PORT_MENU_OFFSET + portNum), 0);
			}
		}
		break;
	case WM_CLOSE:	
		if ((wParam && lParam) || sameThread)
		{
			effect = reinterpret_cast<AEffect*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
			if (effect)
			{
				portNum = *(int*)effect->user;

				RECT rect;
				GetWindowRect(hwnd, &rect);
				setEditorPosition(portNum, rect.left, rect.top);

				if (dialogState[portNum].hFont) DeleteObject(dialogState[portNum].hFont);
				portState[portNum].editorHandle = dialogState[portNum].savedHandle;
				if (portState[portNum].editorHandle != NULL) SendMessageTimeout(portState[portNum].editorHandle, WM_CLOSE, TRUE, TRUE, SMTO_ABORTIFHUNG | SMTO_NORMAL, 1000, NULL);
				EndDialog(hwnd, IDOK);
			}
		}
		else
		{
			ShowWindow(hwnd, SW_HIDE);			
		}

		return TRUE;

		break;
	case WM_DESTROY:
		if (!sameThread) PostQuitMessage(0);
		break;
	}

	return FALSE;
}

INT_PTR CALLBACK EditorProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	AEffect* effect;
	int portNum = 0;

	static bool sameThread = true;
	static float dpiMul = 1.0f;
	static VstInt16 extraHeight = 0;
	static int timerPeriodMS = 30;
	static DialogState dialogState[MAX_PORTS];

	switch (msg)
	{
	case WM_INITDIALOG:
		effect = reinterpret_cast<AEffect*>(lParam);
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
			HDC screen = GetDC(NULL);
			dpiMul = (float)(GetDeviceCaps(screen, LOGPIXELSY)) / 96.0f;
			ReleaseDC(NULL, screen);

			portNum = *(int*)effect->user;

			portState[portNum].isPortActive = true;
			portState[portNum].editorHandle = hwnd;

			SetWindowLongPtr(hwnd, GWLP_USERDATA, lParam);

			
			if (GetCurrentThreadId() != MainThreadId)
			{
				extraHeight = (int)(24 * dpiMul);
				sameThread = false;
			}

			TCHAR wText[20] = { 0 };
			if (sameThread)
			{
				_tcscpy_s(wText, _T("VST Editor"));
			}
			else
			{
#pragma warning(disable:4838) //fake warning even after casting
				TCHAR portSign[] = { _T('A') + static_cast<TCHAR>(portNum) };
#pragma warning(default:4838)
				_tcscpy_s(wText, _T("VST Editor port "));
				_tcsncat_s(wText, portSign, 1);
			}

			SetWindowText(hwnd, wText);			

			SetTimer(hwnd, 1, timerPeriodMS, NULL);
			ERect* eRect = NULL;
			effect->dispatcher(effect, effEditGetRect, 0, 0, &eRect, 0); // dummy call in order S-YXG50 debug panel mode to work...
			effect->dispatcher(effect, effEditOpen, 0, 0, hwnd, 0);
			effect->dispatcher(effect, effSetEditKnobMode, 0, 2, NULL, 0); // Set knob mode to linear(2)				
			effect->dispatcher(effect, effEditGetRect, 0, 0, &eRect, 0);
			if (eRect)
			{
				int width = eRect->right - eRect->left;
				int height = eRect->bottom - eRect->top + extraHeight;
				if (width < (int)(260 * dpiMul))
					width = (int)(260 * dpiMul);
				if (height < (int)(80 * dpiMul))
					height = (int)(80 * dpiMul);
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

					dialogState[portNum].checkBoxWnd = CreateWindowEx(NULL, _T("BUTTON"), _T("Always on Top"), WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX, (int)(5 * dpiMul), eRect->bottom - eRect->top + (int)(3 * dpiMul), (int)(90 * dpiMul), (int)(20 * dpiMul), hwnd, NULL, ((LPCREATESTRUCT)lParam)->hInstance, NULL);

					dialogState[portNum].buttonWnd = CreateWindowEx(NULL, _T("BUTTON"), _T("Save Settings"), WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, width - (int)(90 * dpiMul), eRect->bottom - eRect->top + (int)(2 * dpiMul), (int)(80 * dpiMul), (int)(20 * dpiMul), hwnd, NULL, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
					dialogState[portNum].button2Wnd = CreateWindowEx(NULL, _T("BUTTON"), _T("Switch UI"), WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, (int)(100 * dpiMul), eRect->bottom - eRect->top + (int)(2 * dpiMul), (int)(68 * dpiMul), (int)(20 * dpiMul), hwnd, NULL, ((LPCREATESTRUCT)lParam)->hInstance, NULL);

					GetObject(GetStockObject(DEFAULT_GUI_FONT), sizeof(LOGFONT), &lf);
					dialogState[portNum].hFont = CreateFontIndirect(&lf);
					SendMessage(dialogState[portNum].checkBoxWnd, WM_SETFONT, (WPARAM)dialogState[portNum].hFont, TRUE);
					SendMessage(dialogState[portNum].buttonWnd, WM_SETFONT, (WPARAM)dialogState[portNum].hFont, TRUE);
					SendMessage(dialogState[portNum].button2Wnd, WM_SETFONT, (WPARAM)dialogState[portNum].hFont, TRUE);
				}
				else
				{
					effect->dispatcher(effect, effSetSampleRate, 0, 0, NULL, float(sample_rate));
					effect->dispatcher(effect, effSetBlockSize, 0, (VstIntPtr)(sample_rate * timerPeriodMS * 0.001), NULL, 0);
					sample_pos = 0;
				}
			}
		}		
		return TRUE;
	case WM_SHOWWINDOW:
		effect = reinterpret_cast<AEffect*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
		if (effect && wParam)
		{
			portNum = *(int*)effect->user;
			if(dialogState[portNum].checkBoxWnd) SendMessage(dialogState[portNum].checkBoxWnd, BM_SETCHECK, portState[portNum].alwaysOnTop ? BST_CHECKED : BST_UNCHECKED, 0);
			SetWindowPos(hwnd, portState[portNum].alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
			SetForegroundWindow(hwnd);
		}
		break;
	case WM_SIZE: //Fixes SC-VA display bug after parts section opened/closed		
		effect = reinterpret_cast<AEffect*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
		if (effect && wParam == SIZE_RESTORED)
		{
			ScopeLock<Win32Lock> scopeLock(&dialogLock);

			portNum = *(int*)effect->user;
			ERect* eRect = NULL;
			effect->dispatcher(effect, effEditGetRect, 0, 0, &eRect, 0);
			if (eRect)
			{
				int width = eRect->right - eRect->left;
				int height = eRect->bottom - eRect->top + extraHeight;
				if (width < (int)(260 * dpiMul))
					width = (int)(260 * dpiMul);
				if (height < (int)(80 * dpiMul))
					height = (int)(80 * dpiMul);
				RECT wRect;
				SetRect(&wRect, 0, 0, width, height);
				AdjustWindowRectEx(&wRect, GetWindowLong(hwnd, GWL_STYLE), FALSE, GetWindowLong(hwnd, GWL_EXSTYLE));
				width = wRect.right - wRect.left;
				height = wRect.bottom - wRect.top;
				SetWindowPos(hwnd, HWND_TOP, 0, 0, width, height, SWP_NOMOVE);

				if (!sameThread)
				{
					if (dialogState[portNum].checkBoxWnd)
					{
						SetWindowPos(dialogState[portNum].checkBoxWnd, NULL, (int)(5 * dpiMul), eRect->bottom - eRect->top + (int)(3 * dpiMul), (int)(90 * dpiMul), (int)(20 * dpiMul), SWP_NOZORDER);
						SetWindowPos(hwnd, portState[portNum].alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
					}

					if (dialogState[portNum].buttonWnd)
						SetWindowPos(dialogState[portNum].buttonWnd, NULL, width - (int)(90 * dpiMul), eRect->bottom - eRect->top + (int)(2 * dpiMul), (int)(80 * dpiMul), (int)(20 * dpiMul), SWP_NOZORDER);

					if (dialogState[portNum].button2Wnd)
						SetWindowPos(dialogState[portNum].button2Wnd, NULL, (int)(100 * dpiMul), eRect->bottom - eRect->top + (int)(2 * dpiMul), (int)(68 * dpiMul), (int)(20 * dpiMul), SWP_NOZORDER);
				}
			}
		}
		break;
	case WM_TIMER:
		effect = reinterpret_cast<AEffect*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
		if (effect)
		{
			portNum = *(int*)effect->user;

#pragma warning(disable:6385) //false buffer alarms
#pragma warning(disable:6386) 

			if (sameThread)
			{
				int sampleFrames = (int)(sample_rate * timerPeriodMS * 0.001);
				float** samples = (float**)malloc(sizeof(float**) * effect->numOutputs);
				if (samples)
				{
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
			}

#pragma warning(default:6385)  
#pragma warning(default:6386) 

			if (portState[portNum].editorHandle)
				effect->dispatcher(effect, effEditIdle, 0, 0, NULL, 0);
		}
		break;
	case WM_COMMAND:
		if (wParam == IDCANCEL)
		{
			PostMessage(hwnd, WM_CLOSE, 0, 0);
			return TRUE;
		}

		effect = reinterpret_cast<AEffect*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
		if (effect)
		{
			portNum = *(int*)effect->user;		

			if (sameThread) break;

			if (HIWORD(wParam) == BN_CLICKED && lParam == (LPARAM)dialogState[portNum].checkBoxWnd)
			{
				bool checked = SendMessage(dialogState[portNum].checkBoxWnd, BM_GETCHECK, 0, 0) == BST_CHECKED;
				portState[portNum].alwaysOnTop = checked;
				SetWindowPos(hwnd, checked ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
			}

			else if (HIWORD(wParam) == BN_CLICKED && lParam == (LPARAM)dialogState[portNum].buttonWnd)
			{
				if (!settings_save(effect)) MessageBox(hwnd, _T("Cannot save plugin settings!\r\nMaybe you do not have permission to write plugin's folder \r\nor the plugin has nothing to save."), _T("VST MIDI Driver"), MB_OK | MB_ICONERROR);
				else MessageBox(hwnd, _T("Plugin settings have been saved successfully!"), _T("VST MIDI Driver"), MB_OK | MB_ICONINFORMATION);
			}

			else if (HIWORD(wParam) == BN_CLICKED && lParam == (LPARAM)dialogState[portNum].button2Wnd)
			{
				BOOL IsYamahaWritten = false;
				if (isYamahaPlugin)
				{
					TCHAR caption[32] = {};
					HWND firstChild = GetWindow(hwnd, GW_CHILD);
					GetWindowText(firstChild, caption, _countof(caption));
					BOOL isInDebugMode = (BOOL)_tcscmp(caption, _T("YAMAHASyxgVstWindow"));

					TCHAR vst_path[MAX_PATH] = { 0 };
					_tcscpy_s(vst_path, argv[1]);
					TCHAR* chrP = _tcsrchr(vst_path, '.'); // removes extension
					if (chrP) chrP[0] = 0;
					_tcscat_s(vst_path, _T(".ini"));

					//BOOL isDebugPanel = (BOOL)GetPrivateProfileInt(_T("SYXG50"), _T("DebugPanel"), 0, vst_path);

					if (isInDebugMode)
						IsYamahaWritten = WritePrivateProfileString(_T("SYXG50"), _T("DebugPanel"), _T("0"), vst_path);
					else
						IsYamahaWritten = WritePrivateProfileString(_T("SYXG50"), _T("DebugPanel"), _T("1"), vst_path);

					if (IsYamahaWritten)
					{
						RECT rect;
						GetWindowRect(hwnd, &rect);
						setEditorPosition(portNum, rect.left, rect.top);

						KillTimer(hwnd, 1);
						if (effect)
						{
							effect->dispatcher(effect, effEditClose, 0, 0, NULL, 0);
							portState[portNum].editorHandle = NULL;
						}

						if (dialogState[portNum].hFont) DeleteObject(dialogState[portNum].hFont);
						EndDialog(hwnd, IDOK);

						PostMessage(trayWndHandle, WM_COMMAND, (WPARAM)(PORT_MENU_OFFSET + portNum), TRUE);
					}
				}

				if (!IsYamahaWritten)
				{
					ShowWindow(hwnd, SW_HIDE);

					portState[portNum].alwaysUseHostEditor = !portState[portNum].alwaysUseHostEditor;
					PostMessage(trayWndHandle, WM_COMMAND, (WPARAM)(PORT_MENU_OFFSET + portNum), TRUE);
				}

			}
		}
		break;
	case WM_CLOSE:
		if ((wParam && lParam) || sameThread) //because of JUCE framework editors prevent real closing except when driver quits. 
		{
			effect = reinterpret_cast<AEffect*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
			if (effect)
			{
				portNum = *(int*)effect->user;

				RECT rect;
				GetWindowRect(hwnd, &rect);
				setEditorPosition(portNum, rect.left, rect.top);

				KillTimer(hwnd, 1);
				portState[portNum].editorHandle = NULL;
				effect->dispatcher(effect, effEditClose, 0, 0, NULL, 0);

				if (dialogState[portNum].hFont) DeleteObject(dialogState[portNum].hFont);
				EndDialog(hwnd, IDOK);
			}
		}
		else
		{
			ShowWindow(hwnd, SW_HIDE);
		}

		return TRUE;

		break;
	case WM_DESTROY:
		if (!sameThread) PostQuitMessage(0);
		break;
	}

	return FALSE;
}

#pragma comment(lib, "Winmm")
static void EditorThread(void* threadparam)
{
	MyDLGTEMPLATE vstiEditor;
	AEffect* peffect = static_cast<AEffect*>(threadparam);
	int portNum = *(int*)peffect->user;
	vstiEditor.style = WS_POPUPWINDOW | WS_DLGFRAME | WS_MINIMIZEBOX | WS_SYSMENU | WS_CAPTION | DS_MODALFRAME | DS_CENTER;

	if (highDpiMode && SetThreadDpiAwarenessContext) SetThreadDpiAwarenessContext(highDpiMode);

	HRESULT res = CoInitialize(NULL);

	if ((peffect->flags & effFlagsHasEditor) && !portState[portNum].alwaysUseHostEditor)
		DialogBoxIndirectParam(NULL, &vstiEditor, NULL, (DLGPROC)EditorProc, (LPARAM)peffect);
	else
		DialogBoxParam(NULL, MAKEINTRESOURCE(IDD_GENERALVSTUI), NULL, (DLGPROC)GeneralUiProc, (LPARAM)peffect);

	if (res == S_OK) CoUninitialize();

	_endthread();
}

void showVstEditor(uint32_t portnum, BOOL forceNew)
{
	if (portState[portnum].editorHandle && !forceNew)
	{
		if (!IsWindowVisible(portState[portnum].editorHandle) || IsIconic(portState[portnum].editorHandle))
		{
			ShowWindow(portState[portnum].editorHandle, SW_SHOWNORMAL);
			SetForegroundWindow(portState[portnum].editorHandle);
		}
		else
			ShowWindow(portState[portnum].editorHandle, SW_HIDE); //JUCE framework editors have problems when really closed and re-opened.

	}
	else
	{
		_beginthread(EditorThread, 16384, portState[portnum].pEffect);
	}
}

LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	const int STILLRUNNING = 255;	
	static TCHAR midiClient[64] = { 0 };
	static TCHAR clientBitnessStr[8] = { 0 };
	static TCHAR outputModeStr[8] = { 0 };

#ifdef WIN64
	static const TCHAR bitnessStr[8] = _T(" 64-bit");
#else
	static const TCHAR bitnessStr[8] = _T(" 32-bit");
#endif		

	switch (msg)
	{
		case WM_CREATE:
		{
			TCHAR tmpPath[MAX_PATH] = { 0 };
			TCHAR trayTip[MAX_PATH] = { 0 };
			HMENU pluginMenu = CreatePopupMenu();

			_tcsncpy_s(midiClient, argv[3], _countof(midiClient));
			_tcsncpy_s(clientBitnessStr, argv[4], _countof(clientBitnessStr));
			_tcsncpy_s(outputModeStr, !_tcscmp(argv[5], _T("S")) ? _T("WASAPI") : !_tcscmp(argv[5], _T("A")) ? _T("ASIO") : _T("WaveOut"), _countof(outputModeStr));

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
			AppendMenu(trayMenu, MF_POPUP, (UINT_PTR)pluginMenu, _T("Switch Plugin"));
			AppendMenu(trayMenu, MF_SEPARATOR, 0, _T(""));
			AppendMenu(trayMenu, MF_STRING | MF_ENABLED, WAVEWRITE_START_STOP, _T("Start Wave Recording"));
			AppendMenu(trayMenu, MF_SEPARATOR, 0, _T(""));
			AppendMenu(trayMenu, MF_STRING | MF_ENABLED, RESET_MENU_OFFSET + 1, _T("Send GM Reset"));
			AppendMenu(trayMenu, MF_STRING | MF_ENABLED, RESET_MENU_OFFSET + 2, _T("Send GS Reset"));
			AppendMenu(trayMenu, MF_STRING | MF_ENABLED, RESET_MENU_OFFSET + 3, _T("Send XG Reset"));
			AppendMenu(trayMenu, MF_STRING | MF_ENABLED, RESET_MENU_OFFSET + 4, _T("Send GM2 Reset"));
			AppendMenu(trayMenu, MF_STRING | MF_ENABLED, RESET_MENU_OFFSET + 5, _T("All Notes/CC Off"));
			AppendMenu(trayMenu, MF_SEPARATOR, 0, _T(""));
			AppendMenu(trayMenu, MF_STRING | MF_ENABLED, SHOW_INFO, _T("Info..."));


			nIconData.cbSize = sizeof(NOTIFYICONDATA);
			nIconData.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
			nIconData.hWnd = hwnd;
			nIconData.uID = WM_ICONMSG;
			nIconData.uCallbackMessage = WM_ICONMSG;
			nIconData.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(32512));

			if (IsWinNT4())
				_tcscpy_s(trayTip, _T("VST Midi Synth - "));
			else
				_tcscpy_s(trayTip, _T("VST Midi Synth \r\n"));

			_tcsncat_s(trayTip, argv[3], _countof(trayTip) - _tcslen(trayTip));

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
				//bool hasEditor = portState[0].pEffect->flags & VstAEffectFlags::effFlagsHasEditor;
				GetCursorPos(&cursorPoint);
				CheckMenuItem(trayMenu, PORT_MENU_OFFSET, portState[0].editorHandle != NULL && IsWindowVisible(portState[0].editorHandle) ? MF_CHECKED : MF_UNCHECKED);
				CheckMenuItem(trayMenu, PORT_MENU_OFFSET + 1, portState[1].editorHandle != NULL && IsWindowVisible(portState[1].editorHandle) ? MF_CHECKED : MF_UNCHECKED);
				EnableMenuItem(trayMenu, PORT_MENU_OFFSET, portState[0].isPortActive /* && hasEditor */ ? MF_ENABLED : MF_GRAYED);
				EnableMenuItem(trayMenu, PORT_MENU_OFFSET + 1, portState[1].isPortActive /* && hasEditor*/ ? MF_ENABLED : MF_GRAYED);
				EnableMenuItem(trayMenu, 3, (portState[0].isPortActive || portState[1].isPortActive) && !waveWriter.getIsRecordingStarted() ? MF_BYPOSITION | MF_ENABLED : MF_BYPOSITION | MF_GRAYED);
				EnableMenuItem(trayMenu, WAVEWRITE_START_STOP, portState[0].isPortActive || portState[1].isPortActive || waveWriter.getIsRecordingStarted() ? MF_ENABLED : MF_GRAYED);

				CheckMenuItem(trayMenu, RESET_MENU_OFFSET + 1, lastUsedSysEx == RESET_MENU_OFFSET + 1 ? MF_CHECKED : MF_UNCHECKED);
				CheckMenuItem(trayMenu, RESET_MENU_OFFSET + 2, lastUsedSysEx == RESET_MENU_OFFSET + 2 ? MF_CHECKED : MF_UNCHECKED);
				CheckMenuItem(trayMenu, RESET_MENU_OFFSET + 3, lastUsedSysEx == RESET_MENU_OFFSET + 3 ? MF_CHECKED : MF_UNCHECKED);
				CheckMenuItem(trayMenu, RESET_MENU_OFFSET + 4, lastUsedSysEx == RESET_MENU_OFFSET + 4 ? MF_CHECKED : MF_UNCHECKED);
				CheckMenuItem(trayMenu, RESET_MENU_OFFSET + 5, lastUsedSysEx == RESET_MENU_OFFSET + 5 ? MF_CHECKED : MF_UNCHECKED);

				SetForegroundWindow(hwnd);
				TrackPopupMenu(trayMenu, TPM_RIGHTBUTTON | (GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN), cursorPoint.x, cursorPoint.y, 0, hwnd, NULL);
				PostMessage(hwnd, WM_NULL, 0, 0);
				return 0;
			}		
		}
		break;
		case WM_HELP:
		{
			TCHAR tmpPath[MAX_PATH] = { 0 };

			if (GetWindowsDirectory(tmpPath, MAX_PATH))
			{
				_tcscat_s(tmpPath, _T("\\SysWOW64\\vstmididrv\\Help\\Readme.html"));
				if (GetFileAttributes(tmpPath) == INVALID_FILE_ATTRIBUTES)
				{
					if (GetWindowsDirectory(tmpPath, MAX_PATH))
						_tcscat_s(tmpPath, _T("\\System32\\vstmididrv\\Help\\Readme.html"));
				}

				ShellExecute(hwnd, NULL, tmpPath, NULL, NULL, SW_SHOWNORMAL);
				return 0;
			}
		}
		break;
		case WM_COMMAND:
		{
			static volatile int aboutBoxResult = 0;
			TCHAR tempBuff[MAX_PATH] = { 0 };
			TCHAR versionBuff[MAX_PATH * 2] = _T("MIDI client: ");
			MSGBOXPARAMS params = { 0 };

			if ((int)wParam >= 0 && wParam < MAX_PLUGINS)
			{
				setSelectedPluginIndex((int)wParam);
				return 0;
			}

			switch (wParam)
			{
			case PORT_MENU_OFFSET:
			case PORT_MENU_OFFSET + 1:
				showVstEditor((uint32_t)wParam - PORT_MENU_OFFSET, (BOOL)lParam);
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

			case WAVEWRITE_START_STOP:
			{
				bool isRecStarted;
				if (!waveWriter.getIsRecordingStarted())
				{
					uint32_t response = waveWriter.Init(is4channelMode ? 4 : 2, sample_rate, portState[0].editorHandle ? portState[0].editorHandle : portState[1].editorHandle ? portState[1].editorHandle : hwnd, hwnd, WAVEWRITE_ERROR);
					isRecStarted = response == WaveResponse::Success;
					if (response == WaveResponse::CannotCreate)
					{
						MessageBox(hwnd, _T("Wave file cannot be created.\r\nCheck write permission."), _T("VST Midi Driver"), MB_OK | MB_SYSTEMMODAL | MB_ICONERROR);
					}
				}
				else
				{
					waveWriter.Close();
					isRecStarted = false;
				}

				ModifyMenu(trayMenu, (UINT)wParam, MF_STRING, wParam, isRecStarted ? _T("Stop Wave Recording") : _T("Start Wave Recording"));
				nIconData.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(isRecStarted ? 32513 : 32512));
				Shell_NotifyIcon(NIM_MODIFY, &nIconData);
				return 0;
			}

			case WAVEWRITE_ERROR:
				if (waveWriter.getIsRecordingStarted())
				{					
					waveWriter.Close();

					ModifyMenu(trayMenu, WAVEWRITE_START_STOP, MF_STRING, WAVEWRITE_START_STOP, _T("Start Wave Recording"));
					nIconData.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(32512));
					Shell_NotifyIcon(NIM_MODIFY, &nIconData);

					MessageBox(hwnd, _T("Wave file cannot be written or maximum size has been reached.\r\nWave recording is stopped."), _T("VST Midi Driver"), MB_OK | MB_SYSTEMMODAL | MB_ICONERROR);
				}
				return 0;

			case SHOW_INFO:
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
				_tcscat_s(versionBuff, _T(" "));
				
				_itot_s(sample_rate, tempBuff, 10);
				_tcscat_s(versionBuff, tempBuff);
				_tcscat_s(versionBuff, _T(" Hz"));
							
				_tcscat_s(versionBuff, _T("\r\n \r\n"));

				_tcscat_s(versionBuff, _T("Synth driver "));
				GetSystemDirectory(tempBuff, MAX_PATH);
				_tcscat_s(tempBuff, _T("\\vstmididrv.dll"));
				GetRunTimeFileVersion(tempBuff, versionBuff, _countof(versionBuff));

				_tcscat_s(versionBuff, _T("\r\nHost bridge "));
				GetRunTimeFileVersion(NULL, versionBuff, _countof(versionBuff));

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
	}

	return DefWindowProc(hwnd, msg, wParam, lParam);
}

static void TrayThread(void* threadparam)
{
	MSG message;
	WNDCLASS windowClass = { 0 };
	windowClass.hInstance = GetModuleHandle(NULL);
	windowClass.lpfnWndProc = TrayWndProc;
	windowClass.lpszClassName = _T("VSTHostUtilWindow");

	if (SetThreadDpiAwarenessContext) SetThreadDpiAwarenessContext((HANDLE)-2); //System aware

	RegisterClass(&windowClass);
	trayWndHandle = CreateWindowEx(WS_EX_TOOLWINDOW, windowClass.lpszClassName, _T("VSTTray"), WS_POPUP, 0, 0, 0, 0, NULL, NULL, GetModuleHandle(NULL), NULL);

	//ShowWindow(trayWndHandle, SW_SHOWNORMAL);

	while (GetMessage(&message, NULL, 0, 0)) {
		TranslateMessage(&message);
		DispatchMessage(&message);
	}

	_endthread();
}

#pragma endregion VST dialogs/tray window related callbacks and functions.

#pragma region AudioMaster_Callback
struct audioMasterData
{
	VstIntPtr effect_number;
};

static VstIntPtr VSTCALLBACK audioMaster(AEffect* effect, VstInt32 opcode, VstInt32 index, VstIntPtr value, void* ptr, float opt)
{
	static VstTimeInfo vstTimeInfo = { 0 };

	switch (opcode)
	{
	case audioMasterVersion:
		return 2400;

	case audioMasterCurrentId:
	{
		audioMasterData* data = NULL;
		if (effect) data = static_cast<audioMasterData*>(effect->user);
		if (data) return data->effect_number;
		break;
	}

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
#pragma endregion Main VST audioMaster callback

#pragma region WinMain_Init
int CALLBACK _tWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nShowCmd)
{
#ifdef _DEBUG
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
	_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_WNDW);
	_CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_WNDW);
#endif

	_set_invalid_parameter_handler(InvalidParamHandler);

	int argc = __argc;

#ifdef UNICODE 
	//argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	argv = __wargv;
#else
	argv = __argv;
#endif		

	if (argv == NULL || argc != 6) return Error::InvalidCommandLineArguments;

	TCHAR* end_char = NULL;
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

	HMODULE hDll = NULL;
	main_func pMain = NULL;
	bool isSinglePort32Ch = false;	
	std::vector<uint8_t> chunk;	

	MainThreadId = GetCurrentThreadId();

	HINSTANCE user32 = GetModuleHandle(_T("user32.dll"));
	if (user32)
	{
		LOADUSER32FUNCTION(SetThreadDpiAwarenessContext);
		*((void**)&DynAllowSetForegroundWindow) = GetProcAddress(user32, "AllowSetForegroundWindow");
	}	

	HINSTANCE kernel32 = GetModuleHandle(_T("kernel32.dll"));
#ifdef UNICODE  
	if (kernel32) *((void**)&DynGetModuleHandleEx) = GetProcAddress(kernel32, "GetModuleHandleExW"); //unfortunately in case of newer VS versions GetModuleHandleExW is defined unconditionally in libloaderapi.h despite it is available only from WinXP...
#else
	if (kernel32) *((void**)&DynGetModuleHandleEx) = GetProcAddress(kernel32, "GetModuleHandleExA"); //unfortunately in case of newer VS versions GetModuleHandleExW is defined unconditionally in libloaderapi.h despite it is available only from WinXP...
#endif

	HANDLE null_file = CreateFile(_T("NUL"), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	pipe_in = GetStdHandle(STD_INPUT_HANDLE);
	pipe_out = GetStdHandle(STD_OUTPUT_HANDLE);

	SetStdHandle(STD_INPUT_HANDLE, null_file);
	SetStdHandle(STD_OUTPUT_HANDLE, null_file);

	INITCOMMONCONTROLSEX icc = { 0 };
	icc.dwSize = sizeof(icc);
	icc.dwICC = ICC_WIN95_CLASSES;
	if (!InitCommonControlsEx(&icc)) InitCommonControls();
	//InitCommonControlsEx can fail on Win 2000/XP depending on service packs. It's rude to exit in case of failing since this is not essentiall at all.

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

	portState[0].pEffect = pMain(&audioMaster);

	if (!portState[0].pEffect || portState[0].pEffect->magic != kEffectMagic)
	{
		code = Response::NotAVsti;
		goto exit;
	}

	portState[0].effectData = 0;
	portState[0].pEffect->user = &portState[0].effectData;

	portState[0].pEffect->dispatcher(portState[0].pEffect, effOpen, 0, 0, NULL, 0);

	if (portState[0].pEffect->dispatcher(portState[0].pEffect, effGetPlugCategory, 0, 0, NULL, 0) != kPlugCategSynth ||
		portState[0].pEffect->dispatcher(portState[0].pEffect, effCanDo, 0, 0, "receiveVstMidiEvent", 0) < 1)
	{
		code = Response::VstiIsNotAMidiSynth;
		goto exit;
	}

	portState[1].pEffect = pMain(&audioMaster);
	if (!portState[1].pEffect)
	{
		code = Response::NotAVsti;
		goto exit;
	}

	portState[1].effectData = 1;
	portState[1].pEffect->user = &portState[1].effectData;

	portState[1].pEffect->dispatcher(portState[1].pEffect, effOpen, 0, 0, NULL, 0);

	char name_string[256] = { 0 };
	char vendor_string[256] = { 0 };
	uint32_t name_string_length;
	uint32_t vendor_string_length;
	uint32_t product_string_length;
	uint32_t vendor_version;
	uint32_t unique_id;

	portState[0].pEffect->dispatcher(portState[0].pEffect, effGetEffectName, 0, 0, &name_string, 0);
	portState[0].pEffect->dispatcher(portState[0].pEffect, effGetVendorString, 0, 0, &vendor_string, 0);
	portState[0].pEffect->dispatcher(portState[0].pEffect, effGetProductString, 0, 0, &product_string, 0);

	name_string_length = (uint32_t)strlen(name_string);
	vendor_string_length = (uint32_t)strlen(vendor_string);
	product_string_length = (uint32_t)strlen(product_string);
	vendor_version = (uint32_t)portState[0].pEffect->dispatcher(portState[0].pEffect, effGetVendorVersion, 0, 0, NULL, 0);
	unique_id = portState[0].pEffect->uniqueID;
	isYamahaPlugin = (unique_id == (uint32_t)'xg50') || (unique_id == (uint32_t)'YMF7') || (unique_id == (uint32_t)'S-TY');

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
	put_code(MAX_OUTPUTS);
	if (name_string_length) put_bytes(name_string, name_string_length);
	if (vendor_string_length) put_bytes(vendor_string, vendor_string_length);
	if (product_string_length) put_bytes(product_string, product_string_length);
	
#pragma endregion WinMain initialization 

#pragma region Message_Loop	
	for (;;)
	{
		uint32_t command = get_code();
		if (!command) break;

#ifdef LOG
		Log(_T("Processing command %d\n"), (int)command);
#endif

		switch (command)
		{
		case Command::GetChunkData: // Get Chunk
		{
			getChunk(portState[0].pEffect, chunk);
			put_code(Response::NoError);
			put_code((uint32_t)chunk.size());
			if (chunk.size()) put_bytes(&chunk.front(), (uint32_t)chunk.size());
		}
		break;

		case Command::SetChunkData: // Set Chunk
		{
			uint32_t size = get_code();
			chunk.resize(size);
			if (size) get_bytes(&chunk.front(), size);
			setChunk(portState[0].pEffect, chunk);
			setChunk(portState[1].pEffect, chunk);

			put_code(Response::NoError);
		}
		break;

		/*
		case Command::HasEditor: // Has Editor
			{
				uint32_t has_editor = (portState[0].pEffect->flags & effFlagsHasEditor) ? 1 : 0;

				put_code(Response::NoError);
				put_code(has_editor);
			}
			break;
		*/

		case Command::DisplayEditorModal: // Display Editor Modal
		{
			//if (portState[0].pEffect->flags & effFlagsHasEditor)
			{
				MyDLGTEMPLATE t;
				t.style = WS_POPUPWINDOW | WS_DLGFRAME | DS_MODALFRAME | DS_CENTER;
				//t.dwExtendedStyle = WS_EX_TOOLWINDOW;

				if (highDpiMode && SetThreadDpiAwarenessContext) SetThreadDpiAwarenessContext(highDpiMode);

				if (portState[0].pEffect->flags & effFlagsHasEditor)
					DialogBoxIndirectParam(NULL, &t, NULL, (DLGPROC)EditorProc, (LPARAM)(portState[0].pEffect));
				else
					DialogBoxParam(NULL, MAKEINTRESOURCE(IDD_GENERALVSTUI), NULL, (DLGPROC)GeneralUiProc, (LPARAM)portState[0].pEffect);

				//getChunk(portState[0].pEffect, chunk);
				//setChunk(portState[1].pEffect, chunk);

				if (DynAllowSetForegroundWindow) DynAllowSetForegroundWindow(ASFW_ANY); //allows drivercfg to get back focus 

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
			if (trayWndHandle)
				PostMessage(trayWndHandle, WM_COMMAND, (WPARAM)PORT_MENU_OFFSET + port_num, 0);
		}
		break;

		case Command::InitSysTray:
		{
			_beginthread(TrayThread, 16384, NULL);

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
			highDpiMode = (HANDLE)(int)get_code(); //requires sign extension on 64-bit
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
				if (portState[0].editorHandle != NULL) ShowWindow(portState[0].editorHandle, SW_HIDE);
				if (portState[1].editorHandle != NULL) ShowWindow(portState[1].editorHandle, SW_HIDE);
				portState[0].isPortActive = false;
				portState[1].isPortActive = false;

				if (portState[0].pEffect)
				{
					if (blState.size()) portState[0].pEffect->dispatcher(portState[0].pEffect, effStopProcess, 0, 0, NULL, 0);
				}
				if (portState[1].pEffect)
				{
					if (blState.size()) portState[1].pEffect->dispatcher(portState[1].pEffect, effStopProcess, 0, 0, NULL, 0);
				}
			}
			else
			{
				if (portState[port_num].editorHandle != NULL) ShowWindow(portState[port_num].editorHandle, SW_HIDE);
				portState[port_num].isPortActive = false;

				if (portState[port_num].pEffect)
				{
					if (blState.size()) portState[port_num].pEffect->dispatcher(portState[port_num].pEffect, effStopProcess, 0, 0, NULL, 0);
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
			portState[port].isPortActive = true;
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
			inputEventList.position++;
			inputEventList.position &= (MAX_INPUTEVENT_COUNT - 1);

			memset(ev, 0, sizeof(InputEventList::InputEvent));

			uint32_t size = get_code();

			uint32_t port = (size >> 24) & 1;
			portState[port].isPortActive = true;
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
			is4channelMode = false;
			renderAudiosamples(2);			
			resetInputEvents();
			break;
		}

		case Command::RenderAudioSamples4channel: // Render Samples for 4 channel mode
		{
			is4channelMode = true;
			renderAudiosamples(4);
			resetInputEvents();
			break;
		}

		default:
			code = Response::CommandUnknown;
			goto exit;
			break;
		}

#ifdef LOG
		Log(_T("Command %d done\n"), (int)command);
#endif
	}

#pragma endregion Main message handling loop

#pragma region WinMain_Finish
	exit :
	
	waveWriter.Close();

	if (portState[0].editorHandle != NULL) SendMessageTimeout(portState[0].editorHandle, WM_CLOSE, TRUE, TRUE, SMTO_ABORTIFHUNG | SMTO_NORMAL, 1000, NULL);
	if (portState[1].editorHandle != NULL) SendMessageTimeout(portState[1].editorHandle, WM_CLOSE, TRUE, TRUE, SMTO_ABORTIFHUNG | SMTO_NORMAL, 1000, NULL);
	if (trayWndHandle != NULL) SendMessageTimeout(trayWndHandle, WM_CLOSE, 0, 0, SMTO_ABORTIFHUNG | SMTO_NORMAL, 1000, NULL);

	if (portState[1].pEffect)
	{
		if (blState.size()) portState[1].pEffect->dispatcher(portState[1].pEffect, effStopProcess, 0, 0, NULL, 0);
		portState[1].pEffect->dispatcher(portState[1].pEffect, effClose, 0, 0, NULL, 0);
		portState[1].pEffect = NULL;
	}
	if (portState[0].pEffect)
	{
		if (blState.size()) portState[0].pEffect->dispatcher(portState[0].pEffect, effStopProcess, 0, 0, NULL, 0);
		portState[0].pEffect->dispatcher(portState[0].pEffect, effClose, 0, 0, NULL, 0);
		portState[0].pEffect = NULL;
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

#ifdef LOG
	Log(_T("Exit with code %d\n"), code);
#endif

	return code;
}
#pragma endregion WinMain finalization

