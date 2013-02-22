/* Copyright (C) 2011 Chris Moeller, Brad Miller
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

 #include <assert.h>

VSTDriver::VSTDriver() {
	szPluginPath = NULL;
	bInitialized = false;
	hProcess = NULL;
	hThread = NULL;
	hReadEvent = NULL;
	hChildStd = NULL;
	uNumOutputs = 0;
	sName = NULL;
	sVendor = NULL;
	sProduct = NULL;
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
	long lResult;
	DWORD dwType=REG_SZ;
	DWORD dwSize=0;
	if ( szPath || RegOpenKeyEx(HKEY_CURRENT_USER, _T("Software\\VSTi Driver"),0,KEY_READ|KEY_WOW64_32KEY,&hKey) == ERROR_SUCCESS ) {
		if ( !szPath ) lResult = RegQueryValueEx(hKey, _T("plugin"), NULL, &dwType, NULL, &dwSize);
		if ( szPath || ( lResult == ERROR_SUCCESS && dwType == REG_SZ ) ) {
			if ( szPath ) dwSize = _tcslen( szPath ) * sizeof(TCHAR);
			szPluginPath = (TCHAR*) calloc( dwSize + sizeof(TCHAR), 1 );
			if ( szPath ) _tcscpy( szPluginPath, szPath );
			else RegQueryValueEx(hKey, _T("plugin"), NULL, &dwType, (LPBYTE) szPluginPath, &dwSize);

			uPluginPlatform = test_plugin_platform();

			blChunk.resize( 0 );

			const TCHAR * dot = _tcsrchr( szPluginPath, _T('.') );
			if ( !dot ) dot = szPluginPath + _tcslen( szPluginPath );
			TCHAR * szSettingsPath = ( TCHAR * ) _alloca( ( dot - szPluginPath + 5 ) * sizeof( TCHAR ) );
			_tcsncpy( szSettingsPath, szPluginPath, dot - szPluginPath );
			_tcscpy( szSettingsPath + ( dot - szPluginPath ), _T(".set") );

			FILE * f;
			errno_t err = _tfopen_s( &f, szSettingsPath, _T("rb") );
			if ( !err )
			{
				fseek( f, 0, SEEK_END );
				size_t chunk_size = ftell( f );
				fseek( f, 0, SEEK_SET );
				blChunk.resize( chunk_size );
				fread( blChunk.data(), 1, chunk_size, f );
				fclose( f );
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

static void print_hex(unsigned val,std::wstring &out,unsigned bytes)
{
	unsigned n;
	for(n=0;n<bytes;n++)
	{
		unsigned char c = (unsigned char)((val >> ((bytes - 1 - n) << 3)) & 0xFF);
		out += print_hex_digit( c >> 4 );
		out += print_hex_digit( c & 0xF );
	}
}

static void print_guid(const GUID & p_guid, std::wstring &out)
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

static bool create_pipe_name( std::wstring & out )
{
	GUID guid;
	if ( FAILED( CoCreateGuid( &guid ) ) ) return false;

	out = L"\\\\.\\pipe\\";
	print_guid( guid, out );

	return true;
}

bool VSTDriver::connect_pipe( HANDLE hPipe )
{
	OVERLAPPED ol = {};
	ol.hEvent = hReadEvent;
	ResetEvent( hReadEvent );
	if ( !ConnectNamedPipe( hPipe, &ol ) )
	{
		DWORD error = GetLastError();
		if ( error == ERROR_PIPE_CONNECTED ) return true;
		if ( error != ERROR_IO_PENDING ) return false;

		if ( WaitForSingleObject( hReadEvent, 10000 ) == WAIT_TIMEOUT ) return false;
	}
	return true;
}

extern "C" { extern HINSTANCE hinst_vst_driver; };

bool VSTDriver::process_create()
{
	if ( uPluginPlatform != 32 && uPluginPlatform != 64 ) return false;

	SECURITY_ATTRIBUTES saAttr;

	saAttr.nLength = sizeof(saAttr);
	saAttr.bInheritHandle = TRUE;
	saAttr.lpSecurityDescriptor = NULL;

	if ( !bInitialized )
	{
		if ( FAILED( CoInitialize( NULL ) ) ) return false;
		bInitialized = true;
	}

	hReadEvent = CreateEvent( NULL, TRUE, FALSE, NULL );

	std::wstring pipe_name;
	if ( !create_pipe_name( pipe_name ) )
	{
		process_terminate();
		return false;
	}

	HANDLE hPipe = CreateNamedPipe( pipe_name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE, 1, 65536, 65536, 0, &saAttr );
	DuplicateHandle( GetCurrentProcess(), hPipe, GetCurrentProcess(), &hChildStd, 0, FALSE, DUPLICATE_SAME_ACCESS );

	std::wstring szCmdLine = L"\"";

	TCHAR my_path[MAX_PATH];
	GetModuleFileName( hinst_vst_driver, my_path, _countof(my_path) );

	szCmdLine += my_path;
	szCmdLine.resize( szCmdLine.find_last_of( '\\' ) + 1 );
	szCmdLine += (uPluginPlatform == 64) ? L"vsthost64.exe" : L"vsthost32.exe";
	szCmdLine += L"\" \"";
	szCmdLine += szPluginPath;
	szCmdLine += L"\" ";

	unsigned sum = 0;

	const TCHAR * ch = szPluginPath;
	while ( *ch )
	{
		sum += (TCHAR)( *ch++ * 820109 );
	}

	print_hex( sum, szCmdLine, 4 );

	szCmdLine += L" ";
	szCmdLine += pipe_name.c_str() + 9;

	PROCESS_INFORMATION piProcInfo;
	STARTUPINFO siStartInfo = {0};

	siStartInfo.cb = sizeof(siStartInfo);

	TCHAR CmdLine[MAX_PATH];
	_tcscpy_s(CmdLine, _countof(CmdLine), szCmdLine.c_str());

	if ( !CreateProcess( NULL, CmdLine, NULL, NULL, TRUE, 0, NULL, NULL, &siStartInfo, &piProcInfo ) )
	{
		process_terminate();
		return false;
	}

	hProcess = piProcInfo.hProcess;
	hThread = piProcInfo.hThread;

#ifdef NDEBUG
	SetPriorityClass( hProcess, GetPriorityClass( GetCurrentProcess() ) );
	SetThreadPriority( hThread, GetThreadPriority( GetCurrentThread() ) );
#endif

	if ( !connect_pipe( hChildStd ) )
	{
		process_terminate();
		return false;
	}

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

	return true;
}

void VSTDriver::process_terminate()
{
	if ( hProcess )
	{
		process_write_code( 0 );
		WaitForSingleObject( hProcess, 5000 );
		TerminateProcess( hProcess, 0 );
		CloseHandle( hThread );
		CloseHandle( hProcess );
	}
	if ( hChildStd ) CloseHandle( hChildStd );
	if ( hReadEvent ) CloseHandle( hReadEvent );
	if ( bInitialized ) CoUninitialize();
	bInitialized = false;
	hProcess = NULL;
	hThread = NULL;
	hReadEvent = NULL;
	hChildStd = NULL;
}

bool VSTDriver::process_running()
{
	if ( hProcess && WaitForSingleObject( hProcess, 0 ) == WAIT_TIMEOUT ) return true;
	return false;
}

static void ProcessPendingMessages()
{
	MSG msg = {};
	while ( PeekMessage( &msg, NULL, 0, 0, PM_REMOVE ) ) DispatchMessage( &msg );
}

uint32_t VSTDriver::process_read_bytes_pass( void * out, uint32_t size )
{
	OVERLAPPED ol = {};
	ol.hEvent = hReadEvent;
	ResetEvent( hReadEvent );
	DWORD bytesDone;
	SetLastError( NO_ERROR );
	if ( ReadFile( hChildStd, out, size, &bytesDone, &ol ) ) return bytesDone;
	if ( GetLastError() != ERROR_IO_PENDING ) return 0;

	const HANDLE handles[1] = {hReadEvent};
	SetLastError( NO_ERROR );
	DWORD state;
	for (;;)
	{
		state = MsgWaitForMultipleObjects( _countof( handles ), handles, FALSE, INFINITE, QS_ALLEVENTS );
		if ( state == WAIT_OBJECT_0 + _countof( handles ) ) ProcessPendingMessages();
		else break;
	}

	if ( state == WAIT_OBJECT_0 && GetOverlappedResult( hChildStd, &ol, &bytesDone, TRUE ) ) return bytesDone;

#if 0 && _WIN32_WINNT >= 0x600
	CancelIoEx( hChildStd, &ol );
#else
	CancelIo( hChildStd );
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
		if ( !WriteFile( hChildStd, in, size, &bytesWritten, NULL ) || bytesWritten < size ) process_terminate();
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
	process_write_code( 1 );

	uint32_t code = process_read_code();

	if ( code == 0 )
	{
		uint32_t size = process_read_code();

		out.resize( size );

		process_read_bytes( &out[0], size );
	}
	else process_terminate();
}

void VSTDriver::setChunk( const void * in, unsigned size )
{
	process_write_code( 2 );
	process_write_code( size );
	process_write_bytes( in, size );
	uint32_t code = process_read_code();
	if ( code != 0 ) process_terminate();
}

bool VSTDriver::hasEditor()
{
	process_write_code( 3 );
	uint32_t code = process_read_code();
	if ( code != 0 )
	{
		process_terminate();
		return false;
	}
	code = process_read_code();
	return code != 0;
}

void VSTDriver::displayEditorModal()
{
	process_write_code( 4 );
	uint32_t code = process_read_code();
	if ( code != 0 ) process_terminate();
}

void VSTDriver::CloseVSTDriver() {
	process_terminate();

	if ( szPluginPath ) {
		free( szPluginPath );

		szPluginPath = NULL;
	}
}

BOOL VSTDriver::OpenVSTDriver(TCHAR * szPath) {
	CloseVSTDriver();

	load_settings(szPath);

	if ( process_create() ) {
		process_write_code( 5 );
		process_write_code( sizeof(uint32_t ) );
		process_write_code( 44100 );

		uint32_t code = process_read_code();
		if ( code != 0 ) {
			process_terminate();
			return FALSE;
		}

		process_write_code( 2 );
		process_write_code( blChunk.size() );
		process_write_bytes( blChunk.data(), blChunk.size() );

		code = process_read_code();
		if ( code != 0 ) {
			process_terminate();
			return FALSE;
		}

		return TRUE;
	}

	return FALSE;	
}

void VSTDriver::ResetDriver() {
	process_write_code( 6 );
	uint32_t code = process_read_code();
	if ( code != 0 ) process_terminate();
}

void VSTDriver::ProcessMIDIMessage(DWORD dwPort, DWORD dwParam1) {
	dwParam1 = ( dwParam1 & 0xFFFFFF ) | ( dwPort << 24 );
	process_write_code( 7 );
	process_write_code( dwParam1 );

	uint32_t code = process_read_code();
	if ( code != 0 ) process_terminate();
}

void VSTDriver::ProcessSysEx(DWORD dwPort, const unsigned char *sysexbuffer,int exlen) {
	dwPort = ( dwPort << 24 ) | ( exlen & 0xFFFFFF );
	process_write_code( 8 );
	process_write_code( dwPort );
	process_write_bytes( sysexbuffer, exlen );

	uint32_t code = process_read_code();
	if ( code != 0 ) process_terminate();
}

void VSTDriver::RenderFloat(float * samples, int len, float volume) {
	process_write_code( 9 );
	process_write_code( len );

	uint32_t code = process_read_code();
	if ( code != 0 ) {
		process_terminate();
		memset( samples, 0, sizeof(*samples) * len * uNumOutputs );
		return;
	}

	while ( len ) {
		unsigned len_to_do = len;
		if ( len_to_do > 4096 ) len_to_do = 4096;
		process_read_bytes( samples, sizeof(*samples) * len_to_do * uNumOutputs );
		for ( unsigned i = 0; i < len_to_do * uNumOutputs; i++ ) samples[ i ] *= volume;
		samples += len_to_do * uNumOutputs;
		len -= len_to_do;
	}
}

void VSTDriver::Render(short * samples, int len, float volume)
{
	float * float_out = (float *) _alloca( 512 * uNumOutputs * sizeof(*float_out) );
	while ( len > 0 )
	{
		int len_todo = len > 512 ? 512 : len;
		RenderFloat( float_out, len_todo, volume );
		for ( int i = 0; i < len_todo * uNumOutputs; i++ )
		{
			int sample = ( float_out[i] * 32768.f );
			if ( ( sample + 0x8000 ) & 0xFFFF0000 ) sample = 0x7FFF ^ (sample >> 31);
			samples[0] = sample;
			samples++;
		}
		len -= len_todo;
	}
}
