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
#include <cstdint>
#include <vector>

class VSTDriver {
private:
	TCHAR      * szPluginPath;
	unsigned     uPluginPlatform;

	bool         bInitialized;
	HANDLE       hProcess;
	HANDLE       hThread;
	HANDLE       hReadEvent;
	HANDLE       hChildStd;

	std::vector<std::uint8_t> blChunk;

	unsigned uNumOutputs;

	char       * sName;
	char       * sVendor;
	char       * sProduct;
	uint32_t     uVendorVersion;
	uint32_t     uUniqueId;

	unsigned test_plugin_platform();
	bool connect_pipe( HANDLE hPipe );
	bool process_create();
	void process_terminate();
	bool process_running();
	uint32_t process_read_code();
	void process_read_bytes( void * buffer, uint32_t size );
	uint32_t process_read_bytes_pass( void * buffer, uint32_t size );
	void process_write_code( uint32_t code );
	void process_write_bytes( const void * buffer, uint32_t size );

	void load_settings(TCHAR * szPath);

public:
	VSTDriver();
	~VSTDriver();
	void CloseVSTDriver();
	BOOL OpenVSTDriver(TCHAR * szPath = NULL);
	void ResetDriver();
	void ProcessMIDIMessage(DWORD dwPort, DWORD dwParam1);
	void ProcessSysEx(DWORD dwPort, const unsigned char *sysexbuffer, int exlen);
	void Render(short * samples, int len, float volume = 1.0f);
	void RenderFloat(float * samples, int len, float volume = 1.0f);

	void getEffectName(std::string & out);
	void getVendorString(std::string & out);
	void getProductString(std::string & out);
	long getVendorVersion();
	long getUniqueID();

	// configuration
	void getChunk(std::vector<uint8_t> & out);
	void setChunk( const void * in, unsigned size );

	// editor
	bool hasEditor();
	void displayEditorModal();
};

#endif