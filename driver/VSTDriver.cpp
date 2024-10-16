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

#pragma warning(disable:6255) 
#pragma warning(disable:26812)

enum
{
	BUFFER_SIZE = 3840  
};

extern "C" { extern HINSTANCE hinst_vst_driver; };

UINT GetWaveOutDeviceId() {

	HKEY hKey;
	DWORD dwType = REG_SZ;
	DWORD dwSize = 0;
	WAVEOUTCAPS caps;
	

	long result = RegOpenKeyEx(HKEY_CURRENT_USER, _T("Software\\VSTi Driver\\Output Driver"), 0, KEY_READ, &hKey);
	if (result == NO_ERROR)
	{

		result = RegQueryValueEx(hKey, _T("WinMM WaveOut"), NULL, &dwType, NULL, &dwSize);
		if (result == NO_ERROR && dwType == REG_SZ)
		{
			TCHAR* regValue;
			regValue = (TCHAR*)calloc(dwSize + sizeof(TCHAR), 1);
			if (regValue)
			{
				RegQueryValueEx(hKey, _T("WinMM WaveOut"), NULL, &dwType, (LPBYTE)regValue, &dwSize);

				for (int deviceId = -1; waveOutGetDevCaps(deviceId, &caps, sizeof(caps)) == MMSYSERR_NOERROR; ++deviceId)
				{
					if (regValue && !_tcscmp(regValue, caps.szPname))
					{
						free(regValue);
						RegCloseKey(hKey);
						return deviceId;
					}

				}

				free(regValue);
			}
		}

		RegCloseKey(hKey);
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
	DWORD dwType = REG_SZ;
	DWORD dwSize = 0;	

	long result = RegOpenKeyEx(HKEY_CURRENT_USER, _T("Software\\VSTi Driver\\Output Driver"), 0, KEY_READ, &hKey);
	if (result == NO_ERROR) 
	{
		result = RegQueryValueEx(hKey, _T("Driver Mode"), NULL, &dwType, NULL, &dwSize);
		if (result == NO_ERROR && dwType == REG_SZ && dwSize > 8)
		{
			TCHAR* regValue;
			regValue = (TCHAR*) calloc( dwSize + sizeof(TCHAR), 1 );
			if (regValue)
			{
				RegQueryValueEx(hKey, _T("Driver Mode"), NULL, &dwType, (LPBYTE)regValue, &dwSize);
				if (!_tcscmp(regValue, _T("Bass ASIO")))
				{
					free(regValue);
					RegCloseKey(hKey);
					return true;
				}

				free(regValue);
			}
		}

		RegCloseKey(hKey);
	}

	return false;
}

bool UseWasapi()
{
	HKEY hKey;
	DWORD dwType = REG_SZ;
	DWORD dwSize = 0;	

	long result = RegOpenKeyEx(HKEY_CURRENT_USER, _T("Software\\VSTi Driver\\Output Driver"), 0, KEY_READ, &hKey);
	if (result == NO_ERROR)
	{
#ifdef WIN64
		result = RegQueryValueEx(hKey, _T("Bass ASIO x64"), NULL, &dwType, NULL, &dwSize);
#else
		result = RegQueryValueEx(hKey, _T("Bass ASIO"), NULL, &dwType, NULL, &dwSize);
#endif	
		
		if (result == NO_ERROR && dwType == REG_SZ && dwSize > 20)
		{
			TCHAR* regValue;
			regValue = (TCHAR*)calloc(dwSize + sizeof(TCHAR), 1);
			if (regValue)
			{
#ifdef WIN64
				RegQueryValueEx(hKey, _T("Bass ASIO x64"), NULL, &dwType, (LPBYTE)regValue, &dwSize);
#else
				RegQueryValueEx(hKey, _T("Bass ASIO"), NULL, &dwType, (LPBYTE)regValue, &dwSize);
#endif
				if (_tcsstr(regValue, _T("VSTDriver-ASIO2WASAPI")))
				{
					free(regValue);
					RegCloseKey(hKey);
					return true;
				}

				free(regValue);
			}
		}

		RegCloseKey(hKey);
	}

	return false;
}

namespace Command {
	enum : uint32_t
	{
		Exit = 0,
		GetChunkData = 1,
		SetChunkData = 2,
		//HasEditor = 3,
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

VSTDriver::VSTDriver() :
	szPluginPath(),
	bInitialized(),
	bInitializedOtherModel(),
	hProcess(),
	hThread(),
	hReadEvent(),
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
}

VSTDriver::~VSTDriver() {
	CloseVSTDriver();
	delete [] sName;
	delete [] sVendor;
	delete [] sProduct;
}

static WORD getwordle(BYTE *pData)
{
	return (WORD)(pData[0] | (((WORD)pData[1]) << 8));
}

static DWORD getdwordle(BYTE *pData)
{
	return pData[0] | (((DWORD)pData[1]) << 8) | (((DWORD)pData[2]) << 16) | (((DWORD)pData[3]) << 24);
}

unsigned VSTDriver::test_plugin_platform() {
#define iMZHeaderSize (0x40)
#define iPEHeaderSize (4 + 20 + 224)

	BYTE peheader[iPEHeaderSize];
	DWORD dwOffsetPE;

	FILE * f = _tfopen( szPluginPath, _T("rb") );
	if ( !f ) goto error;
	if ( fread( peheader, 1, iMZHeaderSize, f ) < iMZHeaderSize ) goto error;
	if ( getwordle(peheader) != 0x5A4D ) goto error;
	dwOffsetPE = getdwordle( peheader + 0x3c );
	if ( fseek( f, dwOffsetPE, SEEK_SET ) != 0 ) goto error;
	if ( fread( peheader, 1, iPEHeaderSize, f ) < iPEHeaderSize ) goto error;
	fclose( f ); f = NULL;
	if ( getdwordle( peheader ) != 0x00004550 ) goto error;
	switch ( getwordle( peheader + 4 ) ) {
	case 0x014C: return 32;
	case 0x8664: return 64;
	}

error:
	if ( f ) fclose( f );
	return 0;
}

void VSTDriver::load_settings(TCHAR * szPath) {
	HKEY hKey;	
	DWORD dwType = REG_SZ;
	DWORD dwSize = 0;
	DWORD selIndex = 0;
	
	if ( szPath || RegOpenKeyEx(HKEY_CURRENT_USER, _T("Software\\VSTi Driver"),0,KEY_READ, &hKey) == ERROR_SUCCESS )
	{
		long lResult = 0;
		TCHAR szValueName[20] = _T("plugin");
		if (!szPath)
		{
			dwSize = sizeof(selIndex);
			lResult = RegQueryValueEx(hKey, _T("SelectedPlugin"), NULL, NULL, (LPBYTE)&selIndex, &dwSize);
			if (lResult == ERROR_SUCCESS && selIndex)
			{
				TCHAR szPostfix[12] = { 0 };
				_itot_s(selIndex, szPostfix, 10);
				_tcsncat_s(szValueName, szPostfix, _countof(szPostfix));
			}
			lResult = RegQueryValueEx(hKey, szValueName, NULL, &dwType, NULL, &dwSize);
		}
		if (szPath || (lResult == ERROR_SUCCESS && dwType == REG_SZ)) {
			if (szPath) dwSize = (DWORD)(_tcslen(szPath) * sizeof(TCHAR));
			szPluginPath = (TCHAR*)calloc(dwSize + sizeof(TCHAR), 1);
			if (szPluginPath) {
				if (szPath) _tcscpy(szPluginPath, szPath);
				else RegQueryValueEx(hKey, szValueName, NULL, &dwType, (LPBYTE)szPluginPath, &dwSize);

				uPluginPlatform = test_plugin_platform();

				blChunk.resize(0);

				const TCHAR* dot = _tcsrchr(szPluginPath, _T('.'));
				if (!dot) dot = szPluginPath + _tcslen(szPluginPath);
				TCHAR* szSettingsPath = (TCHAR*)_alloca((dot - szPluginPath + 5) * sizeof(TCHAR));
				_tcsncpy(szSettingsPath, szPluginPath, dot - szPluginPath);
				_tcscpy(szSettingsPath + (dot - szPluginPath), _T(".set"));

				FILE* f;
				errno_t err = _tfopen_s(&f, szSettingsPath, _T("rb"));
				if (!err && f)
				{
					fseek(f, 0, SEEK_END);
					size_t chunk_size = ftell(f);
					fseek(f, 0, SEEK_SET);
					blChunk.resize(chunk_size);
					if (chunk_size) fread(&blChunk.front(), 1, chunk_size, f);
					fclose(f);
				}
			}
		}
		if ( !szPath ) RegCloseKey( hKey);
	}
}

static inline char print_hex_digit(unsigned val)
{
	static const char table[16] = {'0','1','2','3','4','5','6','7','8','9','A','B','C','D','E','F'};
	assert((val & ~0xF) == 0);
	return table[val];
}

static void print_hex(unsigned val,tstring &out,unsigned bytes)
{
	unsigned n;
	for(n=0;n<bytes;n++)
	{
		unsigned char c = (unsigned char)((val >> ((bytes - 1 - n) << 3)) & 0xFF);
		out += print_hex_digit( c >> 4 );
		out += print_hex_digit( c & 0xF );
	}
}

static void print_guid(const GUID & p_guid, tstring &out)
{
	print_hex(p_guid.Data1,out,4);
	out += '-';
	print_hex(p_guid.Data2,out,2);
	out += '-';
	print_hex(p_guid.Data3,out,2);
	out += '-';
	print_hex(p_guid.Data4[0],out,1);
	print_hex(p_guid.Data4[1],out,1);
	out += '-';
	print_hex(p_guid.Data4[2],out,1);
	print_hex(p_guid.Data4[3],out,1);
	print_hex(p_guid.Data4[4],out,1);
	print_hex(p_guid.Data4[5],out,1);
	print_hex(p_guid.Data4[6],out,1);
	print_hex(p_guid.Data4[7],out,1);
}

static bool create_pipe_name( tstring & out )
{
	GUID guid;
	if ( FAILED( CoCreateGuid( &guid ) ) ) return false;

	out = _T("\\\\.\\pipe\\");
	print_guid( guid, out );

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
	GetModuleFileName( hinst_vst_driver, my_path, _countof(my_path) );

	tstring sDir(my_path);
	size_t idx = sDir.find_last_of( L'\\' ) + 1;
	if (idx != tstring::npos)
		sDir.resize( idx );
	tstring sSubdir(my_path + idx);
	idx = sSubdir.find_last_of( L'.' );
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
	if ( uPluginPlatform != 32 && uPluginPlatform != 64 ) return false;

	SECURITY_ATTRIBUTES saAttr = {0};

	saAttr.nLength = sizeof(saAttr);
	saAttr.bInheritHandle = TRUE;
	saAttr.lpSecurityDescriptor = NULL;

	if ( !bInitialized )
	{
		HRESULT ret = CoInitialize( NULL );
		if ( FAILED( ret ) && ret != RPC_E_CHANGED_MODE ) return false;
		bInitialized = true;
		bInitializedOtherModel = ret == RPC_E_CHANGED_MODE;
	}

	hReadEvent = CreateEvent( NULL, TRUE, FALSE, NULL );

	tstring pipe_name_in, pipe_name_out;
	if ( !create_pipe_name( pipe_name_in ) || !create_pipe_name( pipe_name_out ) )
	{
		process_terminate();
		return false;
	}

	HANDLE hPipe = CreateNamedPipe( pipe_name_in.c_str(), PIPE_ACCESS_OUTBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE, 1, 65536, 65536, 0, &saAttr );
	if ( hPipe == INVALID_HANDLE_VALUE )
	{
		process_terminate();
		return false;
	}
	hChildStd_IN_Rd = CreateFile( pipe_name_in.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &saAttr, OPEN_EXISTING, 0, NULL );
	DuplicateHandle( GetCurrentProcess(), hPipe, GetCurrentProcess(), &hChildStd_IN_Wr, 0, FALSE, DUPLICATE_SAME_ACCESS );
	CloseHandle( hPipe );

	hPipe = CreateNamedPipe( pipe_name_out.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE, 1, 65536, 65536, 0, &saAttr );
	if ( hPipe == INVALID_HANDLE_VALUE )
	{
		process_terminate();
		return false;
	}
	hChildStd_OUT_Wr = CreateFile( pipe_name_out.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &saAttr, OPEN_EXISTING, 0, NULL );
	DuplicateHandle( GetCurrentProcess(), hPipe, GetCurrentProcess(), &hChildStd_OUT_Rd, 0, FALSE, DUPLICATE_SAME_ACCESS );
	CloseHandle( hPipe );

	tstring szCmdLine = _T("\"");
	szCmdLine += GetVsthostPath();
	szCmdLine += _T("\" \"");
	szCmdLine += szPluginPath;
	szCmdLine += _T("\" ");

	unsigned sum = 0;

	const TCHAR * ch = szPluginPath;
	while ( *ch )
	{
		sum += (TCHAR)( *ch++ * 820109 );
	}

	print_hex( sum, szCmdLine, 4 );

	PROCESS_INFORMATION piProcInfo;
	STARTUPINFO siStartInfo = {0};

	siStartInfo.cb = sizeof(siStartInfo);
	siStartInfo.hStdInput = hChildStd_IN_Rd;
	siStartInfo.hStdOutput = hChildStd_OUT_Wr;
	siStartInfo.hStdError = GetStdHandle( STD_ERROR_HANDLE );
	siStartInfo.dwFlags |= STARTF_USESTDHANDLES;

	TCHAR CmdLine[MAX_PATH * 2];
	_tcscpy_s(CmdLine, _countof(CmdLine), szCmdLine.c_str());
    
	TCHAR exe_path[MAX_PATH] = {0};
	TCHAR exe_title[MAX_PATH / 2] = {0};
#ifdef WIN64
	const TCHAR bitnessStr[] = _T(" 64-bit");
#else
	const TCHAR bitnessStr[] = _T(" 32-bit");
#endif	

	GetModuleFileName(NULL, exe_path, _countof(exe_path));
	GetFileTitle(exe_path, exe_title, _countof(exe_title));

    _tcscat_s(CmdLine, _T(" \""));
	_tcscat_s(CmdLine, exe_title);
	_tcscat_s(CmdLine, _T("\""));
	_tcscat_s(CmdLine, bitnessStr);	
	_tcscat_s(CmdLine, UseAsio() ? (UseWasapi() ? _T(" S") : _T(" A")) : _T(" W"));

	if ( !CreateProcess( NULL, CmdLine, NULL, NULL, TRUE, 0, NULL, NULL, &siStartInfo, &piProcInfo ) )
	{
		process_terminate();
		return false;
	}

	// Close remote handles so pipes will break when process terminates
	CloseHandle( hChildStd_OUT_Wr );
	CloseHandle( hChildStd_IN_Rd );
	hChildStd_OUT_Wr = NULL;
	hChildStd_IN_Rd = NULL;

	hProcess = piProcInfo.hProcess;
	hThread = piProcInfo.hThread;

#ifdef NDEBUG
	SetPriorityClass( hProcess, GetPriorityClass( GetCurrentProcess() ) );
	//SetThreadPriority( hThread, GetThreadPriority( GetCurrentThread() ) );
	SetThreadPriority( hThread, THREAD_PRIORITY_TIME_CRITICAL);
#endif

	uint32_t code = process_read_code();

	if ( code != 0 )
	{
		process_terminate();
		return false;
	}

	uint32_t name_string_length = process_read_code();
	uint32_t vendor_string_length = process_read_code();
	uint32_t product_string_length = process_read_code();
	uVendorVersion = process_read_code();
	uUniqueId = process_read_code();
	uNumOutputs = process_read_code();

	delete [] sName;
	delete [] sVendor;
	delete [] sProduct;

	sName = new char[ name_string_length + 1 ];
	sVendor = new char[ vendor_string_length + 1 ];
	sProduct = new char[ product_string_length + 1 ];

	process_read_bytes( sName, name_string_length );
	process_read_bytes( sVendor, vendor_string_length );
	process_read_bytes( sProduct, product_string_length );

	sName[ name_string_length ] = 0;
	sVendor[ vendor_string_length ] = 0;
	sProduct[ product_string_length ] = 0;

	//Always keep SC-VA loaded even when ports are closed because of huge loading times. For other plugins this setting is optional.
	IsSCVA = (uUniqueId == (uint32_t)'scva');

	return true;
}

void VSTDriver::process_terminate()
{
	if ( hProcess )
	{
		process_write_code( Command::Exit );
		WaitForSingleObject( hProcess, 5000 );
		TerminateProcess( hProcess, 0 );
		CloseHandle( hThread );
		CloseHandle( hProcess );
	}
	if ( hChildStd_IN_Rd ) CloseHandle( hChildStd_IN_Rd );
	if ( hChildStd_IN_Wr ) CloseHandle( hChildStd_IN_Wr );
	if ( hChildStd_OUT_Rd ) CloseHandle( hChildStd_OUT_Rd );
	if ( hChildStd_OUT_Wr ) CloseHandle( hChildStd_OUT_Wr );
	if ( hReadEvent ) CloseHandle( hReadEvent );
	if ( bInitialized && !bInitializedOtherModel ) CoUninitialize();
	bInitialized = false;
	bInitializedOtherModel = false;
	hProcess = NULL;
	hThread = NULL;
	hReadEvent = NULL;
	hChildStd_IN_Rd = NULL;
	hChildStd_IN_Wr = NULL;
	hChildStd_OUT_Rd = NULL;
	hChildStd_OUT_Wr = NULL;
}

bool VSTDriver::process_running()
{
	if ( hProcess && WaitForSingleObject( hProcess, 0 ) == WAIT_TIMEOUT ) return true;
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

uint32_t VSTDriver::process_read_bytes_pass( void * out, uint32_t size )
{
	OVERLAPPED ol = {};
	ol.hEvent = hReadEvent;
	ResetEvent( hReadEvent );
	DWORD bytesDone;
	SetLastError( NO_ERROR );
	if ( ReadFile( hChildStd_OUT_Rd, out, size, &bytesDone, &ol ) ) return bytesDone;
	if ( GetLastError() != ERROR_IO_PENDING ) return 0;

	const HANDLE handles[1] = {hReadEvent};
	SetLastError( NO_ERROR );
	DWORD state;
	for (;;)
	{
		state = MsgWaitForMultipleObjects( _countof( handles ), handles, FALSE, INFINITE, QS_ALLEVENTS );
		if ( state == WAIT_OBJECT_0 + _countof( handles ) ) ProcessPendingMessages(editorOwner);
		else break;
	}

	if ( state == WAIT_OBJECT_0 && GetOverlappedResult( hChildStd_OUT_Rd, &ol, &bytesDone, TRUE ) ) return bytesDone;

#if 0 && _WIN32_WINNT >= 0x600
	CancelIoEx( hChildStd_OUT_Rd, &ol );
#else
	CancelIo( hChildStd_OUT_Rd );
#endif

	return 0;
}

void VSTDriver::process_read_bytes( void * out, uint32_t size )
{
	if ( process_running() && size )
	{
		uint8_t * ptr = (uint8_t *) out;
		uint32_t done = 0;
		while ( done < size )
		{
			uint32_t delta = process_read_bytes_pass( ptr + done, size - done );
			if ( delta == 0 )
			{
				memset( out, 0xFF, size );
				break;
			}
			done += delta;
		}
	}
	else memset( out, 0xFF, size );
}

uint32_t VSTDriver::process_read_code()
{
	uint32_t code;
	process_read_bytes( &code, sizeof(code) );
	return code;
}

void VSTDriver::process_write_bytes( const void * in, uint32_t size )
{
	if ( process_running() )
	{
		if ( size == 0 ) return;
		DWORD bytesWritten;
		if ( !WriteFile( hChildStd_IN_Wr, in, size, &bytesWritten, NULL ) || bytesWritten < size ) process_terminate();
	}
}

void VSTDriver::process_write_code( uint32_t code )
{
	process_write_bytes( &code, sizeof(code) );
}

void VSTDriver::getEffectName(std::string & out)
{
	out = sName;
}

void VSTDriver::getVendorString(std::string & out)
{
	out = sVendor;
}

void VSTDriver::getProductString(std::string & out)
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

void VSTDriver::getChunk( std::vector<uint8_t> & out )
{
	process_write_code( Command::GetChunkData );

	uint32_t code = process_read_code();

	if ( code == NoError )
	{
		uint32_t size = process_read_code();

		out.resize( size );

		process_read_bytes( &out[0], size );
	}
	else process_terminate();
}

void VSTDriver::setChunk( const void * in, unsigned size )
{
	process_write_code( Command::SetChunkData );
	process_write_code( size );
	process_write_bytes( in, size );
	uint32_t code = process_read_code();
	if ( code != NoError ) process_terminate();
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
	process_write_code( Command::SetSampleRate );
	process_write_code( sizeof(uint32_t ) );
	process_write_code( sampleRate );

	uint32_t code = process_read_code();
	if (code != NoError ) {
		process_terminate();		
	}
}

void VSTDriver::displayEditorModal(unsigned int uDeviceID, HWND owner)
{
	uint32_t code = uDeviceID == 255 ? Command::DisplayEditorModal : Command::DisplayEditorModalThreaded;
	
	editorOwner = owner;

	if (uDeviceID == 0) Sleep(50);
	else if (uDeviceID == 1) Sleep(75);
	process_write_code( code );

	if (code == Command::DisplayEditorModalThreaded) {		
		process_write_code( uDeviceID );
	}

	code = process_read_code();
	if ( code != NoError ) process_terminate();
}

void VSTDriver::CloseVSTDriver() {
	process_terminate();

	if ( szPluginPath ) {
		free( szPluginPath );

		szPluginPath = NULL;
	}
}

BOOL VSTDriver::OpenVSTDriver(TCHAR * szPath, int sampleRate) {
	CloseVSTDriver();

	load_settings(szPath);

	if ( process_create() ) {
		process_write_code( Command::SetSampleRate );
		process_write_code( sizeof(uint32_t ) );
		process_write_code( sampleRate );

		uint32_t code = process_read_code();
		if ( code != NoError ) {
			process_terminate();
			return FALSE;
		}

		if (blChunk.size()) {
			process_write_code( Command::SetChunkData );
			process_write_code( (uint32_t)blChunk.size() );
			if (blChunk.size()) process_write_bytes( &blChunk.front(), (uint32_t)blChunk.size() );
			code = process_read_code();
			if ( code != NoError ) {
				process_terminate();
				return FALSE;
			}

		}		

		return TRUE;
	}

	return FALSE;	
}

void VSTDriver::ResetDriver(unsigned int uDeviceID) {

	process_write_code( Command::Reset );
	process_write_code( uDeviceID );
	uint32_t code = process_read_code();
	if ( code != NoError ) process_terminate();
}

void VSTDriver::ProcessMIDIMessage(DWORD dwPort, DWORD dwParam1) {
	dwParam1 = ( dwParam1 & 0xFFFFFF ) | ( dwPort << 24 );
	process_write_code( Command::SendMidiEvent );
	process_write_code( dwParam1 );

	uint32_t code = process_read_code();
	if ( code != NoError ) process_terminate();
}

void VSTDriver::ProcessSysEx(DWORD dwPort, const unsigned char *sysexbuffer,int exlen) {
	dwPort = ( dwPort << 24 ) | ( exlen & 0xFFFFFF );
	process_write_code( Command::SendMidiSysExEvent );
	process_write_code( dwPort );
	process_write_bytes( sysexbuffer, exlen );

	uint32_t code = process_read_code();
	if ( code != NoError ) process_terminate();
}

uint32_t VSTDriver::RenderFloatInternal(float * samples, int len, float volume, WORD channels) {
	uint32_t opCode = channels == 2 ? Command::RenderAudioSamples : Command::RenderAudioSamples4channel; 
	process_write_code( opCode );
	process_write_code( len );	

	uint32_t code = process_read_code();	
	if ( code != NoError ) {
		process_terminate();
		memset( samples, 0, sizeof(*samples) * len * uNumOutputs * channels / 2);
		return code;
	}
	
	process_read_bytes( samples, sizeof(*samples) * len * uNumOutputs * channels / 2);
	
	if (volume != 1.0f)
	{
		unsigned int tmp = len * uNumOutputs * channels / 2;
		for ( unsigned int i = 0; i < tmp; i++ )
			samples[ i ] *= volume;
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

uint32_t VSTDriver::Render(short * samples, int len, float volume, WORD channels)
{
#pragma warning(disable:6385) //false buffer alarms
#pragma warning(disable:6386) 

	float * float_out = (float *) _alloca((UINT64)(BUFFER_SIZE / 8) * uNumOutputs * channels / 2 * sizeof(*float_out) );
	while ( len > 0 )
	{
		int len_todo = len > (BUFFER_SIZE / 8) ? (BUFFER_SIZE / 8) : len;
		int result = RenderFloatInternal( float_out, len_todo, volume, channels );
		unsigned int tmp = len_todo * uNumOutputs * channels / 2;
		for ( unsigned int i = 0; i < tmp; i++ )
		{
			int sample = (int)( float_out[i] * 32768.f );
			if ( ( sample + 0x8000 ) & 0xFFFF0000 ) sample = 0x7FFF ^ (sample >> 31);
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
