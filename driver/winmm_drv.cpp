/* Copyright (C) 2003, 2004, 2005 Dean Beeler, Jerome Fisher
* Copyright (C) 2011, 2012 Dean Beeler, Jerome Fisher, Sergey V. Mikayev
* Copyright (C) 2023 Zoltan Bacsko - Falcosoft
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

#include "stdafx.h"
#include "../version.h"
#include <math.h>
#include <commdlg.h>

extern "C" { HINSTANCE hinst_vst_driver = NULL; }

#define MAX_DRIVERS 2
#define MAX_CLIENTS 8 // Per driver

static VSTMIDIDRV::MidiSynth& midiSynth = VSTMIDIDRV::MidiSynth::getInstance();
static bool synthOpened = false;
//static HWND hwnd = NULL;
static int driverCount;

static TCHAR* GetFileVersion(TCHAR* result, unsigned int buffSize)
{
	_tcscat_s(result, buffSize, _T("version: ") _T(stringify(VERSION_MAJOR)) _T(".") _T(stringify(VERSION_MINOR)) _T(".") _T(stringify(VERSION_PATCH)));
	return result;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved ){
	if (fdwReason == DLL_PROCESS_ATTACH){
		hinst_vst_driver = hinstDLL;
		//DisableThreadLibraryCalls(hinstDLL);
	}else if(fdwReason == DLL_PROCESS_DETACH){
		//;
		//if (synthOpened)
		//	midiSynth.Close();
	}
	return TRUE;
}

struct Driver {
	bool open;
	int clientCount;
	HDRVR hdrvr;
	DWORD volume;
	struct Client {
		bool allocated;
		DWORD_PTR instance;
		DWORD flags;
		DWORD_PTR callback;
		DWORD synth_instance;
	} clients[MAX_CLIENTS];
} drivers[MAX_DRIVERS];

EXTERN_C LRESULT WINAPI DriverProc(DWORD dwDriverID, HDRVR hdrvr, WORD wMessage, DWORD dwParam1, DWORD dwParam2) {

	TCHAR fileversionBuff[32] = _T("Driver ");

	switch(wMessage) {
	case DRV_LOAD:
		memset(drivers, 0, sizeof(drivers));
		driverCount = 0;
		return DRV_OK;
	case DRV_ENABLE:
		return DRV_OK;
	case DRV_OPEN:
		int driverNum;
		if (driverCount == MAX_DRIVERS) {
			return DRV_CANCEL;
		} else {
			for (driverNum = 0; driverNum < MAX_DRIVERS; driverNum++) {								
				if (!drivers[driverNum].open) break;				
			}			
		}
		
		if (driverNum == MAX_DRIVERS) return DRV_CANCEL;
		drivers[driverNum].open = true;
		drivers[driverNum].clientCount = 0;
		drivers[driverNum].hdrvr = hdrvr;
		drivers[driverNum].volume = 0xFFFFFFFF;
		driverCount++;
		return DRV_OK;
	case DRV_INSTALL:
	case DRV_PNPINSTALL:
		return DRV_OK;
	case DRV_QUERYCONFIGURE:
		return DRVCNF_OK;
	case DRV_CONFIGURE:	
		MessageBox((HWND)dwParam1, GetFileVersion(fileversionBuff, _countof(fileversionBuff)), _T("VST MIDI Driver (Falcomod)"), MB_OK | MB_ICONINFORMATION);
		return DRVCNF_OK;
	case DRV_CLOSE:
		for (int i = 0; i < MAX_DRIVERS; i++) {
			if (drivers[i].open && drivers[i].hdrvr == hdrvr) {
				drivers[i].open = false;
				--driverCount;
				return DRV_OK;
			}
		}
		return DRV_CANCEL;
	case DRV_DISABLE:
	case DRV_REMOVE:		
	case DRV_FREE:
		if (synthOpened)
		{
			midiSynth.Close(true);
			synthOpened = false;			
		}
		return DRV_OK;	
	}
	return DRV_OK;
}


HRESULT modGetCaps(UINT uDeviceID, PVOID capsPtr, DWORD capsSize) {
	
	//Rewritten to give unique GUIDs for vstmidiproxy and to fix Winamp Midi plugin 3.x ANSI/Unicode bug:
	//It seems under unicode WinNT you never get a real ANSI request even form ANSI Midi clients. WinMM handles conversion internally.
	//If capsSize accidentally equals an ANSI structure size (MIDIOUTCAPSA, MIDIOUTCAPS2A) the request still expects wide strings. 
	
	static MIDIOUTCAPS2 myCaps2[MAX_DRIVERS] = { 0 };	
	static const TCHAR synthName[] = _T("VST MIDI Synth (port X)");	
		
	if (!myCaps2[uDeviceID].wMid)
	{
		myCaps2[uDeviceID].wMid = MM_UNMAPPED;
		myCaps2[uDeviceID].wPid = MM_MPU401_MIDIOUT;
		myCaps2[uDeviceID].wTechnology = MOD_MIDIPORT;
		myCaps2[uDeviceID].vDriverVersion = VERSION_MAJOR << 8 | VERSION_MINOR;
		myCaps2[uDeviceID].wVoices = 0;
		myCaps2[uDeviceID].wNotes = 0;
		myCaps2[uDeviceID].wChannelMask = 0xffff;
		myCaps2[uDeviceID].dwSupport = MIDICAPS_VOLUME;
		
		_tcscpy_s(myCaps2[uDeviceID].szPname, synthName);
		unsigned portCharPos = (unsigned)(_tcschr(synthName, 'X') - synthName);
		myCaps2[uDeviceID].szPname[portCharPos] = _T('A') + static_cast<TCHAR>(uDeviceID);
	  
		//MIDIOUTCAPS2 extra
		myCaps2[uDeviceID].ManufacturerGuid = VSTMidiDrvManufacturerGuid;
		myCaps2[uDeviceID].ProductGuid = uDeviceID ? VSTMidiDrvPortBGuid : VSTMidiDrvPortAGuid;
	}

	memcpy(capsPtr, &myCaps2[uDeviceID], min(sizeof(MIDIOUTCAPS2), capsSize));
	return MMSYSERR_NOERROR;	
	
}

void DoCallback(int driverNum, DWORD_PTR clientNum, DWORD msg, DWORD_PTR param1, DWORD_PTR param2) {
	Driver::Client *client = &drivers[driverNum].clients[clientNum];
	DriverCallback(client->callback, client->flags, drivers[driverNum].hdrvr, msg, client->instance, param1, param2);
}

LONG OpenDriver(Driver *driver, UINT uDeviceID, UINT uMsg, DWORD_PTR dwUser, DWORD_PTR dwParam1, DWORD_PTR dwParam2) {
	int clientNum;
	if (driver->clientCount == 0) {
		clientNum = 0;
	} else if (driver->clientCount == MAX_CLIENTS) {
		return MMSYSERR_ALLOCATED;
	} else {
		int i;
		for (i = 0; i < MAX_CLIENTS; i++) {				
			if (!driver->clients[i].allocated) break;			
		}

		if (i == MAX_CLIENTS) return MMSYSERR_ALLOCATED;
		clientNum = i;
	}
	MIDIOPENDESC *desc = (MIDIOPENDESC *)dwParam1;
	driver->clients[clientNum].allocated = true;
	driver->clients[clientNum].flags = HIWORD(dwParam2);
	driver->clients[clientNum].callback = desc->dwCallback;
	driver->clients[clientNum].instance = desc->dwInstance;
	*(LONG *)dwUser = clientNum;
	driver->clientCount++;
	DoCallback(uDeviceID, clientNum, MOM_OPEN, NULL, NULL);
	return MMSYSERR_NOERROR;
}

LONG CloseDriver(Driver *driver, UINT uDeviceID, UINT uMsg, DWORD_PTR dwUser, DWORD_PTR dwParam1, DWORD_PTR dwParam2) {
	if (!driver->clients[dwUser].allocated) {
		return MMSYSERR_INVALPARAM;
	}
	driver->clients[dwUser].allocated = false;
	driver->clientCount--;
	DoCallback(uDeviceID, dwUser, MOM_CLOSE, NULL, NULL);
	return MMSYSERR_NOERROR;
}

EXTERN_C DWORD WINAPI modMessage(DWORD uDeviceID, DWORD uMsg, DWORD_PTR dwUser, DWORD_PTR dwParam1, DWORD_PTR dwParam2) {
	
	if (uDeviceID >= MAX_DRIVERS) return MMSYSERR_BADDEVICEID;

	MIDIHDR *midiHdr;
	Driver *driver = &drivers[uDeviceID];
	DWORD instance;
	DWORD res;
	
	switch (uMsg) {
	case MODM_OPEN:
		if (!synthOpened) {
			if (midiSynth.Init(uDeviceID) != 0) return MMSYSERR_ERROR;
			synthOpened = true;			
		}
		else if (!drivers[uDeviceID].clientCount) {
			midiSynth.InitDialog(uDeviceID);
		}

		instance = NULL;		

		res = OpenDriver(driver, uDeviceID, uMsg, dwUser, dwParam1, dwParam2);
		driver->clients[*(LONG *)dwUser].synth_instance = instance;
		return res;

	case MODM_CLOSE:
		if (driver->clients[dwUser].allocated == false) {
			return MMSYSERR_ERROR;
		}

		res = CloseDriver(driver, uDeviceID, uMsg, dwUser, dwParam1, dwParam2);
		if (synthOpened) 
		{
			if(!drivers[uDeviceID].clientCount) midiSynth.Reset(uDeviceID);
						
			int clientCounts = 0;
			for (int driverNum = 0; driverNum < MAX_DRIVERS; driverNum++) {
			clientCounts += drivers[driverNum].clientCount;
			if (clientCounts) break;
			}

			if(!clientCounts) {
				TCHAR exe_path[MAX_PATH] = {0};
				TCHAR exe_title[MAX_PATH / 2] = {0};
				
				GetModuleFileName(NULL, exe_path, _countof(exe_path));
				GetFileTitle(exe_path, exe_title, _countof(exe_title));
				
				//Always close fully if client is explorer. This can happen in Windows 2000's sidebar preview.
				bool forceUnload = !_tcsicmp(exe_title,_T("Explorer.exe")) || !_tcsicmp(exe_title,_T("Explorer"));
				
				midiSynth.Close(forceUnload);
				synthOpened = false;
			}		

		}
		return res;	

	case MODM_GETDEVCAPS:
		return modGetCaps(uDeviceID, (PVOID)dwParam1, (DWORD)dwParam2);

	case MODM_DATA:
		if (driver->clients[dwUser].allocated == false) {
			return MMSYSERR_ERROR;
		}
		midiSynth.PushMIDI(uDeviceID, (DWORD)dwParam1);
		return MMSYSERR_NOERROR;

	case MODM_LONGDATA:
		if (driver->clients[dwUser].allocated == false) {
			return MMSYSERR_ERROR;
		}
		midiHdr = (MIDIHDR *)dwParam1;
		if ((midiHdr->dwFlags & MHDR_PREPARED) == 0) {
			return MIDIERR_UNPREPARED;
		}
		midiSynth.PlaySysEx(uDeviceID, (unsigned char*)midiHdr->lpData, midiHdr->dwBufferLength);
		midiHdr->dwFlags |= MHDR_DONE;
		midiHdr->dwFlags &= ~MHDR_INQUEUE;
		DoCallback(uDeviceID, dwUser, MOM_DONE, dwParam1, NULL);
		return MMSYSERR_NOERROR;

	case MODM_GETVOLUME:
		*(DWORD*)dwParam1 = driver->volume;
		return MMSYSERR_NOERROR;	

	case MODM_SETVOLUME:
		driver->volume = (DWORD)dwParam1;	 
		midiSynth.SetVolume(uDeviceID, sqrt(float(LOWORD(dwParam1)) / 65535.f)); //falco: for separate port A/B note velocity
		return MMSYSERR_NOERROR;
	
	case MODM_RESET:
		for (int i = 0; i <= 15; i++)
		{
			DWORD msg = 0;
			msg = (0xB0 | i) | (0x40 << 8); //Sustain off
			midiSynth.PushMIDI(uDeviceID, msg);
			msg = (0xB0 | i) | (0x7B << 8); //All Notes off
			midiSynth.PushMIDI(uDeviceID, msg);
			msg = (0xB0 | i) | (0x79 << 8);  //All Controllers off
			midiSynth.PushMIDI(uDeviceID, msg);
			msg = (0xB0 | i) | (0x78 << 8);  //All Sounds off
			midiSynth.PushMIDI(uDeviceID, msg);
		}
		return MMSYSERR_NOERROR;

	case MODM_GETNUMDEVS:
		return MAX_DRIVERS;

	case MODM_PREPARE:
		return MMSYSERR_NOTSUPPORTED;

	case MODM_UNPREPARE:
		return MMSYSERR_NOTSUPPORTED;

	case MODM_CACHEPATCHES:
		return MMSYSERR_NOTSUPPORTED;

	case MODM_CACHEDRUMPATCHES:
		return MMSYSERR_NOTSUPPORTED;

	default:
		
		return MMSYSERR_NOERROR;
		break;
	}
}
