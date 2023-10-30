// cplwrapper.cpp : Defines the entry point for the DLL application.
// Falco: This cpl file is only relevant for Win NT4 which cannot handle exe files as control panel items. This cpl item simply invokes vstmididrvcfg.exe.  
// It is hardcoded to link with msvcrt.lib from VC++ 6.

#include "stdafx.h"
#include "cplwrapper.h"
#include <cpl.h>
#include <shellapi.h>

BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
					 )
{
	switch (ul_reason_for_call)
	{
	case DLL_PROCESS_ATTACH:
	case DLL_THREAD_ATTACH:
	case DLL_THREAD_DETACH:
	case DLL_PROCESS_DETACH:
		break;
	}
    return TRUE;
}

extern "C" LONG APIENTRY CPlApplet(
	HWND hwndCPL,       // handle of Control Panel window
	UINT uMsg,          // message
	LONG_PTR lParam1,       // first message parameter
	LONG_PTR lParam2        // second message parameter
)
{
	
	LPCPLINFO lpCPlInfo;
	LONG retCode = 0;

	switch (uMsg)
	{
	case CPL_INIT:              // first message, sent once
		return TRUE;

	case CPL_GETCOUNT:          // second message, sent once
		return 1L;                // (LONG)NUM_APPLETS;

	case CPL_INQUIRE:        // third message, sent once per app
		lpCPlInfo = (LPCPLINFO)lParam2;

		lpCPlInfo->idIcon = IDI_VST_CPL;
		lpCPlInfo->idName = IDS_VST_CPL_NAME;
		lpCPlInfo->idInfo = IDS_VST_CPL_DESCRIPTION;
		lpCPlInfo->lData = 0L;
		break;

	case CPL_DBLCLK:            // application icon double-clicked
	{
		wchar_t spath [MAX_PATH] = {0};
		GetSystemDirectory(spath, MAX_PATH);
		lstrcat(spath, L"\\vstmididrv\\vstmididrvcfg.exe");
		ShellExecute(hwndCPL, L"open", spath, NULL, NULL, SW_SHOWNORMAL);	
		
	}
	break;
	}
	return retCode;
}







