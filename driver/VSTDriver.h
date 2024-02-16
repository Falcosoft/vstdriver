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

#ifndef __VSTDRIVER_H__
#define __VSTDRIVER_H__

#include <windows.h>
#include <malloc.h>
#include <stdio.h>
#include <tchar.h>
#include "../external_packages/aeffect.h"
#include "../external_packages/aeffectx.h"
#if (defined(_MSC_VER) && (_MSC_VER < 1600))
#include "../common/backport_cstdint"
#else
#include <cstdint>
#endif
#include <string>
#include <vector>

#ifndef UNICODE  
typedef std::string tstring;
#else
typedef std::wstring tstring;
#endif

UINT GetWaveOutDeviceId();
bool IsWinNT4();
bool IsVistaOrNewer();
bool UseAsio();

const uint32_t NoError = 0;
const uint32_t ResetRequest = 255;

class VSTDriver {
private:
	TCHAR      * szPluginPath;
	unsigned     uPluginPlatform;

	bool         bInitialized;
	bool         bInitializedOtherModel;
	bool		 IsSCVA;
	HANDLE       hProcess;
	HANDLE       hThread;
	HANDLE       hReadEvent;
	HANDLE       hChildStd_IN_Rd;
	HANDLE       hChildStd_IN_Wr;
	HANDLE       hChildStd_OUT_Rd;
	HANDLE       hChildStd_OUT_Wr;

	std::vector<std::uint8_t> blChunk;

	unsigned uNumOutputs;

	char       * sName;
	char       * sVendor;
	char       * sProduct;
	uint32_t     uVendorVersion;
	uint32_t     uUniqueId;

	unsigned test_plugin_platform();
	bool connect_pipe( HANDLE hPipe );
	tstring GetVsthostPath();
	bool process_create();
	void process_terminate();
	bool process_running();
	uint32_t process_read_code();
	void process_read_bytes( void * buffer, uint32_t size );
	uint32_t process_read_bytes_pass( void * buffer, uint32_t size );
	void process_write_code( uint32_t code );
	void process_write_bytes( const void * buffer, uint32_t size );
	uint32_t RenderFloatInternal(float* samples, int len, float volume, WORD channels);

	void load_settings(TCHAR * szPath);

public:	
	VSTDriver();
	~VSTDriver();
	void CloseVSTDriver();
	BOOL OpenVSTDriver(TCHAR * szPath = NULL, int sampleRate = 48000);
	void ResetDriver(unsigned int uDeviceID);
	void ProcessMIDIMessage(DWORD dwPort, DWORD dwParam1);
	void ProcessSysEx(DWORD dwPort, const unsigned char *sysexbuffer, int exlen);
	uint32_t Render(short * samples, int len, float volume = 1.0f, WORD channels = 2);
	uint32_t RenderFloat(float * samples, int len, float volume = 1.0f, WORD channels = 2);

	void getEffectName(std::string & out);
	void getVendorString(std::string & out);
	void getProductString(std::string & out);
	long getVendorVersion();
	long getUniqueID();
	bool getIsSCVA() { return IsSCVA; };

	// configuration
	void getChunk(std::vector<uint8_t> & out);
	void setChunk( const void * in, unsigned size );

	// editor
	bool hasEditor();
	void displayEditorModal(unsigned int uDeviceID = 255);
	void setHighDpiMode(unsigned int modeNum = 0);
	void setSinglePort32ChMode();
	void initSysTray();
	void setSampleRate(unsigned int sampleRate);
};

#endif