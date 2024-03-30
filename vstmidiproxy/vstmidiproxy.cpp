// Copyright (C) 2024 Zoltán Bacskó - Falcosoft
// vstmidiproxy.cpp : Defines the entry point for the application.
//
#pragma warning(disable : 4996)

#include "stdafx.h"
#include "vstmidiproxy.h"
#include "../version.h"

#ifdef _DEBUG
#define _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>
#endif

HMODULE TeVirtualMIDI32 = NULL; // TeVirtualMIDI handle	

#define WIN32DEF(f) (WINAPI *f)
#define LOADVMIDIFUNCTION(f) *((void**)&f)=GetProcAddress(TeVirtualMIDI32,#f)
#define TEVM_RX 1
#define TEVM_RX_ONLY 4
#define WM_ICONMSG WM_APP + 2

typedef struct _VM_MIDI_PORT VM_MIDI_PORT, * LPVM_MIDI_PORT;
typedef void (CALLBACK* LPVM_MIDI_DATA_CB)(LPVM_MIDI_PORT midiPort, LPBYTE midiDataBytes, DWORD length, DWORD_PTR dwCallbackInstance);

static LPCWSTR WIN32DEF(virtualMIDIGetVersion)(PWORD major, PWORD minor, PWORD release, PWORD build) = NULL;
static LPVM_MIDI_PORT WIN32DEF(virtualMIDICreatePortEx2)(LPCWSTR portName, LPVM_MIDI_DATA_CB callback, DWORD_PTR dwCallbackInstance, DWORD maxSysexLength, DWORD flags) = NULL;
static BOOL WIN32DEF(virtualMIDIGetProcesses)(LPVM_MIDI_PORT midiPort, ULONG64* processIds, PDWORD length) = NULL;
static void WIN32DEF(virtualMIDIClosePort)(LPVM_MIDI_PORT midiPort) = NULL;

HINSTANCE hInst;
HWND Mainhwnd;
BOOL IsVMidiPresent = FALSE;
LPVM_MIDI_PORT TeVMPortA = NULL;
LPVM_MIDI_PORT TeVMPortB = NULL;
HMIDIOUT OutPortA = NULL;
HMIDIOUT OutPortB = NULL;

HANDLE proxyMutex = NULL;

static void CALLBACK teVMCallback(LPVM_MIDI_PORT midiPort, LPBYTE midiDataBytes, DWORD length, DWORD_PTR dwCallbackInstance)
{
	HMIDIOUT outPort = NULL;
	if (midiPort == TeVMPortA) outPort = OutPortA;
	else if (midiPort == TeVMPortB) outPort = OutPortB;

	if (!outPort) return;

	if (length < 4 && length > 0)
	{
		midiOutShortMsg(outPort, (DWORD)(*(LPDWORD)midiDataBytes));
	}
	else if (length > 0)
	{
		MIDIHDR SysExHeader = { 0 };
		SysExHeader.dwBufferLength = length;
		SysExHeader.dwBytesRecorded = length;
		SysExHeader.lpData = (LPSTR)midiDataBytes;
		midiOutPrepareHeader(outPort, &SysExHeader, sizeof(SysExHeader));
		midiOutLongMsg(outPort, &SysExHeader, sizeof(SysExHeader));
		// while ((SysExHeader.dwFlags & MHDR_DONE) != MHDR_DONE) {}; //not needed for VST Midi driver since it does not use async buffers.
		midiOutUnprepareHeader(outPort, &SysExHeader, sizeof(SysExHeader));
	}
}

TCHAR* GetFileVersionString(TCHAR* result)
{	
	_tcscat(result, _T("version: ") _T(stringify(VERSION_MAJOR)) _T(".") _T(stringify(VERSION_MINOR)) _T(".") _T(stringify(VERSION_PATCH)));
	return result;
}

BOOL Initialize(HINSTANCE hInstance, int nCmdShow)
{
	LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM); //forward declaration for main dlgproc.  

	TCHAR szWindowClass[] = _T("VstMidiProxy");

	proxyMutex = CreateMutex(NULL, true, _T("vstmidiproxy32"));
	if (GetLastError() == ERROR_ALREADY_EXISTS)
	{
		HWND winHandle = FindWindow(szWindowClass, NULL);
		if (winHandle)
		{
			ShowWindow(winHandle, SW_SHOWNORMAL);
			SetForegroundWindow(winHandle);
		}

		CloseHandle(proxyMutex);
		ExitProcess(0);
		return FALSE;
	}

	INITCOMMONCONTROLSEX icc = { 0 };
	icc.dwSize = sizeof(icc);
	icc.dwICC = ICC_WIN95_CLASSES | ICC_LINK_CLASS;
	if (!InitCommonControlsEx(&icc)) InitCommonControls();

	UINT portCount = midiOutGetNumDevs();
	for (UINT i = 0; i < portCount; i++)
	{
		MIDIOUTCAPS2 caps = { 0 };
		midiOutGetDevCaps(i, (LPMIDIOUTCAPS)&caps, sizeof(caps));

		if (caps.ManufacturerGuid == VSTMidiDrvManufacturerGuid && caps.ProductGuid == VSTMidiDrvPortAGuid)
			midiOutOpen(&OutPortA, i, NULL, NULL, CALLBACK_NULL);
		else if (caps.ManufacturerGuid == VSTMidiDrvManufacturerGuid && caps.ProductGuid == VSTMidiDrvPortBGuid)
			midiOutOpen(&OutPortB, i, NULL, NULL, CALLBACK_NULL);

		if (OutPortA && OutPortB) break;
	}

	if (!OutPortA || !OutPortB)
	{
		MessageBox(Mainhwnd, _T("VST Midi driver 2.4+ is not properly installed.\r\nDefault driver ports cannot be found.\r\nTry to reinstall the driver."), _T("VST Midi Proxy Error"), MB_OK | MB_ICONERROR);
		return FALSE;
	}

	hInst = hInstance;
	WNDCLASSEXW wcex = { 0 };

	wcex.cbSize = sizeof(WNDCLASSEX);
	wcex.style = 0;
	wcex.lpfnWndProc = WndProc;
	wcex.cbClsExtra = 0;
	wcex.cbWndExtra = DLGWINDOWEXTRA;
	wcex.hInstance = hInstance;
	wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_VSTMIDIPROXY));
	wcex.hIconSm = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));
	wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
	wcex.hbrBackground = (HBRUSH)(CTLCOLOR_DLG);
	wcex.lpszMenuName = NULL;
	wcex.lpszClassName = szWindowClass;

	if (!RegisterClassEx(&wcex)) return FALSE;

	Mainhwnd = CreateDialog(hInstance, szWindowClass, NULL, NULL);

	if (!Mainhwnd)
	{
		return FALSE;
	}

	TeVirtualMIDI32 = LoadLibrary(_T("teVirtualMIDI32.dll"));
	if (TeVirtualMIDI32)
	{
		LOADVMIDIFUNCTION(virtualMIDIGetVersion);
		LOADVMIDIFUNCTION(virtualMIDICreatePortEx2);
		LOADVMIDIFUNCTION(virtualMIDIGetProcesses);
		LOADVMIDIFUNCTION(virtualMIDIClosePort);

		IsVMidiPresent = virtualMIDIGetVersion && virtualMIDICreatePortEx2 && virtualMIDIGetProcesses && virtualMIDIClosePort;
	}

	if (IsVMidiPresent)
	{
		TeVMPortA = virtualMIDICreatePortEx2(L"VST MIDI Synth Global (port A)", teVMCallback, NULL, 65535, TEVM_RX | TEVM_RX_ONLY);
		TeVMPortB = virtualMIDICreatePortEx2(L"VST MIDI Synth Global (port B)", teVMCallback, NULL, 65535, TEVM_RX | TEVM_RX_ONLY);
		IsVMidiPresent = IsVMidiPresent && TeVMPortA && TeVMPortB;
		if (IsVMidiPresent) nCmdShow = SW_HIDE;
	}
	else
	{
		MessageBox(Mainhwnd, _T("VST Midi Proxy requires LoopMidi to be installed.\r\nYou can find LoopMidi download links on the main dialog of VST Midi Proxy."), _T("VST Midi Proxy Error"), MB_OK | MB_ICONERROR);
	}

	ShowWindow(Mainhwnd, nCmdShow);
	UpdateWindow(Mainhwnd);

	return TRUE;
}

int APIENTRY _tWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPTSTR lpCmdLine, int nCmdShow)
{
#ifdef _DEBUG
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
	_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_WNDW);
	_CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_WNDW);
#endif

	if (!Initialize(hInstance, nCmdShow))
	{
		return FALSE;
	}


	MSG msg;
	while (GetMessage(&msg, NULL, 0, 0))
	{
		if (!IsDialogMessage(Mainhwnd, &msg))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
	}

	return (int)msg.wParam;
}

void appendText(HWND hEdit, LPCTSTR newText)
{
	LRESULT TextLen = SendMessage(hEdit, WM_GETTEXTLENGTH, 0, 0);
	SendMessage(hEdit, EM_SETSEL, (WPARAM)TextLen, (LPARAM)TextLen);
	SendMessage(hEdit, EM_REPLACESEL, FALSE, (LPARAM)newText);
}

BOOL isX86Process(HANDLE process)
{
	SYSTEM_INFO systemInfo = { 0 };
	GetNativeSystemInfo(&systemInfo);

	if (systemInfo.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL)
		return TRUE;

	BOOL bIsWow64 = FALSE;
	IsWow64Process(process, &bIsWow64);
	return bIsWow64;
}

LPTSTR getProcessName(LPTSTR buffer, DWORD processId)
{
	HANDLE pHandle = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processId);
	if (pHandle)
	{
		TCHAR exeBuff[MAX_PATH] = { 0 };
		TCHAR titleBuff[MAX_PATH] = { 0 };
		GetModuleFileNameEx(pHandle, 0, exeBuff, MAX_PATH);
		GetFileTitle(exeBuff, titleBuff, MAX_PATH);
		_tcscat(titleBuff, isX86Process(pHandle) ? _T(" (32-bit)") : _T(" (64-bit)"));
		_tcscat(buffer, titleBuff);

		CloseHandle(pHandle);
	}

	return buffer;
}

BOOL getAutoStart()
{
	long lResult;
	DWORD dwType = REG_SZ;
	HKEY hKey;
	ULONG size;

	lResult = RegOpenKeyEx(HKEY_CURRENT_USER, _T("Software\\Microsoft\\Windows\\CurrentVersion\\Run"), 0, KEY_READ, &hKey);
	if (lResult == NO_ERROR)
	{
		TCHAR szValueName[] = _T("VstMidiProxy");
		lResult = RegQueryValueEx(hKey, szValueName, NULL, &dwType, NULL, &size);
		RegCloseKey(hKey);

		return lResult == ERROR_SUCCESS;

	}
	return FALSE;
}

void setAutoStart(bool enabled)
{
	long lResult;
	HKEY hKey;

	lResult = RegOpenKeyEx(HKEY_CURRENT_USER, _T("Software\\Microsoft\\Windows\\CurrentVersion\\Run"), 0, KEY_READ | KEY_WRITE, &hKey);
	if (lResult == NO_ERROR)
	{
		RegDeleteValue(hKey, _T("VstMidiProxy"));
		if (enabled)
		{
			TCHAR pszValue[MAX_PATH] = { 0 };
			GetModuleFileName(NULL, pszValue, MAX_PATH);
			RegSetValueEx(hKey, _T("VstMidiProxy"), NULL, REG_SZ, (LPBYTE)pszValue, (DWORD)((_tcslen(pszValue) + 1) * sizeof(TCHAR)));
		}

		RegCloseKey(hKey);
	}

}

int refreshClientList(const HWND hWnd)
{
	int clientCount = 0;
	HWND clientsCtl = GetDlgItem(hWnd, IDC_CLIENTS);
	if (virtualMIDIGetProcesses)
	{
		ULONG64 processIds[16] = { 0 };
		DWORD processIdSize = sizeof(processIds);

		SetWindowText(clientsCtl, _T(""));

		virtualMIDIGetProcesses(TeVMPortA, (ULONG64*)&processIds, &processIdSize);
		processIdSize /= sizeof(ULONG64);

		for (DWORD i = 0; i < processIdSize; i++)
		{
			TCHAR buffer[MAX_PATH] = _T("Port A: ");
			_tcscat(getProcessName(buffer, (DWORD)processIds[i]), _T("\r\n"));
			appendText(clientsCtl, buffer);
			clientCount++;
		}

		processIdSize = sizeof(processIds);
		virtualMIDIGetProcesses(TeVMPortB, (ULONG64*)&processIds, &processIdSize);
		processIdSize /= sizeof(ULONG64);

		for (DWORD i = 0; i < processIdSize; i++)
		{
			TCHAR buffer[MAX_PATH] = _T("Port B: ");
			_tcscat(getProcessName(buffer, (DWORD)processIds[i]), _T("\r\n"));
			appendText(clientsCtl, buffer);
			clientCount++;
		}

	}

	return clientCount;
}

UINT GetWaveOutDeviceId() {

	HKEY hKey = NULL;
	DWORD dwType = REG_SZ;
	WAVEOUTCAPS caps;
	static TCHAR regValue[32] = { 0 };

	bool initDone = !!_tcslen(regValue);

	long result = initDone ? NO_ERROR : RegOpenKeyEx(HKEY_CURRENT_USER, _T("Software\\VSTi Driver\\Output Driver"), 0, KEY_READ | KEY_WRITE, &hKey);
	if (result == NO_ERROR)
	{
		if (!initDone)
		{
			DWORD dwSize = sizeof(regValue);
			RegQueryValueEx(hKey, _T("WinMM WaveOut"), NULL, &dwType, (LPBYTE)regValue, &dwSize);
		}

		for (int deviceId = -1; waveOutGetDevCaps(deviceId, &caps, sizeof(caps)) == MMSYSERR_NOERROR; ++deviceId)
		{
			if (!_tcscmp(regValue, caps.szPname))
			{

				if (hKey) RegCloseKey(hKey);
				return deviceId;
			}
		}

		if (hKey) RegCloseKey(hKey);
	}

	return WAVE_MAPPER;
}

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
			regValue = (TCHAR*)calloc(dwSize + sizeof(TCHAR), 1);
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

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	static NOTIFYICONDATA nIconData = { 0 };
	static int sliderMax = 65535;
	static DWORD waveDeviceId = WAVE_MAPPER;
	static bool useAsio = false;
	static DWORD timerCounter = 0;

	switch (message)
	{
	case WM_CREATE:
	{
		nIconData.cbSize = sizeof(NOTIFYICONDATA);
		nIconData.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
		nIconData.hWnd = hWnd;
		nIconData.uID = WM_ICONMSG;
		nIconData.uCallbackMessage = WM_ICONMSG;
		nIconData.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_VSTMIDIPROXY));
		_tcscpy(nIconData.szTip, _T("VST Midi Proxy"));

		Shell_NotifyIcon(NIM_ADD, &nIconData);

		useAsio = UseAsio();
		if (!useAsio) waveDeviceId = GetWaveOutDeviceId();

		return FALSE;
	}
	break;
	case WM_SHOWWINDOW:
	{
		if (wParam)
		{
			HWND versionCtl = GetDlgItem(hWnd, IDC_STATIC_VERSION);
			HWND drvVersionCtl = GetDlgItem(hWnd, IDC_STATIC_DRVVERSION);
			HWND autostartCtl = GetDlgItem(hWnd, IDC_AUTOSTART);
			HWND valueSlider = GetDlgItem(hWnd, IDC_SLIDER_VOL);

			SendMessage(valueSlider, TBM_SETRANGEMIN, FALSE, 0);
			SendMessage(valueSlider, TBM_SETRANGEMAX, FALSE, sliderMax);
			SendMessage(valueSlider, TBM_SETTICFREQ, sliderMax / 10, 0);
			SendMessage(valueSlider, TBM_SETLINESIZE, 0, sliderMax / 100);
			SendMessage(valueSlider, TBM_SETPAGESIZE, 0, sliderMax / 10);

			SendMessage(autostartCtl, BM_SETCHECK, getAutoStart() ? BST_CHECKED : BST_UNCHECKED, 0);

			TCHAR fileversionBuff[64] = _T("VST Midi Proxy ");
			SetWindowText(versionCtl, GetFileVersionString(fileversionBuff));

			_tcscpy(fileversionBuff, _T("TeVirtualMIDI version: "));
			if (virtualMIDIGetVersion) _tcscat(fileversionBuff, virtualMIDIGetVersion(NULL, NULL, NULL, NULL));
			SetWindowText(drvVersionCtl, fileversionBuff);

			timerCounter = 0;
			SendMessage(hWnd, WM_TIMER, 1, 0);
			SetTimer(hWnd, 1, 50, NULL);

			return TRUE;
		}
	}
	break;
	case WM_TIMER:
	{
		if (wParam == 1)
		{
			if (!(timerCounter & 3)) refreshClientList(hWnd); //it's enough to refresh client list at every 4th cycle

			timerCounter++;

			if (!useAsio) waveDeviceId = GetWaveOutDeviceId();

			DWORD volume;
			HWND valueSlider = GetDlgItem(hWnd, IDC_SLIDER_VOL);
			waveOutGetVolume((HWAVEOUT)waveDeviceId, &volume);
			volume &= 0xFFFF;
			SendMessage(valueSlider, TBM_SETPOS, TRUE, (LPARAM)sliderMax - volume);
		}
	}
	break;
	case WM_VSCROLL:
	{
		HWND valueSlider = GetDlgItem(hWnd, IDC_SLIDER_VOL);
		if (lParam == (LPARAM)valueSlider)
		{
			int selectedValue = sliderMax - (int)SendMessage(valueSlider, TBM_GETPOS, 0, 0);
			waveOutSetVolume((HWAVEOUT)waveDeviceId, selectedValue | (selectedValue << 16));

		}
	}
	break;
	case WM_NOTIFY:
	{
		switch (((LPNMHDR)lParam)->code)
		{
		case NM_CLICK:
		case NM_RETURN:
		{
			PNMLINK pNMLink = (PNMLINK)lParam;
			LITEM   item = pNMLink->item;

			ShellExecute(NULL, L"open", item.szUrl, NULL, NULL, SW_SHOW);
		}
		}
	}
	break;
	case WM_ICONMSG:
	{
		if (wParam == WM_ICONMSG && (lParam == WM_RBUTTONDOWN || lParam == WM_LBUTTONDOWN))
		{
			ShowWindow(hWnd, SW_SHOWNORMAL);
			return TRUE;
		}
	}
	break;
	case WM_COMMAND:
	{
		HWND autostartCtl = GetDlgItem(hWnd, IDC_AUTOSTART);
		if (HIWORD(wParam) == BN_CLICKED && lParam == (LPARAM)autostartCtl)
		{
			bool checked = SendMessage(autostartCtl, BM_GETCHECK, 0, 0) == BST_CHECKED;
			setAutoStart(checked);

		}
		else if (wParam == IDCANCEL || wParam == IDOK)
		{
			PostMessage(hWnd, WM_CLOSE, 0, 0);
			return TRUE;
		}
		else if (wParam == IDC_EXIT)
		{
			PostMessage(hWnd, WM_CLOSE, TRUE, TRUE);
			return TRUE;
		}

	}
	break;
	case WM_HELP:
	{
		TCHAR tmpPath[MAX_PATH] = { 0 };

		if (GetWindowsDirectory(tmpPath, MAX_PATH))
		{
			//hack to skip to proper anchor in html 

			_tcscat(tmpPath, _T("\\SysWOW64\\vstmididrv\\Help\\advredir.html"));

			if (GetFileAttributes(tmpPath) == INVALID_FILE_ATTRIBUTES)
			{
				if (GetWindowsDirectory(tmpPath, MAX_PATH))
				{
					_tcscat(tmpPath, _T("\\System32\\vstmididrv\\Help\\advredir.html"));
				}
			}

			ShellExecute(hWnd, NULL, tmpPath, NULL, NULL, SW_SHOWNORMAL);
		}
	}
	break;
	case WM_CLOSE:
	{
		if (wParam && lParam)
		{
			if (refreshClientList(hWnd))
			{
				if (MessageBox(hWnd, _T("Some clients are still connected.\r\nAre you sure you want to exit?"), _T("VST Midi Proxy"), MB_YESNO | MB_ICONQUESTION) == IDNO) return FALSE;
			}
			DestroyWindow(hWnd);
		}
		else
		{
			ShowWindow(hWnd, SW_HIDE);
		}

		KillTimer(hWnd, 1);

		return TRUE;
	}
	break;
	case WM_DESTROY:
	{
		Shell_NotifyIcon(NIM_DELETE, &nIconData);

		if (OutPortA) midiOutClose(OutPortA);
		if (OutPortB) midiOutClose(OutPortB);
		if (TeVMPortA) virtualMIDIClosePort(TeVMPortA);
		if (TeVMPortB) virtualMIDIClosePort(TeVMPortB);
		if (TeVirtualMIDI32) FreeLibrary(TeVirtualMIDI32);
		if (proxyMutex) CloseHandle(proxyMutex);

		PostQuitMessage(0);
		return TRUE;
	}
	}

	return DefDlgProc(hWnd, message, wParam, lParam);
}

