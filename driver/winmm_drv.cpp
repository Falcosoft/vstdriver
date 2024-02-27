/* Copyright (C) 2003, 2004, 2005 Dean Beeler, Jerome Fisher
* Copyright (C) 2011, 2012 Dean Beeler, Jerome Fisher, Sergey V. Mikayev
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
#include <math.h>

extern "C" { HINSTANCE hinst_vst_driver = 0; }

#define MAX_DRIVERS 2
#define MAX_CLIENTS 8 // Per driver

static VSTMIDIDRV::MidiSynth& midiSynth = VSTMIDIDRV::MidiSynth::getInstance();
static bool synthOpened = false;
//static HWND hwnd = NULL;
static int driverCount;

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

#pragma comment(lib,"Version.lib") 
static TCHAR* GetFileVersion(TCHAR* result, unsigned int buffSize)
{
	DWORD               dwSize = 0;
	BYTE* pVersionInfo = NULL;
	VS_FIXEDFILEINFO* pFileInfo = NULL;
	UINT                pLenFileInfo = 0;
	TCHAR tmpBuff[MAX_PATH];

	GetModuleFileName(hinst_vst_driver, tmpBuff, MAX_PATH);

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
	MIDIOUTCAPSA * myCapsA;
	MIDIOUTCAPSW * myCapsW;
	MIDIOUTCAPS2A * myCaps2A;
	MIDIOUTCAPS2W * myCaps2W;

	CHAR synthName[] = "VST MIDI Synth\0";
	WCHAR synthNameW[] = L"VST MIDI Synth\0";

	CHAR synthPortA[] = " (port A)\0";
	WCHAR synthPortAW[] = L" (port A)\0";

	CHAR synthPortB[] = " (port B)\0";
	WCHAR synthPortBW[] = L" (port B)\0";

	switch (capsSize) {
	case (sizeof(MIDIOUTCAPSA)):
		myCapsA = (MIDIOUTCAPSA *)capsPtr;
		myCapsA->wMid = MM_UNMAPPED;
		myCapsA->wPid = MM_MPU401_MIDIOUT;
		memcpy(myCapsA->szPname, synthName, sizeof(synthName));
		memcpy(myCapsA->szPname + strlen(synthName), uDeviceID ? synthPortB : synthPortA, sizeof(synthPortA));
		myCapsA->wTechnology = MOD_MIDIPORT;
		myCapsA->vDriverVersion = 0x0090;
		myCapsA->wVoices = 0;
		myCapsA->wNotes = 0;
		myCapsA->wChannelMask = 0xffff;
		myCapsA->dwSupport = MIDICAPS_VOLUME;
		return MMSYSERR_NOERROR;

	case (sizeof(MIDIOUTCAPSW)):
		myCapsW = (MIDIOUTCAPSW *)capsPtr;
		myCapsW->wMid = MM_UNMAPPED;
		myCapsW->wPid = MM_MPU401_MIDIOUT;
		memcpy(myCapsW->szPname, synthNameW, sizeof(synthNameW));
		memcpy(myCapsW->szPname + wcslen(synthNameW), uDeviceID ? synthPortBW : synthPortAW, sizeof(synthPortAW));
		myCapsW->wTechnology = MOD_MIDIPORT;
		myCapsW->vDriverVersion = 0x0090;
		myCapsW->wVoices = 0;
		myCapsW->wNotes = 0;
		myCapsW->wChannelMask = 0xffff;
		myCapsW->dwSupport = MIDICAPS_VOLUME;
		return MMSYSERR_NOERROR;

	case (sizeof(MIDIOUTCAPS2A)):
		myCaps2A = (MIDIOUTCAPS2A *)capsPtr;
		myCaps2A->wMid = MM_UNMAPPED;
		myCaps2A->wPid = MM_MPU401_MIDIOUT;
		memcpy(myCaps2A->szPname, synthName, sizeof(synthName));
		memcpy(myCaps2A->szPname + strlen(synthName), uDeviceID ? synthPortB : synthPortA, sizeof(synthPortA));
		myCaps2A->wTechnology = MOD_MIDIPORT;
		myCaps2A->vDriverVersion = 0x0090;
		myCaps2A->wVoices = 0;
		myCaps2A->wNotes = 0;
		myCaps2A->wChannelMask = 0xffff;
		myCaps2A->dwSupport = MIDICAPS_VOLUME;
		return MMSYSERR_NOERROR;

	case (sizeof(MIDIOUTCAPS2W)):
		myCaps2W = (MIDIOUTCAPS2W *)capsPtr;
		myCaps2W->wMid = MM_UNMAPPED;
		myCaps2W->wPid = MM_MPU401_MIDIOUT;
		memcpy(myCaps2W->szPname, synthNameW, sizeof(synthNameW));
		memcpy(myCaps2W->szPname + wcslen(synthNameW), uDeviceID ? synthPortBW : synthPortAW, sizeof(synthPortAW));
		myCaps2W->wTechnology = MOD_MIDIPORT;
		myCaps2W->vDriverVersion = 0x0090;
		myCaps2W->wVoices = 0;
		myCaps2W->wNotes = 0;
		myCaps2W->wChannelMask = 0xffff;
		myCaps2W->dwSupport = MIDICAPS_VOLUME;
		return MMSYSERR_NOERROR;

	default:
		return MMSYSERR_ERROR;
	}
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
				midiSynth.Close(false);
				synthOpened = false;
			}		

		}
		return res;

	case MODM_PREPARE:
		return MMSYSERR_NOTSUPPORTED;

	case MODM_UNPREPARE:
		return MMSYSERR_NOTSUPPORTED;

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

	default:
		
		return MMSYSERR_NOERROR;
		break;
	}
}
