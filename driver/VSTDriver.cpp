/* Copyright (C) 2011 Chris Moeller, Brad Miller
*  Copyright (C) 2023 Zoltan Bacsko - Falcosoft
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

#include "VSTDriver.h"
#include <commdlg.h>
#include <assert.h>
#include <new>
#include "../common/LegacySafety.h"

#pragma warning(disable:6255) 
#pragma warning(disable:26812)

enum
{
	BUFFER_SIZE = 3840
};

extern "C" { extern HINSTANCE hinst_vst_driver; };

UINT GetWaveOutDeviceId() {
	HKEY hKey;
	if (RegOpenKeyEx(HKEY_CURRENT_USER, _T("Software\\VSTi Driver\\Output Driver"), 0, KEY_READ, &hKey) != ERROR_SUCCESS)
		return WAVE_MAPPER;
	TCHAR value[MAX_PATH] = { 0 };
	bool valid = VSTDriverReadRegistryString(hKey, _T("WinMM WaveOut"), value, _countof(value));
	RegCloseKey(hKey);
	if (valid) {
		WAVEOUTCAPS caps;
		for (int id = -1; waveOutGetDevCaps(id, &caps, sizeof(caps)) == MMSYSERR_NOERROR; ++id)
			if (!_tcscmp(value, caps.szPname)) return id;
	}
	return WAVE_MAPPER;
}
#pragma warning(disable:28159)
bool IsVistaOrNewer()
{
	OSVERSIONINFOEX osvi = { 0 };
	BOOL bOsVersionInfoEx;
	osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEX);
	bOsVersionInfoEx = GetVersionEx((OSVERSIONINFO*)&osvi);
	if (bOsVersionInfoEx == FALSE) return false;
	return (VER_PLATFORM_WIN32_NT == osvi.dwPlatformId && osvi.dwMajorVersion > 5);
}
#pragma warning(default:28159)

bool UseAsio()
{
	HKEY hKey;
	if (RegOpenKeyEx(HKEY_CURRENT_USER, _T("Software\\VSTi Driver\\Output Driver"), 0, KEY_READ, &hKey) != ERROR_SUCCESS)
		return false;
	TCHAR value[MAX_PATH] = { 0 };
	bool valid = VSTDriverReadRegistryString(hKey, _T("Driver Mode"), value, _countof(value));
	RegCloseKey(hKey);
	return valid && !_tcscmp(value, _T("Bass ASIO"));
}

bool UseWasapi()
{
	HKEY hKey;
	if (RegOpenKeyEx(HKEY_CURRENT_USER, _T("Software\\VSTi Driver\\Output Driver"), 0, KEY_READ, &hKey) != ERROR_SUCCESS)
		return false;
	TCHAR value[MAX_PATH] = { 0 };
#ifdef WIN64
	LPCTSTR name = _T("Bass ASIO x64");
	LPCTSTR fallbackName = _T("Bass ASIO");
#else
	LPCTSTR name = _T("Bass ASIO");
	LPCTSTR fallbackName = _T("Bass ASIO x64");
#endif
	bool valid = VSTDriverReadRegistryString(hKey, name, value, _countof(value));
	// Match the ASIO driver selection fallback when this architecture has no selection.
	if (!valid || !value[0])
		valid = VSTDriverReadRegistryString(hKey, fallbackName, value, _countof(value));
	RegCloseKey(hKey);
	return valid && _tcsstr(value, _T("VSTDriver-ASIO2WASAPI")) != NULL;
}

// Reused for serialized pipe transfers; no allocation is needed per MIDI/audio request.
// Both the OVERLAPPED and its buffer must survive a cancellation timeout.
struct VSTDriverPipeIO {
	OVERLAPPED overlapped;
	BYTE buffer[65536];
};

// Keep timeout recovery separate from a worker delayed outside bridge I/O.
class VSTDriverIOActivity {
	volatile LONG* pending;
public:
	explicit VSTDriverIOActivity(volatile LONG* state) : pending(state) {
		InterlockedExchange(pending, 1);
	}
	~VSTDriverIOActivity() {
		InterlockedExchange(pending, 0);
	}
};

VSTDriver::VSTDriver() :
	szPluginPath(),
	bInitialized(),
	bInitializedOtherModel(),
	hProcess(),
	childProcessId(),
	hThread(),
	hReadEvent(),
	hWriteEvent(),
	hCancelEvent(),
	pipeIO(),
	pendingIO(),
	ioTimeout(INFINITE),
	hChildStd_IN_Rd(),
	hChildStd_IN_Wr(),
	hChildStd_OUT_Rd(),
	hChildStd_OUT_Wr(),
	uNumOutputs(),
	sName(),
	sVendor(),
	sProduct(),
	IsSCVA(),
	uPluginPlatform(),
	uVendorVersion(),
	uUniqueId()
{
	editorOwner = (HWND)-1;
	hCancelEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
}

VSTDriver::~VSTDriver() {
	CloseVSTDriver();
	delete pipeIO;
	if (hCancelEvent) CloseHandle(hCancelEvent);
	delete[] sName;
	delete[] sVendor;
	delete[] sProduct;
}

static WORD getwordle(BYTE* pData)
{
	return (WORD)(pData[0] | (((WORD)pData[1]) << 8));
}

static DWORD getdwordle(BYTE* pData)
{
	return pData[0] | (((DWORD)pData[1]) << 8) | (((DWORD)pData[2]) << 16) | (((DWORD)pData[3]) << 24);
}

unsigned VSTDriver::test_plugin_platform() {
#define iMZHeaderSize (0x40)
#define iPEHeaderSize (4 + 20 + 224)

	BYTE peheader[iPEHeaderSize];
	DWORD dwOffsetPE;

	FILE* f = _tfopen(szPluginPath, _T("rb"));
	if (!f) goto error;
	if (fread(peheader, 1, iMZHeaderSize, f) < iMZHeaderSize) goto error;
	if (getwordle(peheader) != 0x5A4D) goto error;
	dwOffsetPE = getdwordle(peheader + 0x3c);
	if (fseek(f, dwOffsetPE, SEEK_SET) != 0) goto error;
	if (fread(peheader, 1, iPEHeaderSize, f) < iPEHeaderSize) goto error;
	fclose(f); f = NULL;
	if (getdwordle(peheader) != 0x00004550) goto error;
	switch (getwordle(peheader + 4)) {
	case 0x014C: return 32;
	case 0x8664: return 64;
	}

error:
	if (f) fclose(f);
	return 0;
}

void VSTDriver::load_settings(TCHAR* szPath) {
	blChunk.clear();
	uPluginPlatform = 0;
	TCHAR path[MAX_PATH] = { 0 };
	if (szPath) {
		if (_tcslen(szPath) >= _countof(path)) return;
		_tcscpy_s(path, szPath);
	}
	else {
		HKEY key;
		if (RegOpenKeyEx(HKEY_CURRENT_USER, _T("Software\\VSTi Driver"), 0, KEY_READ, &key) != ERROR_SUCCESS) return;
		DWORD index = 0, bytes = sizeof(index), type = 0;
		if (RegQueryValueEx(key, _T("SelectedPlugin"), NULL, &type, (LPBYTE)&index, &bytes) != ERROR_SUCCESS ||
			type != REG_DWORD || bytes != sizeof(index) || index > 9) index = 0;
		TCHAR valueName[20] = _T("plugin");
		if (index) {
			TCHAR suffix[12] = { 0 };
			_ultot_s(index, suffix, _countof(suffix), 10);
			_tcscat_s(valueName, suffix);
		}
		bool valid = VSTDriverReadRegistryString(key, valueName, path, _countof(path));
		RegCloseKey(key);
		if (!valid) return;
	}
	if (!path[0]) return;
	size_t chars = _tcslen(path) + 1;
	szPluginPath = (TCHAR*)calloc(chars, sizeof(TCHAR));
	if (!szPluginPath) return;
	memcpy(szPluginPath, path, chars * sizeof(TCHAR));
	uPluginPlatform = test_plugin_platform();

	// Replace only the filename extension, not a dot in a directory name.
	tstring settings(path);
	size_t slash = settings.find_last_of(_T("\\/"));
	size_t dot = settings.find_last_of(_T('.'));
	if (dot != tstring::npos && (slash == tstring::npos || dot > slash)) settings.resize(dot);
	settings += _T(".set");
	FILE* file = NULL;
	if (_tfopen_s(&file, settings.c_str(), _T("rb")) || !file) return;
	bool valid = fseek(file, 0, SEEK_END) == 0;
	long length = valid ? ftell(file) : -1;
	valid = length >= 0 && (unsigned long)length <= VSTDriverMaxChunkBytes && fseek(file, 0, SEEK_SET) == 0;
	if (valid) valid = VSTDriverResizeChunk(blChunk, (size_t)length);
	if (valid && length) valid = fread(&blChunk.front(), 1, (size_t)length, file) == (size_t)length;
	fclose(file);
	if (!valid) {
		blChunk.clear();
		OutputDebugString(_T("VST MIDI Driver: invalid or unreadable settings file ignored\n"));
	}
}

static inline char print_hex_digit(unsigned val)
{
	static const char table[16] = { '0','1','2','3','4','5','6','7','8','9','A','B','C','D','E','F' };
	assert((val & ~0xF) == 0);
	return table[val];
}

static void print_hex(unsigned val, tstring& out, unsigned bytes)
{
	unsigned n;
	for (n = 0; n < bytes; n++)
	{
		unsigned char c = (unsigned char)((val >> ((bytes - 1 - n) << 3)) & 0xFF);
		out += print_hex_digit(c >> 4);
		out += print_hex_digit(c & 0xF);
	}
}

static void print_guid(const GUID& p_guid, tstring& out)
{
	print_hex(p_guid.Data1, out, 4);
	out += '-';
	print_hex(p_guid.Data2, out, 2);
	out += '-';
	print_hex(p_guid.Data3, out, 2);
	out += '-';
	print_hex(p_guid.Data4[0], out, 1);
	print_hex(p_guid.Data4[1], out, 1);
	out += '-';
	print_hex(p_guid.Data4[2], out, 1);
	print_hex(p_guid.Data4[3], out, 1);
	print_hex(p_guid.Data4[4], out, 1);
	print_hex(p_guid.Data4[5], out, 1);
	print_hex(p_guid.Data4[6], out, 1);
	print_hex(p_guid.Data4[7], out, 1);
}

static bool create_pipe_name(tstring& out)
{
	GUID guid;
	if (FAILED(CoCreateGuid(&guid))) return false;

	out = _T("\\\\.\\pipe\\");
	print_guid(guid, out);

	return true;
}

/*
bool VSTDriver::connect_pipe(HANDLE hPipe)
{
	OVERLAPPED ol = {};
	ol.hEvent = hReadEvent;
	ResetEvent( hReadEvent );
	if ( !ConnectNamedPipe( hPipe, &ol ) )
	{
		DWORD error = GetLastError();
		if ( error == ERROR_PIPE_CONNECTED ) return true;
		if ( error != ERROR_IO_PENDING ) return false;

		if ( WaitForSingleObject( hReadEvent, 5000 ) == WAIT_TIMEOUT ) return false;
	}
	return true;
}
*/

tstring VSTDriver::GetVsthostPath()
{
	TCHAR my_path[MAX_PATH];
	GetModuleFileName(hinst_vst_driver, my_path, _countof(my_path));

	tstring sDir(my_path);
	size_t idx = sDir.find_last_of(L'\\') + 1;
	if (idx != tstring::npos)
		sDir.resize(idx);
	tstring sSubdir(my_path + idx);
	idx = sSubdir.find_last_of(L'.');
	if (idx != tstring::npos)
		sSubdir.resize(idx);
	sSubdir += L'\\';
	tstring sFile((uPluginPlatform == 64) ? _T("vstbridgeapp64.exe") : _T("vstbridgeapp32.exe"));

	tstring sHostPath = sDir + sFile;
	if (::GetFileAttributes(sHostPath.c_str()) == INVALID_FILE_ATTRIBUTES)
		sHostPath = sDir + sSubdir + sFile;

	return sHostPath;
}

bool VSTDriver::process_create()
{
	if (uPluginPlatform != 32 && uPluginPlatform != 64) return false;

	SECURITY_ATTRIBUTES saAttr = { 0 };

	saAttr.nLength = sizeof(saAttr);
	saAttr.bInheritHandle = TRUE;
	saAttr.lpSecurityDescriptor = NULL;

	if (!bInitialized)
	{
		HRESULT ret = CoInitialize(NULL);
		if (FAILED(ret) && ret != RPC_E_CHANGED_MODE) return false;
		bInitialized = true;
		bInitializedOtherModel = ret == RPC_E_CHANGED_MODE;
	}

	hReadEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	hWriteEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (!hReadEvent || !hWriteEvent || !hCancelEvent) {
		process_terminate();
		return false;
	}
	ResetEvent(hCancelEvent);

	tstring pipe_name_in, pipe_name_out;
	if (!create_pipe_name(pipe_name_in) || !create_pipe_name(pipe_name_out))
	{
		process_terminate();
		return false;
	}

	HANDLE hPipe = CreateNamedPipe(pipe_name_in.c_str(), PIPE_ACCESS_OUTBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE, 1, 65536, 65536, 0, &saAttr);
	if (hPipe == INVALID_HANDLE_VALUE)
	{
		process_terminate();
		return false;
	}
	hChildStd_IN_Rd = CreateFile(pipe_name_in.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &saAttr, OPEN_EXISTING, 0, NULL);
	DuplicateHandle(GetCurrentProcess(), hPipe, GetCurrentProcess(), &hChildStd_IN_Wr, 0, FALSE, DUPLICATE_SAME_ACCESS);
	CloseHandle(hPipe);

	hPipe = CreateNamedPipe(pipe_name_out.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE, 1, 65536, 65536, 0, &saAttr);
	if (hPipe == INVALID_HANDLE_VALUE)
	{
		process_terminate();
		return false;
	}
	hChildStd_OUT_Wr = CreateFile(pipe_name_out.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &saAttr, OPEN_EXISTING, 0, NULL);
	DuplicateHandle(GetCurrentProcess(), hPipe, GetCurrentProcess(), &hChildStd_OUT_Rd, 0, FALSE, DUPLICATE_SAME_ACCESS);
	CloseHandle(hPipe);

	tstring szCmdLine = _T("\"");
	szCmdLine += GetVsthostPath();
	szCmdLine += _T("\" \"");
	szCmdLine += szPluginPath;
	szCmdLine += _T("\" ");

	unsigned sum = 0;

	const TCHAR* ch = szPluginPath;
	while (*ch)
	{
		sum += (TCHAR)(*ch++ * 820109);
	}

	print_hex(sum, szCmdLine, 4);

	PROCESS_INFORMATION piProcInfo;
	STARTUPINFO siStartInfo = { 0 };

	siStartInfo.cb = sizeof(siStartInfo);
	siStartInfo.hStdInput = hChildStd_IN_Rd;
	siStartInfo.hStdOutput = hChildStd_OUT_Wr;
	siStartInfo.hStdError = GetStdHandle(STD_ERROR_HANDLE);
	siStartInfo.dwFlags |= STARTF_USESTDHANDLES;

	TCHAR CmdLine[MAX_PATH * 2];
	_tcscpy_s(CmdLine, _countof(CmdLine), szCmdLine.c_str());

	TCHAR exe_path[MAX_PATH] = { 0 };
	TCHAR exe_title[MAX_PATH / 2] = { 0 };
#ifdef WIN64
	const TCHAR bitnessStr[] = _T(" 64-bit");
#else
	const TCHAR bitnessStr[] = _T(" 32-bit");
#endif	
	GetModuleFileName(NULL, exe_path, _countof(exe_path));
	GetFileTitle(exe_path, exe_title, _countof(exe_title));

	DWORD proxyPortNum = 0;
	TCHAR proxyPort[] = _T(" 0");

	if (_tcsstr(exe_title, _T("vstmidiproxy")))
	{
		TCHAR* pProxyCmdLine;
		pProxyCmdLine = GetCommandLine();

		proxyPort[1] = pProxyCmdLine[_tcslen(pProxyCmdLine) - 1];
		TCHAR* end_char = NULL;
		proxyPortNum = _tcstoul(&proxyPort[1], &end_char, 10);
	}

	_tcscat_s(CmdLine, _T(" \""));
	_tcscat_s(CmdLine, exe_title);
	_tcscat_s(CmdLine, _T("\""));
	_tcscat_s(CmdLine, bitnessStr);
	_tcscat_s(CmdLine, UseAsio() ? (UseWasapi() ? _T(" S") : _T(" A")) : _T(" W"));
	if (proxyPortNum != 0 && proxyPortNum < 8) _tcscat_s(CmdLine, proxyPort);

	if (!CreateProcess(NULL, CmdLine, NULL, NULL, TRUE, 0, NULL, NULL, &siStartInfo, &piProcInfo))
	{
		process_terminate();
		return false;
	}

	// Close remote handles so pipes will break when process terminates
	CloseHandle(hChildStd_OUT_Wr);
	CloseHandle(hChildStd_IN_Rd);
	hChildStd_OUT_Wr = NULL;
	hChildStd_IN_Rd = NULL;

	hProcess = piProcInfo.hProcess;
	childProcessId = piProcInfo.dwProcessId;
	hThread = piProcInfo.hThread;

#ifdef NDEBUG
	SetPriorityClass(hProcess, GetPriorityClass(GetCurrentProcess()));
	//SetThreadPriority( hThread, GetThreadPriority( GetCurrentThread() ) );
	SetThreadPriority(hThread, THREAD_PRIORITY_TIME_CRITICAL);
#endif

	uint32_t code = process_read_code();

	if (code != 0)
	{
		process_terminate();
		return false;
	}

	// Read failures must not become lengths or channel counts.
	uint32_t metadata[6];
	if (!process_read_bytes(metadata, sizeof(metadata)) ||
		metadata[0] > 255 || metadata[1] > 255 || metadata[2] > 255 ||
		metadata[5] != 2) { // The bridge always sends stereo, or two stereo ports.
		process_terminate();
		return false;
	}

	char* name = new (std::nothrow) char[metadata[0] + 1];
	char* vendor = new (std::nothrow) char[metadata[1] + 1];
	char* product = new (std::nothrow) char[metadata[2] + 1];
	if (!name || !vendor || !product ||
		!process_read_bytes(name, metadata[0]) ||
		!process_read_bytes(vendor, metadata[1]) ||
		!process_read_bytes(product, metadata[2])) {
		delete[] name;
		delete[] vendor;
		delete[] product;
		process_terminate();
		return false;
	}
	name[metadata[0]] = vendor[metadata[1]] = product[metadata[2]] = 0;

	delete[] sName;
	delete[] sVendor;
	delete[] sProduct;
	sName = name;
	sVendor = vendor;
	sProduct = product;
	uVendorVersion = metadata[3];
	uUniqueId = metadata[4];
	uNumOutputs = metadata[5];

	//Always keep SC-VA loaded even when ports are closed because of huge loading times. For other plugins this setting is optional.
	IsSCVA = (uUniqueId == (uint32_t)'scva');

	return true;
}

void VSTDriver::cancel_and_wait_for_io(HANDLE pipe, VSTDriverPipeIO* operation)
{
	// CancelIo cancels only I/O issued by THIS thread (available on NT4).
	CancelIo(pipe);
	if (WaitForSingleObject(operation->overlapped.hEvent, 5000) == WAIT_OBJECT_0) {
		DWORD bytesDone = 0;
		GetOverlappedResult(pipe, &operation->overlapped, &bytesDone, FALSE);
		return;
	}

	// Stop using this connection. Retain the unfinished operation and its handles
	// until process exit so a late completion cannot touch freed/reused storage.
	cancelPendingIO();
	if (pipe == hChildStd_IN_Wr) hChildStd_IN_Wr = NULL;
	if (pipe == hChildStd_OUT_Rd) hChildStd_OUT_Rd = NULL;
	if (operation->overlapped.hEvent == hReadEvent) hReadEvent = NULL;
	if (operation->overlapped.hEvent == hWriteEvent) hWriteEvent = NULL;
	pipeIO = NULL;
	OutputDebugString(_T("VST MIDI Driver: pipe cancellation timed out; connection abandoned\n"));
}

void VSTDriver::process_terminate()
{
	if (hProcess) {
		// A failed pipe write must never recursively enter process_terminate.
		// Attempt a bounded graceful exit only when the protocol is still usable.
		if (process_running() && !isIOCancelled() && hWriteEvent) {
			if (!pipeIO) pipeIO = new (std::nothrow) VSTDriverPipeIO;
			if (pipeIO) {
				VSTDriverIOActivity activity(&pendingIO);
				VSTDriverPipeIO* operation = pipeIO;
				DWORD command = Command::Exit, written = 0;
				memset(&operation->overlapped, 0, sizeof(operation->overlapped));
				operation->overlapped.hEvent = hWriteEvent;
				memcpy(operation->buffer, &command, sizeof(command));
				ResetEvent(hWriteEvent);
				BOOL done = WriteFile(hChildStd_IN_Wr, operation->buffer, sizeof(command), &written, &operation->overlapped);
				if (!done && GetLastError() == ERROR_IO_PENDING) {
					if (WaitForSingleObject(hWriteEvent, 500) == WAIT_OBJECT_0)
						done = GetOverlappedResult(hChildStd_IN_Wr, &operation->overlapped, &written, FALSE);
					else
						cancel_and_wait_for_io(hChildStd_IN_Wr, operation);
				}
				if (done && written == sizeof(command)) WaitForSingleObject(hProcess, 5000);
			}
		}
		if (process_running()) {
			TerminateProcess(hProcess, 0);
			WaitForSingleObject(hProcess, 2000);
		}
		CloseHandle(hThread);
		CloseHandle(hProcess);
	}
	if (hChildStd_IN_Rd) CloseHandle(hChildStd_IN_Rd);
	if (hChildStd_IN_Wr) CloseHandle(hChildStd_IN_Wr);
	if (hChildStd_OUT_Rd) CloseHandle(hChildStd_OUT_Rd);
	if (hChildStd_OUT_Wr) CloseHandle(hChildStd_OUT_Wr);
	if (hReadEvent) CloseHandle(hReadEvent);
	if (hWriteEvent) CloseHandle(hWriteEvent);
	if (bInitialized && !bInitializedOtherModel) CoUninitialize();
	bInitialized = false;
	bInitializedOtherModel = false;
	hProcess = hThread = hReadEvent = hWriteEvent = NULL;
	childProcessId = 0;
	hChildStd_IN_Rd = hChildStd_IN_Wr = NULL;
	hChildStd_OUT_Rd = hChildStd_OUT_Wr = NULL;
	// hCancelEvent belongs to this VSTDriver object, not the child process.
}

void VSTDriver::cancelPendingIO()
{
	if (hCancelEvent) SetEvent(hCancelEvent);
}

bool VSTDriver::isIOCancelled()
{
	return hCancelEvent && WaitForSingleObject(hCancelEvent, 0) == WAIT_OBJECT_0;
}

bool VSTDriver::hasPendingIO()
{
	return InterlockedCompareExchange(&pendingIO, 0, 0) != 0;
}

static BOOL CALLBACK HideBridgeWindow(HWND window, LPARAM processId)
{
	DWORD windowProcessId = 0;
	GetWindowThreadProcessId(window, &windowProcessId);
	if (windowProcessId == (DWORD)processId) ShowWindowAsync(window, SW_HIDE);
	return TRUE;
}

void VSTDriver::hideEditors()
{
	// Do not wait for the rendering pipe or another thread's window procedure.
	if (childProcessId && process_running())
		EnumWindows(HideBridgeWindow, (LPARAM)childProcessId);
}

bool VSTDriver::process_running()
{
	if (hProcess && WaitForSingleObject(hProcess, 0) == WAIT_TIMEOUT) return true;
	return false;
}

static void ProcessPendingMessages(HWND editorOwner)
{
	MSG msg = { 0 };
	while (PeekMessage(&msg, editorOwner, 0, 0, PM_REMOVE))
	{
		DispatchMessage(&msg);
	}
}

uint32_t VSTDriver::process_transfer_bytes_pass(void* out, uint32_t size, bool writing)
{
	if (isIOCancelled() || !process_running()) return 0;
	if (!pipeIO) pipeIO = new (std::nothrow) VSTDriverPipeIO;
	if (!pipeIO) return 0;
	VSTDriverIOActivity activity(&pendingIO);
	VSTDriverPipeIO* operation = pipeIO;
	// The byte-stream callers already retry partial transfers.
	if (size > sizeof(operation->buffer)) size = (uint32_t)sizeof(operation->buffer);
	HANDLE pipe = writing ? hChildStd_IN_Wr : hChildStd_OUT_Rd;
	OVERLAPPED& ol = operation->overlapped;
	memset(&ol, 0, sizeof(ol));
	ol.hEvent = writing ? hWriteEvent : hReadEvent;
	ResetEvent(ol.hEvent);
	DWORD bytesDone = 0;
	if (writing) memcpy(operation->buffer, out, size);
	BOOL done = writing ? WriteFile(pipe, operation->buffer, size, &bytesDone, &ol) :
		ReadFile(pipe, operation->buffer, size, &bytesDone, &ol);
	if (done) {
		if (bytesDone > size) return 0;
		if (!writing) memcpy(out, operation->buffer, bytesDone);
		return bytesDone;
	}
	if (GetLastError() != ERROR_IO_PENDING) return 0;

	const HANDLE handles[3] = { ol.hEvent, hCancelEvent, hProcess };
	DWORD started = GetTickCount();
	for (;;) {
		DWORD waitTime = ioTimeout;
		if (ioTimeout != INFINITE) {
			DWORD elapsed = GetTickCount() - started;
			waitTime = elapsed < ioTimeout ? ioTimeout - elapsed : 0;
		}
		DWORD state = MsgWaitForMultipleObjects(3, handles, FALSE, waitTime, QS_ALLEVENTS);
		if (state == WAIT_OBJECT_0 + 3) {
			ProcessPendingMessages(editorOwner);
			continue;
		}
		if (state == WAIT_OBJECT_0 && GetOverlappedResult(pipe, &ol, &bytesDone, FALSE)) {
			if (bytesDone > size) return 0;
			if (!writing) memcpy(out, operation->buffer, bytesDone);
			return bytesDone;
		}
		cancel_and_wait_for_io(pipe, operation);
		return 0;
	}
}

uint32_t VSTDriver::process_read_bytes_pass(void* out, uint32_t size)
{
	return process_transfer_bytes_pass(out, size, false);
}

bool VSTDriver::process_read_bytes(void* out, uint32_t size)
{
	if (!size) return true;
	uint32_t done = 0;
	while (done < size) {
		uint32_t delta = process_read_bytes_pass((uint8_t*)out + done, size - done);
		if (!delta) {
			memset(out, 0xFF, size); // Preserve existing error-code callers.
			cancelPendingIO();
			return false;
		}
		done += delta;
	}
	return true;
}

uint32_t VSTDriver::process_read_code()
{
	uint32_t code;
	process_read_bytes(&code, sizeof(code));
	return code;
}

bool VSTDriver::process_write_bytes(const void* in, uint32_t size)
{
	uint32_t done = 0;
	while (done < size) {
		uint32_t delta = process_transfer_bytes_pass((uint8_t*)in + done, size - done, true);
		if (!delta) {
			cancelPendingIO();
			return false;
		}
		done += delta;
	}
	return true;
}

void VSTDriver::process_write_code(uint32_t code)
{
	process_write_bytes(&code, sizeof(code));
}

void VSTDriver::getEffectName(std::string& out)
{
	out = sName;
}

void VSTDriver::getVendorString(std::string& out)
{
	out = sVendor;
}

void VSTDriver::getProductString(std::string& out)
{
	out = sProduct;
}

long VSTDriver::getVendorVersion()
{
	return uVendorVersion;
}

long VSTDriver::getUniqueID()
{
	return uUniqueId;
}

void VSTDriver::getChunk(std::vector<uint8_t>& out)
{
	out.clear();
	process_write_code(Command::GetChunkData);
	uint32_t code = process_read_code();
	if (code == VSTDriverInvalidChunk) return;
	if (code != NoError) { process_terminate(); return; }
	uint32_t size = 0;
	if (!process_read_bytes(&size, sizeof(size)) || !VSTDriverResizeChunk(out, size) ||
		(size && !process_read_bytes(&out.front(), size))) {
		out.clear();
		cancelPendingIO();
		process_terminate();
	}
}

bool VSTDriver::setChunk(const void* in, unsigned size)
{
	if (size > VSTDriverMaxChunkBytes || (size && !in)) return false;
	process_write_code(Command::SetChunkData);
	process_write_code(size);
	process_write_bytes(in, size);
	uint32_t code = process_read_code();
	if (code != NoError && code != VSTDriverInvalidChunk) process_terminate();
	return code == NoError;
}

/*bool VSTDriver::hasEditor()
{
	process_write_code( Command::HasEditor );
	uint32_t code = process_read_code();
	if ( code != NoError )
	{
		process_terminate();
		return false;
	}
	code = process_read_code();
	return code != 0;
}*/

void VSTDriver::setHighDpiMode(unsigned int modeNum)
{
	if (modeNum)
	{
		process_write_code(Command::SetHighDpiMode);
		process_write_code(modeNum);

		uint32_t code = process_read_code();
		if (code != NoError) {
			process_terminate();
		}
	}
}

void VSTDriver::initSysTray()
{
	process_write_code(Command::InitSysTray);

	uint32_t code = process_read_code();
	if (code != NoError) {
		process_terminate();
	}
	Sleep(50);
}

void VSTDriver::setSinglePort32ChMode()
{
	process_write_code(Command::SetSinglePort32ChMode);

	uint32_t code = process_read_code();
	if (code != NoError) {
		process_terminate();
	}

}

void VSTDriver::setSampleRate(unsigned int sampleRate)
{
	process_write_code(Command::SetSampleRate);
	process_write_code(sizeof(uint32_t));
	process_write_code(sampleRate);

	uint32_t code = process_read_code();
	if (code != NoError) {
		process_terminate();
	}
}

void VSTDriver::displayEditorModal(unsigned int uDeviceID, HWND owner)
{
	uint32_t code = uDeviceID == 255 ? Command::DisplayEditorModal : Command::DisplayEditorModalThreaded;

	editorOwner = owner;

	process_write_code(code);

	if (code == Command::DisplayEditorModalThreaded) {
		process_write_code(uDeviceID);
	}

	code = process_read_code();
	if (code != NoError) process_terminate();
}

void VSTDriver::CloseVSTDriver() {
	process_terminate();

	if (szPluginPath) {
		free(szPluginPath);

		szPluginPath = NULL;
	}
}

BOOL VSTDriver::OpenVSTDriver(TCHAR* szPath, int sampleRate) {
	CloseVSTDriver();

	load_settings(szPath);

	if (process_create()) {
		process_write_code(Command::SetSampleRate);
		process_write_code(sizeof(uint32_t));
		process_write_code(sampleRate);

		uint32_t code = process_read_code();
		if (code != NoError) {
			process_terminate();
			return FALSE;
		}

		if (blChunk.size()) {
			process_write_code(Command::SetChunkData);
			process_write_code((uint32_t)blChunk.size());
			if (blChunk.size()) process_write_bytes(&blChunk.front(), (uint32_t)blChunk.size());
			code = process_read_code();
			if (code == VSTDriverInvalidChunk) {
				OutputDebugString(_T("VST MIDI Driver: malformed or incompatible settings ignored\n"));
			}
			else if (code != NoError) {
				process_terminate();
				return FALSE;
			}

		}

		return TRUE;
	}

	return FALSE;
}

void VSTDriver::ResetDriver(unsigned int uDeviceID) {
	DWORD previousTimeout = ioTimeout;
	ioTimeout = 2000; // Closing a retained port must not wait forever for plugin reset.

	process_write_code(Command::Reset);
	process_write_code(uDeviceID);
	uint32_t code = process_read_code();
	if (code != NoError) process_terminate();
	ioTimeout = previousTimeout;
}

void VSTDriver::ProcessMIDIMessage(DWORD dwPort, DWORD dwParam1) {
	dwParam1 = (dwParam1 & 0xFFFFFF) | (dwPort << 24);
	uint32_t request[2] = { Command::SendMidiEvent, dwParam1 };
	process_write_bytes(request, sizeof(request));

	uint32_t code = process_read_code();
	if (code != NoError) process_terminate();
}

void VSTDriver::ProcessMIDIMessages(const DWORD* messages, DWORD count) {
	if (!messages || !count || count > VSTDriverMidiProtocol::MaxBatchEvents) return;
	for (DWORD i = 0; i < count; ++i) {
		if ((messages[i] >> 24) > 1) return;
	}
	if (count == 1) {
		ProcessMIDIMessage(messages[0] >> 24, messages[0] & 0xFFFFFF);
		return;
	}

	// One contiguous request and one acknowledgement, with no heap allocation.
	uint32_t packet[2 + VSTDriverMidiProtocol::MaxBatchEvents];
	packet[0] = Command::SendMidiEvents;
	packet[1] = count;
	memcpy(packet + 2, messages, count * sizeof(*messages));
	if (!process_write_bytes(packet, (count + 2) * sizeof(*packet))) {
		process_terminate();
		return;
	}
	uint32_t code = process_read_code();
	if (code != NoError) process_terminate();
}

void VSTDriver::ProcessSysEx(DWORD dwPort, const unsigned char* sysexbuffer, int exlen) {
	// Validate before writing any part of the command; header and payload must agree.
	if (dwPort > 1 || exlen <= 0 || (unsigned)exlen > VSTDriverMaxSysExBytes || !sysexbuffer) return;
	dwPort = (dwPort << 24) | (unsigned)exlen;
	uint32_t request[2] = { Command::SendMidiSysExEvent, dwPort };
	process_write_bytes(request, sizeof(request));
	process_write_bytes(sysexbuffer, exlen);

	uint32_t code = process_read_code();
	if (code != NoError) process_terminate();
}

uint32_t VSTDriver::RenderFloatInternal(float* samples, int len, float volume, WORD channels) {
	uint32_t opCode = channels == 2 ? Command::RenderAudioSamples : Command::RenderAudioSamples4channel;
	uint32_t request[2] = { opCode, (uint32_t)len };
	process_write_bytes(request, sizeof(request));

	uint32_t code = process_read_code();
	if (code != NoError) {
		process_terminate();
		memset(samples, 0, sizeof(*samples) * len * uNumOutputs * channels / 2);
		return code;
	}

	if (!process_read_bytes(samples, sizeof(*samples) * len * uNumOutputs * channels / 2)) {
		memset(samples, 0, sizeof(*samples) * len * uNumOutputs * channels / 2);
		process_terminate();
		return (uint32_t)-1;
	}

	if (volume != 1.0f)
	{
		unsigned int tmp = len * uNumOutputs * channels / 2;
		for (unsigned int i = 0; i < tmp; i++)
			samples[i] *= volume;
	}

	return 0;
}

uint32_t VSTDriver::RenderFloat(float* samples, int len, float volume, WORD channels)
{
	while (len > 0)
	{
		int len_todo = len > BUFFER_SIZE ? BUFFER_SIZE : len;
		int result = RenderFloatInternal(samples, len_todo, volume, channels);
		samples += len_todo * uNumOutputs * channels / 2;
		len -= len_todo;
		if (result == ResetRequest) return result;
	}

	return 0;
}

uint32_t VSTDriver::Render(short* samples, int len, float volume, WORD channels)
{
#pragma warning(disable:6385) //false buffer alarms
#pragma warning(disable:6386) 

	const int maxFrames = BUFFER_SIZE / 4;
	float* float_out = (float*)_alloca((UINT64)maxFrames * uNumOutputs * channels / 2 * sizeof(*float_out));
	while (len > 0)
	{
		int len_todo = len > maxFrames ? maxFrames : len;
		int result = RenderFloatInternal(float_out, len_todo, volume, channels);
		unsigned int tmp = len_todo * uNumOutputs * channels / 2;
		for (unsigned int i = 0; i < tmp; i++)
		{
			int sample = (int)(float_out[i] * 32768.f);
			if ((sample + 0x8000) & 0xFFFF0000) sample = 0x7FFF ^ (sample >> 31);
			samples[0] = sample;
			samples++;
		}
		len -= len_todo;
		if (result == ResetRequest) return result;
	}

	return 0;

#pragma warning(default:6385)  
#pragma warning(default:6386)
}
