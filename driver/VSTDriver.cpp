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
 
enum Command : uint32_t
{
    Exit = 0,
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

const uint32_t NoError = 0;

VSTDriver::VSTDriver() {
	szPluginPath = NULL;
	bInitialized = false;
	bInitializedOtherModel = false;
	hProcess = NULL;
	hThread = NULL;
	hReadEvent = NULL;
    hChildStd_IN_Rd = NULL;
    hChildStd_IN_Wr = NULL;
    hChildStd_OUT_Rd = NULL;
    hChildStd_OUT_Wr = NULL;
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
			if ( szPath ) dwSize = (DWORD)(_tcslen( szPath ) * sizeof(TCHAR));
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
#if (defined(_MSC_VER) && (_MSC_VER < 1600))
				if (chunk_size) fread( &blChunk.front(), 1, chunk_size, f );
#else
				if (chunk_size) fread( blChunk.data(), 1, chunk_size, f );
#endif
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

std::wstring VSTDriver::GetVsthostPath()
{
	TCHAR my_path[MAX_PATH];
	GetModuleFileName( hinst_vst_driver, my_path, _countof(my_path) );

	std::wstring sDir(my_path);
    size_t idx = sDir.find_last_of( L'\\' ) + 1;
    if (idx != std::wstring::npos)
	  sDir.resize( idx );
    std::wstring sSubdir(my_path + idx);
    idx = sSubdir.find_last_of( L'.' );
    if (idx != std::wstring::npos)
      sSubdir.resize(idx);
    sSubdir += L'\\';
    std::wstring sFile((uPluginPlatform == 64) ? L"vsthost64.exe" : L"vsthost32.exe");

    std::wstring sHostPath = sDir + sFile;
    if (::GetFileAttributesW(sHostPath.c_str()) == INVALID_FILE_ATTRIBUTES)
      sHostPath = sDir + sSubdir + sFile;

	return sHostPath;
}

bool VSTDriver::process_create()
{
	if ( uPluginPlatform != 32 && uPluginPlatform != 64 ) return false;

	SECURITY_ATTRIBUTES saAttr;

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

	std::wstring pipe_name_in, pipe_name_out;
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

	std::wstring szCmdLine = L"\"";
    szCmdLine += GetVsthostPath();
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

	PROCESS_INFORMATION piProcInfo;
	STARTUPINFO siStartInfo = {0};

	siStartInfo.cb = sizeof(siStartInfo);
    siStartInfo.hStdInput = hChildStd_IN_Rd;
    siStartInfo.hStdOutput = hChildStd_OUT_Wr;
    siStartInfo.hStdError = GetStdHandle( STD_ERROR_HANDLE );
    siStartInfo.dwFlags |= STARTF_USESTDHANDLES;

	TCHAR CmdLine[MAX_PATH];
	_tcscpy_s(CmdLine, _countof(CmdLine), szCmdLine.c_str());

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
	SetThreadPriority( hThread, THREAD_PRIORITY_HIGHEST );
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
	if ( ReadFile( hChildStd_OUT_Rd, out, size, &bytesDone, &ol ) ) return bytesDone;
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

bool VSTDriver::hasEditor()
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
}

void VSTDriver::displayEditorModal(unsigned int uDeviceID)
{
	uint32_t code = uDeviceID == 255 ? Command::DisplayEditorModal : Command::DisplayEditorModalThreaded;
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
#if (defined(_MSC_VER) && (_MSC_VER < 1600))
			if (blChunk.size()) process_write_bytes( &blChunk.front(), (uint32_t)blChunk.size() );
#else
			if (blChunk.size()) process_write_bytes( blChunk.data(), (uint32_t)blChunk.size() );
#endif
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

void VSTDriver::RenderFloat(float * samples, int len, float volume, WORD channels) {
	uint32_t opCode = channels == 2 ? Command::RenderAudioSamples : Command::RenderAudioSamples4channel; 
	process_write_code( opCode );
	process_write_code( len );

	uint32_t code = process_read_code();
	if ( code != NoError ) {
		process_terminate();
		memset( samples, 0, sizeof(*samples) * len * uNumOutputs * channels / 2);
		return;
	}

	while ( len ) {
		unsigned len_to_do = len;
		if ( len_to_do > 4096 ) len_to_do = 4096;
		process_read_bytes( samples, sizeof(*samples) * len_to_do * uNumOutputs * channels / 2);
		for ( unsigned i = 0; i < len_to_do * uNumOutputs * channels / 2; i++ ) samples[ i ] *= volume;
		samples += len_to_do * uNumOutputs * channels / 2;
		len -= len_to_do;
	}
}

void VSTDriver::Render(short * samples, int len, float volume, WORD channels)
{
	float * float_out = (float *) _alloca( 512 * uNumOutputs * sizeof(*float_out) );
	while ( len > 0 )
	{
		int len_todo = len > 512 ? 512 : len;
		RenderFloat( float_out, len_todo, volume, channels );
		for ( unsigned int i = 0; i < len_todo * uNumOutputs; i++ )
		{
			int sample = (int)( float_out[i] * 32768.f );
			if ( ( sample + 0x8000 ) & 0xFFFF0000 ) sample = 0x7FFF ^ (sample >> 31);
			samples[0] = sample;
			samples++;
		}
		len -= len_todo;
	}
}
