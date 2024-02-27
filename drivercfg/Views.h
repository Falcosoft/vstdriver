#if !defined(AFX_VIEWS_H__20020629_8D64_963C_A351_0080AD509054__INCLUDED_)
#define AFX_VIEWS_H__20020629_8D64_963C_A351_0080AD509054__INCLUDED_

#include "../external_packages/mmddk.h"
#include "../external_packages/audiodefs.h"
#include "../driver/VSTDriver.h"

/// Define BASSASIO functions as pointers
#define BASSASIODEF(f) (WINAPI *f)
#define LOADBASSASIOFUNCTION(f) *((void**)&f)=GetProcAddress(bassasio,#f)

#define MAX_PLUGINS 10

#if(_WIN32_WINNT < 0x0500) 
	#define     GA_PARENT       1
	#define     GA_ROOT         2
	#define     GA_ROOTOWNER    3
	
	// Actually this is supported by Win NT4 SP6 but built-in headers only contain these definitions if _WIN32_WINNT >= 0x0500	
	extern "C" {
		WINUSERAPI HWND WINAPI GetAncestor( __in HWND hwnd, __in UINT gaFlags);
	}
#endif

#ifdef WIN64
	TCHAR windowName[32] = _T("VSTi Driver Configuration (x64)");
#else	
	#include <intrin.h>
	TCHAR windowName[32] = _T("VSTi Driver Configuration");	
#endif	

#include "../external_packages/bassasio.h"

using namespace std;

typedef AEffect* (*PluginEntryProc) (audioMasterCallback audioMaster);
static INT_PTR CALLBACK EditorProc (HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// for VSTDriver
extern "C" HINSTANCE hinst_vst_driver = NULL;

static HINSTANCE bassasio = NULL;       // bassasio handle  

static BOOL isASIO = FALSE;
static bool usingASIO = false;  
static bool is4chMode = false;
static bool isWinNT4 =  false;
static DWORD portBOffsetVal = 2;
static DWORD usePrivateAsioOnly = 0;
static unsigned char SelectedPluginIndex = 0;


static bool IsWaveFormatSupported(UINT sampleRate, UINT deviceId)
{
	PCMWAVEFORMAT wFormatLegacy = { 0 };
	WAVEFORMATEXTENSIBLE wFormat = { 0 };
	WORD channels = 2;	 

	if(isWinNT4)
	{
		wFormatLegacy.wf.wFormatTag = WAVE_FORMAT_PCM;
		wFormatLegacy.wf.nChannels = channels;
		wFormatLegacy.wf.nSamplesPerSec = sampleRate;
		wFormatLegacy.wBitsPerSample = 16;
		wFormatLegacy.wf.nBlockAlign = wFormatLegacy.wf.nChannels * wFormatLegacy.wBitsPerSample / 8;
		wFormatLegacy.wf.nAvgBytesPerSec = wFormatLegacy.wf.nBlockAlign * wFormatLegacy.wf.nSamplesPerSec;
	}
	else
	{	
		//if a given sample rate is supported with 32-bit float then it is sure to be also supported with 16-bit int 
		wFormat.Format.cbSize = sizeof(wFormat) - sizeof(wFormat.Format);
		wFormat.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
		wFormat.Format.nChannels = channels;
		wFormat.Format.nSamplesPerSec = sampleRate;
		wFormat.Format.wBitsPerSample = 32;
		wFormat.Format.nBlockAlign = wFormat.Format.nChannels * wFormat.Format.wBitsPerSample / 8;
		wFormat.Format.nAvgBytesPerSec = wFormat.Format.nBlockAlign * wFormat.Format.nSamplesPerSec;
		wFormat.dwChannelMask = SPEAKER_STEREO;
		wFormat.Samples.wValidBitsPerSample = wFormat.Format.wBitsPerSample;
		wFormat.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
	}
	
	if (waveOutOpen(NULL, deviceId, isWinNT4 ? (LPWAVEFORMATEX)&wFormatLegacy : &wFormat.Format, NULL, NULL, WAVE_FORMAT_QUERY) == MMSYSERR_NOERROR) return true;
	return false;
}

static BOOL IsASIO() 
{
	TCHAR installpath[MAX_PATH];        
	TCHAR bassasiopath[MAX_PATH];	

	GetModuleFileName(hinst_vst_driver, installpath, MAX_PATH);
	//PathRemoveFileSpec(installpath);
	TCHAR *chrP = _tcsrchr(installpath, '\\'); //removes SHLWAPI dependency for WIN NT4
	if(chrP) chrP[0] = 0;


	// Load Bass Asio
	_tcscpy_s(bassasiopath, installpath);
	_tcscat_s(bassasiopath, _T("\\bassasio_vstdrv.dll"));
	bassasio = LoadLibrary(bassasiopath);        

	if (bassasio)
	{

//BassAsio requires SSE support. SSE is guaranteed on x64 but not on x86.
#ifndef WIN64

		int cpuInfo[4] = { 0 };
		__cpuid(cpuInfo, 1);
		
		if (!(cpuInfo[3] & 0x2000000)) //Test SSE bit 25 in register EDX
		{			
			MessageBox(FindWindow(NULL, windowName),
			_T("BassAsio requires a CPU with SSE support!\r\nSince SSE is not detected ASIO output is not available.\r\n\r\nYou should reinstall this driver without BassAsio\r\nor delete bassasio_vstdrv.dll to stop this warning."),
			_T("VST MIDI Driver"), MB_OK | MB_ICONWARNING | MB_SYSTEMMODAL);
			
			return FALSE;
		}
#endif	

		LOADBASSASIOFUNCTION(BASS_ASIO_ErrorGetCode);
		LOADBASSASIOFUNCTION(BASS_ASIO_SetWindow);
		LOADBASSASIOFUNCTION(BASS_ASIO_Init);
		LOADBASSASIOFUNCTION(BASS_ASIO_Free);
		LOADBASSASIOFUNCTION(BASS_ASIO_GetInfo);
		LOADBASSASIOFUNCTION(BASS_ASIO_GetDeviceInfo);
		LOADBASSASIOFUNCTION(BASS_ASIO_ChannelGetInfo);
		LOADBASSASIOFUNCTION(BASS_ASIO_ControlPanel);
		LOADBASSASIOFUNCTION(BASS_ASIO_CheckRate);
				
		TCHAR asio2WasapiPath[MAX_PATH];
		_tcscpy_s(asio2WasapiPath, installpath);
		_tcscat_s(asio2WasapiPath, _T("\\ASIO2WASAPI_vstdrv.dll"));
		if (IsVistaOrNewer() && GetFileAttributes(asio2WasapiPath) != INVALID_FILE_ATTRIBUTES)
		{
			LOADBASSASIOFUNCTION(BASS_ASIO_AddDevice);

			char asio2WasapiAnsiPath[MAX_PATH] = { 0 };
#ifdef UNICODE  
			WideCharToMultiByte(CP_ACP, 0, (LPCWSTR)asio2WasapiPath, -1, (char*)asio2WasapiAnsiPath, MAX_PATH, NULL, NULL);
#else
			strcpy_s(asio2WasapiAnsiPath, asio2WasapiPath);
#endif	
			static const GUID CLSID_ASIO2WASAPI_DRIVER = { 0x3981c4c8, 0xfe12, 0x4b0f, { 0x98, 0xa0, 0xd1, 0xb6, 0x67, 0xbd, 0xa6, 0x15 } };
			BASS_ASIO_AddDevice(&CLSID_ASIO2WASAPI_DRIVER, asio2WasapiAnsiPath, "VSTDriver-ASIO2WASAPI");
		
		}

		BASS_ASIO_DEVICEINFO info;
		return BASS_ASIO_GetDeviceInfo(0, &info);		
	}	

	return FALSE;	
}

static BOOL settings_load(VSTDriver * effect)
{
	BOOL retResult = FALSE;
	long lResult;	
	ULONG size;
	CRegKeyEx reg;

	lResult = reg.Create(HKEY_CURRENT_USER, _T("Software\\VSTi Driver"), 0, 0, KEY_READ | KEY_WRITE);
	if (lResult == ERROR_SUCCESS){
		TCHAR szValueName[12] = _T("plugin");
		if (SelectedPluginIndex)		
		{
			TCHAR szPostfix[4] = { 0 };
			_tcscat_s(szValueName, _itot(SelectedPluginIndex, szPostfix, 10));
		}
		lResult = reg.QueryStringValue(szValueName, NULL, &size);
		if (lResult == ERROR_SUCCESS) {
			TCHAR vst_path[MAX_PATH] = { 0 };
			reg.QueryStringValue(szValueName, vst_path, &size);
			TCHAR *chrP = _tcsrchr(vst_path, '.'); // removes extension
			if(chrP) chrP[0] = 0;
			_tcscat_s(vst_path, _T(".set"));
			HANDLE fileHandle = CreateFile(vst_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);	
			if (fileHandle != INVALID_HANDLE_VALUE)
			{
				size_t chunk_size = GetFileSize(fileHandle, NULL);
				if (chunk_size != INVALID_FILE_SIZE)
				{
					vector<uint8_t> chunk;
					chunk.resize( chunk_size );
#if (defined(_MSC_VER) && (_MSC_VER < 1600))
					if (chunk_size) {
						retResult = ReadFile(fileHandle, (char*) &chunk.front(), (DWORD)chunk_size, &size, NULL);
						if (effect) effect->setChunk( &chunk.front(), (unsigned int)chunk_size);
					}
#else
					if (chunk_size) {
						retResult = ReadFile(fileHandle, (char*) chunk.data(), (DWORD)chunk_size, &size, NULL);					
						if (effect) effect->setChunk( chunk.data(), (unsigned int)chunk_size );
					}

#endif
				}				
				CloseHandle(fileHandle);
			}

		}
		reg.Close();
	}

	return retResult;
}

static BOOL settings_save(VSTDriver * effect)
{
	BOOL retResult = FALSE;
	long lResult;	
	ULONG size;
	CRegKeyEx reg;

	lResult = reg.Create(HKEY_CURRENT_USER, _T("Software\\VSTi Driver"), 0, 0, KEY_READ | KEY_WRITE); // falco fix: otherwise reg.QueryStringValue gets back an ACCESS_DENIED(5) error.
	if (lResult == ERROR_SUCCESS) {		
		
		TCHAR szValueName[12] = _T("plugin");
		if (SelectedPluginIndex)
		{
			TCHAR szPostfix[4] = { 0 };
			_tcscat_s(szValueName, _itot(SelectedPluginIndex, szPostfix, 10));
		}		
		lResult = reg.QueryStringValue(szValueName, NULL, &size);
		
		if (lResult == ERROR_SUCCESS) {
			TCHAR vst_path[MAX_PATH] = { 0 };
			reg.QueryStringValue(szValueName, vst_path, &size);
			TCHAR* chrP = _tcsrchr(vst_path, '.'); // removes extension
			if(chrP) chrP[0] = 0;
			_tcscat_s(vst_path, _T(".set"));
			HANDLE fileHandle = CreateFile(vst_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);			
			if (fileHandle != INVALID_HANDLE_VALUE)
			{
				vector<uint8_t> chunk;
				if (effect) effect->getChunk( chunk );
#if (defined(_MSC_VER) && (_MSC_VER < 1600))

				if (chunk.size() >= (2 * sizeof(uint32_t) + sizeof(bool))) retResult = WriteFile(fileHandle, &chunk.front(), (DWORD)chunk.size(), &size, NULL); 
#else
				if (chunk.size() >= (2 * sizeof(uint32_t) + sizeof(bool))) retResult = WriteFile(fileHandle, chunk.data(), (DWORD)chunk.size(), &size, NULL);

#endif
				CloseHandle(fileHandle);
			}

		}
		reg.Close();
	}

	return retResult;
}

static CString LoadOutputDriver(CString valueName)
{
	CRegKeyEx reg;
	CString value;

	long result = reg.Open(HKEY_CURRENT_USER, _T("Software\\VSTi Driver\\Output Driver"), KEY_READ);
	if (result != NO_ERROR)
	{
		return value;
	}

	ULONG size = 0;

	result = reg.QueryStringValue(valueName, NULL, &size);
	if (result == NO_ERROR && size > 0)
	{
		reg.QueryStringValue(valueName, value.GetBuffer(size), &size);
		value.ReleaseBuffer();
	}

	reg.Close();

	return value;
}

static void SaveDwordValue(LPCTSTR key, DWORD value)
{	   
	CRegKeyEx reg;
	long lResult;
	lResult = reg.Create(HKEY_CURRENT_USER, _T("Software\\VSTi Driver"), 0, 0, KEY_WRITE);
	if (lResult == ERROR_SUCCESS)
	{
		reg.SetDWORDValue(key, value);
		reg.Close();
	}		   
}


#if _MSC_VER > 1000
#pragma once
#endif // _MSC_VER > 1000

class CView1 : public CDialogImpl<CView1>
{

	CComboBox vst_info;
	CComboBox vst_buffer_size, vst_sample_rate, vst_sample_format;
	CButton vst_load, vst_configure, vst_showvst, vst_4chmode, vst_unload;
	CStatic vst_vendor, vst_effect, file_info;
	CTrackBarCtrl volume_slider;
	TCHAR vst_path[MAX_PATH];
	HANDLE hbrBkgnd;
	DWORD highDpiMode;
	DWORD enableSinglePort32ChMode;
	DWORD keepDriverLoaded;
	int origDropdownWidth;
	
	VSTDriver * effect;
public:
	CView1() :		
		vst_info(), vst_buffer_size(), vst_sample_rate(), vst_sample_format(),
		vst_load(), vst_configure(), vst_showvst(), vst_4chmode(), vst_unload(),
		vst_vendor(), vst_effect(), file_info(),
		volume_slider(),
		vst_path(),
		hbrBkgnd(),
		highDpiMode(),
		origDropdownWidth(),
		effect()
	{		
		
		enableSinglePort32ChMode = (DWORD)-1;
		keepDriverLoaded = (DWORD)-1;

		usePrivateAsioOnly = 0;		
		isWinNT4 = IsWinNT4();
		isASIO = IsASIO();
		usingASIO = isASIO && LoadOutputDriver(_T("Driver Mode")).CompareNoCase(_T("Bass ASIO")) == 0;
	}

	~CView1()
	{
		if (effect) free_vst();
		if (bassasio)
		{
			FreeLibrary(bassasio);
			bassasio = NULL;
		}

		if (hbrBkgnd != NULL) DeleteObject(hbrBkgnd);
	}

	enum { IDD = IDD_MAIN };

	BEGIN_MSG_MAP(CView1)
		MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialogView1)
		COMMAND_ID_HANDLER(IDC_VSTLOAD,OnButtonAdd)
		COMMAND_ID_HANDLER(IDC_VSTCONFIG,OnButtonConfig)
		COMMAND_ID_HANDLER(IDC_UNLOAD, OnButtonUnload)
		COMMAND_HANDLER(IDC_SHOWVST, BN_CLICKED, OnClickedSHOWVST)		
		COMMAND_HANDLER(IDC_VSTLOADED, CBN_SELCHANGE, OnCbnSelchangeVSTLoaded)
		COMMAND_HANDLER(IDC_VSTLOADED, CBN_DROPDOWN, OnCbnDropDownVSTLoaded)
		COMMAND_HANDLER(IDC_SAMPLERATE, CBN_SELCHANGE, OnCbnSelchangeSamplerate)
		COMMAND_HANDLER(IDC_BUFFERSIZE, CBN_SELCHANGE, OnCbnSelchangeBuffersize)
		COMMAND_HANDLER(IDC_SAMPLEFORMAT, CBN_SELCHANGE, OnCbnSelchangeSampleformat)
		MESSAGE_HANDLER(WM_HSCROLL, OnHScroll)
		MESSAGE_HANDLER(WM_CTLCOLORSTATIC, OnCtlColorStatic)
		COMMAND_HANDLER(IDC_USE4CH, BN_CLICKED, OnBnClickedUse4ch)
	END_MSG_MAP()	

	void load_settings()
	{
		long lResult;
		vst_path[0] = 0;
		ULONG size;		
		CRegKeyEx reg;
		lResult = reg.Create(HKEY_CURRENT_USER, _T("Software\\VSTi Driver"));
		if (lResult == ERROR_SUCCESS){

			for (int i = 0; i < MAX_PLUGINS; i++) {
				TCHAR szValueName[8] = _T("plugin");
				if (i)
				{
					TCHAR szPostfix[2] = { 0 };
					_tcscat_s(szValueName, _itot(i, szPostfix, 10));
				}
				lResult = reg.QueryStringValue(szValueName, NULL, &size);
				if (lResult == ERROR_SUCCESS && size > 2) {
					TCHAR pluginPath[MAX_PATH] = { 0 };
					reg.QueryStringValue(szValueName, pluginPath, &size);
					vst_info.AddString(pluginPath);
				}
				else
					vst_info.AddString(_T("No VSTi loaded"));
			}

			DWORD reg_value;
			TCHAR tmpBuff[34];

			lResult = reg.QueryDWORDValue(_T("SelectedPlugin"), reg_value);
			if (lResult == ERROR_SUCCESS) {
				SelectedPluginIndex = (unsigned char)reg_value;				
			}
			lResult = reg.QueryDWORDValue(_T("ShowVstDialog"),reg_value);
			if (lResult == ERROR_SUCCESS) {
				vst_showvst.SetCheck(reg_value);
			}
			lResult = reg.QueryDWORDValue(_T("Use4ChannelMode"),reg_value);
			if (lResult == ERROR_SUCCESS) {
				if (!isWinNT4 || usingASIO) {
					vst_4chmode.SetCheck(reg_value);
					if (reg_value) is4chMode = true;
				}
			}		   
			lResult = reg.QueryDWORDValue(_T("UseFloat"),reg_value);
			if (lResult == ERROR_SUCCESS) {			   
				if ((!isWinNT4 || usingASIO) && reg_value) vst_sample_format.SelectString(-1, _T("32-bit Float"));
				else vst_sample_format.SelectString(-1, _T("16-bit Int"));
			}
			lResult = reg.QueryDWORDValue(_T("SampleRate"),reg_value);
			if (lResult == ERROR_SUCCESS) {			   
				vst_sample_rate.SelectString(-1, _ultot(reg_value, tmpBuff, 10));			   
			}
			lResult = reg.QueryDWORDValue(_T("BufferSize"),reg_value);
			if (lResult == ERROR_SUCCESS) {
				vst_buffer_size.SelectString(-1, _ultot(reg_value, tmpBuff, 10));
			}
			lResult = reg.QueryDWORDValue(_T("Gain"),reg_value);
			if (lResult == ERROR_SUCCESS) {
				volume_slider.SetPos((int)reg_value);
			}
			lResult = reg.QueryDWORDValue(_T("PortBOffset"),reg_value);
			if (lResult == ERROR_SUCCESS) {
				portBOffsetVal = reg_value;
			}
			lResult = reg.QueryDWORDValue(_T("HighDpiMode"), reg_value);
			if (lResult == ERROR_SUCCESS) {
				highDpiMode = reg_value;
			}
			lResult = reg.QueryDWORDValue(_T("EnableSinglePort32ChMode"), reg_value);
			if (lResult == ERROR_SUCCESS) {
				enableSinglePort32ChMode = reg_value;
			}
			lResult = reg.QueryDWORDValue(_T("KeepDriverLoaded"), reg_value);
			if (lResult == ERROR_SUCCESS) {
				keepDriverLoaded = reg_value;
			}
			lResult = reg.QueryDWORDValue(_T("UsePrivateAsioOnly"), reg_value);
			if (lResult == ERROR_SUCCESS) {
				usePrivateAsioOnly = reg_value;
			}	



			reg.Close();
			
			vst_info.SetCurSel(SelectedPluginIndex);
			vst_info.GetLBText(SelectedPluginIndex, vst_path);				
			load_vst(vst_path, false);
			if (effect)
			{
				vst_configure.EnableWindow(effect->hasEditor());
				vst_unload.EnableWindow(true);
			}
			else
				vst_unload.EnableWindow(false);

		}

		vst_info.SetCurSel(SelectedPluginIndex);

	}

	LRESULT OnCtlColorStatic(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled)
	{

		if (lParam == (LPARAM)GetDlgItem(IDC_VENDOR).m_hWnd || lParam == (LPARAM)GetDlgItem(IDC_EFFECT).m_hWnd)
		{
			HDC hdcStatic = (HDC)wParam;
			SetTextColor(hdcStatic, RGB(0, 0, 0));
			SetBkColor(hdcStatic, RGB(220, 220, 220));

			if (hbrBkgnd == NULL)
			{
				hbrBkgnd = CreateSolidBrush(RGB(220, 220, 220));
			}
			return (INT_PTR)hbrBkgnd;
		}
		return 0;
	}

	

	LRESULT OnButtonUnload(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/) 
	{	
		if (effect) free_vst();
		vst_effect.SetWindowText(_T("No VSTi loaded"));
		vst_vendor.SetWindowText(_T("No VSTi loaded"));
		int selIndex = vst_info.GetCurSel();
		vst_info.DeleteString(selIndex);
		vst_info.InsertString(selIndex, _T("No VSTi loaded"));
		vst_info.SetCurSel(selIndex);

		// HKEY hKey, hSubKey;
		long lResult;
		CRegKeyEx reg;
		lResult = reg.Create(HKEY_CURRENT_USER, _T("Software\\VSTi Driver"), 0, 0, KEY_WRITE);
		if (lResult == ERROR_SUCCESS)
		{
			TCHAR szValueName[12] = _T("plugin");
			if (SelectedPluginIndex)
			{
				TCHAR szPostfix[4] = { 0 };
				_tcscat_s(szValueName, _itot(SelectedPluginIndex, szPostfix, 10));
			}
			reg.SetStringValue(szValueName, _T(""));
			reg.Close();
		}
				
		vst_configure.EnableWindow(false);
		vst_unload.EnableWindow(false);

		return 0;
	}

	LRESULT OnButtonAdd(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/ )
	{
		TCHAR szFileName[MAX_PATH] = {0};
		LPCTSTR sFiles = 
			_T("VSTi instruments (*.dll)\0*.dll\0")
			_T("All Files (*.*)\0*.*\0\0");
				 
		vst_info.GetLBText(SelectedPluginIndex, szFileName);
		if (_tcscmp(_T("No VSTi loaded"), szFileName)) {
			vst_path[0] = 0;
			vst_info.GetLBText(SelectedPluginIndex, vst_path);
		}
		CFileDialog dlg( TRUE, NULL, vst_path, OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT, sFiles);
		if (dlg.DoModal() == IDOK)
		{
			szFileName[0] = 0;
			_tcscpy_s(szFileName,dlg.m_szFileName);
			if (load_vst(szFileName))
			{				
				vst_info.DeleteString(SelectedPluginIndex);
				vst_info.InsertString(SelectedPluginIndex, szFileName);
				vst_info.SetCurSel(SelectedPluginIndex);

				// HKEY hKey, hSubKey;
				long lResult;
				CRegKeyEx reg;
				lResult = reg.Create(HKEY_CURRENT_USER, _T("Software\\VSTi Driver"), 0, 0, KEY_WRITE);
				if (lResult == ERROR_SUCCESS)
				{
					TCHAR szValueName[12] = _T("plugin");
					if (SelectedPluginIndex)
					{
						TCHAR szPostfix[4] = { 0 };
						_tcscat_s(szValueName, _itot(SelectedPluginIndex, szPostfix, 10));
					}
					reg.SetStringValue(szValueName, szFileName);
					reg.Close();
				}				
							
				vst_configure.EnableWindow(effect->hasEditor());
				vst_unload.EnableWindow(true);

				BOOL dummy;
				OnCbnSelchangeVSTLoaded(0, 0, 0, dummy);
			}
			else
			{
				// HKEY hKey, hSubKey;
				long lResult;
				CRegKeyEx reg;
				lResult = reg.Create(HKEY_CURRENT_USER, _T("Software\\VSTi Driver"), 0, 0, KEY_WRITE);

				if (lResult == ERROR_SUCCESS)
				{
					TCHAR szValueName[12] = _T("plugin");
					if (SelectedPluginIndex)
					{
						TCHAR szPostfix[4] = { 0 };
						_tcscat_s(szValueName, _itot(SelectedPluginIndex, szPostfix, 10));
					}
					reg.SetStringValue(szValueName, _T(""));
					reg.Close();
				}

				vst_configure.EnableWindow(false);
				vst_unload.EnableWindow(false);
			}
		}
		return 0;
	}

	LRESULT OnClickedSHOWVST(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
	{  
		SaveDwordValue(_T("ShowVstDialog"), vst_showvst.GetCheck());

		return 0;
	}

	LRESULT OnBnClickedUse4ch(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
	{   
		SaveDwordValue(_T("Use4ChannelMode"), vst_4chmode.GetCheck());
		is4chMode = vst_4chmode.GetCheck() !=  BST_UNCHECKED;

		return 0;
	}

	//calculate proper width of dropdown in order full paths to be visible.
	LRESULT OnCbnDropDownVSTLoaded(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
	{	
		const int PADDING = 8;
		HDC dc = vst_info.GetDC();
		HFONT font = (HFONT)vst_info.SendMessage(WM_GETFONT, 0, 0);
		HFONT oldFont  = (HFONT)SelectObject(dc, font);
		
		TCHAR path[MAX_PATH] = { 0 };
		SIZE size;		
		int itemsWidth = 0;		

		for (int i = 0; i < MAX_PLUGINS; i++)
		{
			int len = vst_info.GetLBText(i, path);
			GetTextExtentPoint32(dc, path, len, &size);
			if (size.cx > itemsWidth) itemsWidth = size.cx;

		}		

		itemsWidth += PADDING;
		int maxWidth = GetSystemMetrics(SM_CXSCREEN) / 2;
		vst_info.SetDroppedWidth(min(maxWidth, max(origDropdownWidth, itemsWidth)));

		SelectObject(dc, oldFont);
		vst_info.ReleaseDC(dc);

		return 0;
	}

	LRESULT OnCbnSelchangeVSTLoaded(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
	{		
		TCHAR pluginPath[MAX_PATH] = { 0 };
		int selIndex = vst_info.GetCurSel();
		vst_info.GetLBText(selIndex, pluginPath);
		if (load_vst(pluginPath, false))
		{
			// HKEY hKey, hSubKey;
			long lResult;
			CRegKeyEx reg;
			lResult = reg.Create(HKEY_CURRENT_USER, _T("Software\\VSTi Driver"), 0, 0, KEY_WRITE);
			if (lResult == ERROR_SUCCESS)
			{
				reg.SetDWORDValue(_T("SelectedPlugin"), selIndex);
				reg.Close();
			}
			vst_configure.EnableWindow(effect->hasEditor());
			vst_unload.EnableWindow(true);
		}
		else
		{
			vst_configure.EnableWindow(false);
			vst_unload.EnableWindow(false);
		}

		SelectedPluginIndex = selIndex;
		return 0;
	}

	LRESULT OnCbnSelchangeSamplerate(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
	{	   
		TCHAR tmpBuff[8];
		vst_sample_rate.GetWindowText(tmpBuff, 8);
		SaveDwordValue(_T("SampleRate"), _ttoi(tmpBuff));

		return 0;
	} 

	LRESULT OnCbnSelchangeSampleformat(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
	{
		DWORD value;
		TCHAR tmpBuff[14];
		vst_sample_format.GetWindowText(tmpBuff, 14);	   
		if(!_tcscmp(tmpBuff, _T("32-bit Float"))) value = 1;
		else value  = 0;	  
		SaveDwordValue(_T("UseFloat"), value);

		return 0;
	} 

	LRESULT OnCbnSelchangeBuffersize(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
	{	  
		TCHAR tmpBuff[8];
		vst_buffer_size.GetWindowText(tmpBuff, 8);
		SaveDwordValue(_T("BufferSize"), _ttoi(tmpBuff));

		return 0;
	} 

	LRESULT OnHScroll(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled) 
	{	   
		if ((HWND)lParam == volume_slider.m_hWnd && LOWORD(wParam) == SB_ENDSCROLL)
		{		          
			SaveDwordValue(_T("Gain"), (DWORD)volume_slider.GetPos());
		}
		return 0;
	}

	LRESULT OnButtonConfig(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/ )
	{
		if(effect && effect->hasEditor())
		{
			HWND hWnd = GetAncestor(this->m_hWnd, GA_ROOT);
			::EnableWindow(hWnd, FALSE);
			effect->setHighDpiMode(highDpiMode);

			TCHAR tmpBuff[8];
			vst_sample_rate.GetWindowText(tmpBuff, 8);	      
			effect->setSampleRate(_ttoi(tmpBuff));
			
			effect->displayEditorModal();
			::EnableWindow(hWnd, TRUE);
			::SetForegroundWindow(this->m_hWnd); //Gets back focus after editor is closed. AllowSetForegroundWindow has to be called by vsthost.			
			
			effect->ProcessMIDIMessage(0, 0x90); //force some plugins to enable save/load functions.
			float sample[2] = {0};
			effect->RenderFloat(&sample[0], 1);
		}
		return 0;
	}

	void free_vst()
	{
		if(effect)
		{
			if (!settings_save(effect))
			{				
				string vstStr;
				effect->getProductString(vstStr);
				char msg[MAX_PATH] = "Cannot save plugin settings for ";
				strncat_s(msg, vstStr.c_str(), MAX_PATH - 2 - strlen(msg));
				strcat_s(msg, "!");
				::MessageBoxA(m_hWnd, msg, "VST MIDI Driver", MB_OK | MB_ICONERROR);
			}
			if(!highDpiMode) SaveDwordValue(_T("HighDpiMode"),(DWORD)-5); //set DPI_AWARENESS_CONTEXT_UNAWARE_GDISCALED as default for VST editors;
			if(enableSinglePort32ChMode == (DWORD)-1) SaveDwordValue(_T("EnableSinglePort32ChMode"), 1);
			if(keepDriverLoaded == (DWORD)-1) SaveDwordValue(_T("KeepDriverLoaded"), 1);
			if(IsVistaOrNewer()) SaveDwordValue(_T("UsePrivateAsioOnly"), usePrivateAsioOnly);
			
			delete effect;
			effect = NULL;
		}
	}

	BOOL load_vst(TCHAR * szPluginPath, bool checkValidity = true)
	{
		if(effect) free_vst(); // falco fix: otherwise an empty save occures at every start.
		effect = new VSTDriver;
		if (!effect->OpenVSTDriver(szPluginPath))
		{
			delete effect;
			effect = NULL;
			if (checkValidity && szPluginPath && *szPluginPath)
				MessageBox(_T("This is NOT a VSTi synth!"), _T("VST MIDI Driver"), MB_OK | MB_ICONERROR);
			vst_effect.SetWindowText(_T("No VSTi loaded"));
			vst_vendor.SetWindowText(_T("No VSTi loaded"));
			int selIndex = vst_info.GetCurSel();
			vst_info.DeleteString(selIndex);
			vst_info.InsertString(selIndex, _T("No VSTi loaded"));
			vst_info.SetCurSel(selIndex);
			return FALSE;
		}

		string vstStr;
		effect->getEffectName(vstStr);	   
		::SetWindowTextA(vst_effect.m_hWnd, vstStr.c_str());
		effect->getVendorString(vstStr);	  
		::SetWindowTextA(vst_vendor.m_hWnd, vstStr.c_str());

		effect->ProcessMIDIMessage(0, 0x90); //force some plugins to enable save/load functions.
		float sample[2] = {0};
		effect->RenderFloat(&sample[0], 1);

		settings_load(effect);	   

		return TRUE;
	}

#pragma comment(lib,"Version.lib") 
	TCHAR* GetFileVersion(TCHAR* result, unsigned int buffSize)
	{
		DWORD               dwSize = 0;
		BYTE* pVersionInfo = NULL;
		VS_FIXEDFILEINFO* pFileInfo = NULL;
		UINT                pLenFileInfo = 0;
		TCHAR tmpBuff[MAX_PATH];

		GetModuleFileName(NULL, tmpBuff, MAX_PATH);

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

	void ResetDriverSettings() 
	{			
		
		BASS_ASIO_INFO info = { 0 };

		TCHAR bufferText[8] = {0};
		vst_buffer_size.GetLBText(vst_buffer_size.GetCurSel(), bufferText);

		TCHAR sampleRateText[8] = {0};
		vst_sample_rate.GetLBText(vst_sample_rate.GetCurSel(), sampleRateText);
		
		vst_buffer_size.ResetContent();
		vst_sample_rate.ResetContent();

		if (usingASIO) 
		{
			
#ifdef WIN64
			CString selectedOutputDriver = LoadOutputDriver(_T("Bass ASIO x64"));
#else
			CString selectedOutputDriver = LoadOutputDriver(_T("Bass ASIO"));
#endif
			vst_buffer_size.AddString(_T("Default"));	
			vst_buffer_size.AddString(_T("2 "));
			vst_buffer_size.AddString(_T("5"));
			vst_buffer_size.AddString(_T("10 "));
			vst_buffer_size.AddString(_T("15"));
			vst_buffer_size.AddString(_T("20 "));
			vst_buffer_size.AddString(_T("30"));
			vst_buffer_size.AddString(_T("50"));
			

			if(vst_buffer_size.SelectString(-1, bufferText) == CB_ERR) 
				vst_buffer_size.SelectString(-1, _T("Default"));

			vst_sample_format.EnableWindow(TRUE);
			vst_4chmode.EnableWindow(TRUE);

			CString selectedOutputChannel = selectedOutputDriver.Mid(3, 2);
			int selectedOutputChannelInt = _ttoi(selectedOutputChannel.GetString());
			selectedOutputDriver = selectedOutputDriver.Left(2);
			int selectedOutputDriverInt = _ttoi(selectedOutputDriver.GetString());						

			if (BASS_ASIO_Init(selectedOutputDriverInt, 0))
			{				
				BASS_ASIO_GetInfo(&info);
				
				bool is48K = false;
				if(BASS_ASIO_CheckRate(22050.0))vst_sample_rate.AddString(_T("22050"));
				if(BASS_ASIO_CheckRate(32000.0))vst_sample_rate.AddString(_T("32000"));
				if(BASS_ASIO_CheckRate(44100.0))vst_sample_rate.AddString(_T("44100"));
				if(BASS_ASIO_CheckRate(48000.0))
				{
					vst_sample_rate.AddString(_T("48000"));
					is48K = true;
				}
				if(BASS_ASIO_CheckRate(49716.0))vst_sample_rate.AddString(_T("49716"));
				if(BASS_ASIO_CheckRate(96000.0))vst_sample_rate.AddString(_T("96000"));
				if(BASS_ASIO_CheckRate(192000.0))vst_sample_rate.AddString(_T("192000"));
				
				if(vst_sample_rate.SelectString(-1, sampleRateText) == CB_ERR) 
				{
					if(is48K)vst_sample_rate.SelectString(-1, _T("48000"));
					else vst_sample_rate.SetCurSel(vst_sample_rate.GetCount() - 1);
				}

				BASS_ASIO_Free();
			}

			TCHAR tmpBuff[64];
			unsigned int realOffset = !info.outputs ? selectedOutputChannelInt + portBOffsetVal : (selectedOutputChannelInt + portBOffsetVal) % info.outputs;
			_stprintf_s(tmpBuff, 64, _T("4 channel mode (port A: ASIO Ch %d/%d; port B: ASIO Ch %u/%u)"), selectedOutputChannelInt, selectedOutputChannelInt + 1, realOffset, realOffset + 1);
			vst_4chmode.SetWindowText(tmpBuff);

		}
		else
		{
			vst_buffer_size.AddString(_T("40"));
			vst_buffer_size.AddString(_T("60"));
			vst_buffer_size.AddString(_T("80"));
			vst_buffer_size.AddString(_T("100"));
			vst_buffer_size.AddString(_T("120"));
			vst_buffer_size.AddString(_T("140"));
			vst_buffer_size.AddString(_T("160"));
			//vst_buffer_size.AddString(_T("180"));
			vst_buffer_size.AddString(_T("200"));

			if(vst_buffer_size.SelectString(-1, bufferText) == CB_ERR) 
				vst_buffer_size.SelectString(-1, _T("80"));
			
			vst_4chmode.SetWindowText(_T("4 channel mode (port A: Front speakers; port B: Rear speakers)"));

			bool is48K = false;
			UINT deviceId = GetWaveOutDeviceId();
			if (IsWaveFormatSupported(22050, deviceId))vst_sample_rate.AddString(_T("22050"));
			if (IsWaveFormatSupported(32000, deviceId))vst_sample_rate.AddString(_T("32000"));
			if (IsWaveFormatSupported(44100, deviceId))vst_sample_rate.AddString(_T("44100"));
			if (IsWaveFormatSupported(48000, deviceId))
			{
				vst_sample_rate.AddString(_T("48000"));
				is48K = true;
			}
			if (IsWaveFormatSupported(49716, deviceId))vst_sample_rate.AddString(_T("49716"));
			if (IsWaveFormatSupported(96000, deviceId))vst_sample_rate.AddString(_T("96000"));
			if (IsWaveFormatSupported(192000, deviceId))vst_sample_rate.AddString(_T("192000"));

			if(vst_sample_rate.SelectString(-1, sampleRateText) == CB_ERR) 
			{
				if (is48K)vst_sample_rate.SelectString(-1, _T("48000"));
				else vst_sample_rate.SetCurSel(vst_sample_rate.GetCount() - 1);
			}

			if(isWinNT4) {
				vst_4chmode.SetCheck(0);
				vst_sample_format.SelectString(-1, _T("16-bit Int"));
				vst_sample_format.EnableWindow(FALSE);
				vst_4chmode.EnableWindow(FALSE);

			}
		}

	}

	HWND CreateToolTip(int toolID, HWND hDlg, PTSTR pszText)
	{	
		HWND hwndTool = GetDlgItem(toolID);
		
		HWND hwndTip = CreateWindowEx(NULL, TOOLTIPS_CLASS, NULL, WS_POPUP | TTS_ALWAYSTIP, 
			CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, hDlg, NULL, GetModuleHandle(NULL), NULL);

		if (!hwndTool || !hwndTip)
		{
			return (HWND)NULL;
		}
	
		TOOLINFO toolInfo = {0};
		toolInfo.cbSize = TTTOOLINFOA_V1_SIZE;
		toolInfo.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
		toolInfo.hwnd = hDlg;
		toolInfo.uId = (UINT_PTR)hwndTool;
		toolInfo.lpszText = pszText;
		SendMessage(hwndTip, TTM_ADDTOOL, 0, (LPARAM)&toolInfo);

		return hwndTool;
	}

	LRESULT OnInitDialogView1(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled)
	{
		TCHAR fileversionBuff[32] = { 0 };
		effect = NULL;
		BASS_ASIO_INFO info = {0};		

		vst_sample_format = GetDlgItem(IDC_SAMPLEFORMAT);
		vst_sample_rate = GetDlgItem(IDC_SAMPLERATE);
		vst_buffer_size = GetDlgItem(IDC_BUFFERSIZE);
		vst_showvst = GetDlgItem(IDC_SHOWVST);
		vst_4chmode = GetDlgItem(IDC_USE4CH);
		vst_load = GetDlgItem(IDC_VSTLOAD);
		vst_info = GetDlgItem(IDC_VSTLOADED);
		vst_configure = GetDlgItem(IDC_VSTCONFIG);
		vst_effect = GetDlgItem(IDC_EFFECT);
		vst_vendor = GetDlgItem(IDC_VENDOR);
		file_info = GetDlgItem(IDC_FILEVERSION);
		volume_slider = GetDlgItem(IDC_VOLUME);
		vst_unload = GetDlgItem(IDC_UNLOAD);

		file_info.SetWindowText(GetFileVersion(fileversionBuff, _countof(fileversionBuff)));
		vst_effect.SetWindowText(_T("No VSTi loaded"));
		vst_vendor.SetWindowText(_T("No VSTi loaded"));
		
		vst_sample_rate.ResetContent();
		vst_buffer_size.ResetContent();

		CString selectedOutputDriver;
		CString selectedOutputChannel;
		int selectedOutputChannelInt;
		int selectedOutputDriverInt;

		vst_sample_format.AddString(_T("16-bit Int"));
		vst_sample_format.AddString(_T("32-bit Float"));

		if (!isWinNT4 || usingASIO) {			
			vst_sample_format.SelectString(-1, _T("32-bit Float"));
		}
		else {
			vst_sample_format.SelectString(-1, _T("16-bit Int"));
			vst_sample_format.EnableWindow(FALSE);
			vst_4chmode.EnableWindow(FALSE);
		}	

		if (usingASIO) 
		{

#ifdef WIN64
			selectedOutputDriver = LoadOutputDriver(_T("Bass ASIO x64"));
#else
			selectedOutputDriver = LoadOutputDriver(_T("Bass ASIO"));
#endif
			vst_buffer_size.AddString(_T("Default"));
					
			vst_buffer_size.AddString(_T("2 "));
			vst_buffer_size.AddString(_T("5"));
			vst_buffer_size.AddString(_T("10 "));
			vst_buffer_size.AddString(_T("15"));
			vst_buffer_size.AddString(_T("20 "));
			vst_buffer_size.AddString(_T("30"));
			vst_buffer_size.AddString(_T("50"));
			
			vst_buffer_size.SelectString(-1, _T("Default"));

			selectedOutputChannel = selectedOutputDriver.Mid(3, 2);
			selectedOutputChannelInt = _ttoi(selectedOutputChannel.GetString());
			selectedOutputDriver = selectedOutputDriver.Left(2);
			selectedOutputDriverInt = _ttoi(selectedOutputDriver.GetString());

			if (BASS_ASIO_Init(selectedOutputDriverInt, 0))	
			{
				BASS_ASIO_GetInfo(&info);

				bool is48K = false;
				if(BASS_ASIO_CheckRate(22050.0)) vst_sample_rate.AddString(_T("22050"));
				if(BASS_ASIO_CheckRate(32000.0))vst_sample_rate.AddString(_T("32000"));
				if(BASS_ASIO_CheckRate(44100.0))vst_sample_rate.AddString(_T("44100"));
				if(BASS_ASIO_CheckRate(48000.0))
				{
					vst_sample_rate.AddString(_T("48000"));
					is48K = true;
				}
				if(BASS_ASIO_CheckRate(49716.0))vst_sample_rate.AddString(_T("49716"));
				if(BASS_ASIO_CheckRate(96000.0))vst_sample_rate.AddString(_T("96000"));
				if(BASS_ASIO_CheckRate(192000.0))vst_sample_rate.AddString(_T("192000"));
				
				if(is48K)vst_sample_rate.SelectString(-1, _T("48000"));
				else vst_sample_rate.SetCurSel(vst_sample_rate.GetCount() - 1);

				BASS_ASIO_Free();
			}
		}
		else
		{
			vst_buffer_size.AddString(_T("40"));
			vst_buffer_size.AddString(_T("60"));
			vst_buffer_size.AddString(_T("80"));
			vst_buffer_size.AddString(_T("100"));
			vst_buffer_size.AddString(_T("120"));
			vst_buffer_size.AddString(_T("140"));
			vst_buffer_size.AddString(_T("160"));
			//vst_buffer_size.AddString(_T("180"));
			vst_buffer_size.AddString(_T("200"));
			vst_buffer_size.SelectString(-1, _T("80"));
			vst_4chmode.SetWindowText(_T("4 channel mode (port A: Front speakers; port B: Rear speakers)"));

			bool is48K = false;
			UINT deviceId = GetWaveOutDeviceId();
			if (IsWaveFormatSupported(22050, deviceId))vst_sample_rate.AddString(_T("22050"));
			if (IsWaveFormatSupported(32000, deviceId))vst_sample_rate.AddString(_T("32000"));
			if (IsWaveFormatSupported(44100, deviceId))vst_sample_rate.AddString(_T("44100"));
			if (IsWaveFormatSupported(48000, deviceId))
			{
				vst_sample_rate.AddString(_T("48000"));
				is48K = true;
			}
			if (IsWaveFormatSupported(49716, deviceId))vst_sample_rate.AddString(_T("49716"));
			if (IsWaveFormatSupported(96000, deviceId))vst_sample_rate.AddString(_T("96000"));
			if (IsWaveFormatSupported(192000, deviceId))vst_sample_rate.AddString(_T("192000"));
			
			if (is48K)vst_sample_rate.SelectString(-1, _T("48000"));
			else vst_sample_rate.SetCurSel(vst_sample_rate.GetCount() - 1);
		}

		volume_slider.SetRange(-12, 12);
		volume_slider.SetPos(0);	

		load_settings();

		TCHAR tmpBuff[64];
		if (usingASIO) 
		{			
			unsigned int realOffset = !info.outputs ? selectedOutputChannelInt + portBOffsetVal : (selectedOutputChannelInt + portBOffsetVal) % info.outputs;
			_stprintf_s(tmpBuff, 64, _T("4 channel mode (port A: ASIO Ch %d/%d; port B: ASIO Ch %u/%u)"), selectedOutputChannelInt, selectedOutputChannelInt + 1, realOffset, realOffset + 1);
			vst_4chmode.SetWindowText(tmpBuff);				
		}
		
		vst_sample_rate.GetWindowText(tmpBuff, 8); //list of valid sample rates could change.   
		SaveDwordValue(_T("SampleRate"), _ttoi(tmpBuff));

		//Unload tooltip
		HWND hWnd = GetAncestor(this->m_hWnd, GA_ROOT);
		CreateToolTip(IDC_UNLOAD, hWnd, _T("Unload selected VSTi"));

		origDropdownWidth = vst_info.GetDroppedWidth();

		return TRUE;
	}
};

class CView3 : public CDialogImpl<CView3>
{
	CListBox playbackDevices;
	CComboBox driverMode, portbOffset;
	CButton asio_openctlp;
	CStatic portbOffsetText;
	bool driverChanged;
	bool resetList;
	std::vector<int> drvChArr;

public:	
	CView3() :
		playbackDevices(),
		driverMode(), portbOffset(),
		asio_openctlp(),
		portbOffsetText(),
		driverChanged(),
		resetList(),
		drvChArr()
	{		
		drvChArr.reserve(8); //reasonable starting capacity
	}

	~CView3()
	{

	}

	enum
	{
		IDD = IDD_SOUND
	};

	BEGIN_MSG_MAP(CView3)
		MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialogView3)
		COMMAND_ID_HANDLER(IDC_ASIOCTRLP, OnButtonOpen)
		COMMAND_HANDLER(IDC_COMBO1, CBN_SELCHANGE, OnCbnSelchangeCombo1)
		COMMAND_HANDLER(IDC_LIST1, LBN_SELCHANGE, OnLbnSelchangeList1)
		//COMMAND_HANDLER(IDC_COMBO_PORTB, CBN_EDITCHANGE, OnCbnEditchangeComboPortb)
		COMMAND_HANDLER(IDC_COMBO_PORTB, CBN_SELCHANGE, OnCbnSelchangeComboPortb)
	END_MSG_MAP()	

	bool GetDriverChanged() 
	{
		return driverChanged;
	}

	void SetDriverChanged(bool changed) 
	{
		driverChanged = changed;
	}

	void ShowPortBControls(bool show) 
	{
		if (show)
		{
			portbOffset.ShowWindow(SW_SHOW);
			portbOffsetText.ShowWindow(SW_SHOW);
		}
		else 
		{
			portbOffset.ShowWindow(SW_HIDE);
			portbOffsetText.ShowWindow(SW_HIDE);
		}
	}

	bool SaveOutputDriver(CString valueName, CString value)
	{
		CRegKeyEx reg;

		/// Create the Output Driver registry subkey
		long result = reg.Create(HKEY_CURRENT_USER, _T("Software\\VSTi Driver\\Output Driver"), 0, 0, KEY_WRITE);

		if (result != NO_ERROR)
		{
			return false;
		}

		/// Save the OutputDriver settings
		result = reg.SetStringValue(valueName, value);

		reg.Close();

		return result == NO_ERROR;
	}

	void LoadWaveOutDrivers()
	{
		CString deviceItem;
		CString deviceName;
		WAVEOUTCAPS caps;
		CString selectedOutputDriver = LoadOutputDriver(_T("WinMM WaveOut"));

		for (size_t deviceId = -1; waveOutGetDevCaps(deviceId, &caps, sizeof(caps)) == MMSYSERR_NOERROR; ++deviceId) {

			deviceName = CString(caps.szPname);
			playbackDevices.AddString(deviceName);
			if (selectedOutputDriver.IsEmpty() && deviceId == -1) playbackDevices.SelectString(0, deviceName);
			if (selectedOutputDriver.CompareNoCase(deviceName) == 0) playbackDevices.SelectString(0, deviceName);

		}		

	}	

	void LoadAsioDrivers() 
	{        
		CString deviceItem;
		CString deviceName;
#ifdef WIN64
		CString selectedOutputDriver = LoadOutputDriver(_T("Bass ASIO x64"));
#else
		CString selectedOutputDriver = LoadOutputDriver(_T("Bass ASIO"));
#endif       

		/* not needed for a vector
		DWORD deviceCount = 0;
		for (int i = 0; BASS_ASIO_GetDeviceInfo(i, &asioDeviceInfo); i++)
		{
		deviceCount++;
		}
		*/

		bool isSelected = false;

		playbackDevices.InitStorage(64, 64 * sizeof(TCHAR));
		playbackDevices.SendMessage(WM_SETREDRAW, FALSE, 0); //let's speed up ListBox drawing in case of many ASIO drivers/channels 

		BASS_ASIO_DEVICEINFO asioDeviceInfo;

		char firstDriverName[24] = { 0 };
		for (int deviceId = 0; BASS_ASIO_GetDeviceInfo(deviceId, &asioDeviceInfo); ++deviceId)
		{			
			if (!deviceId) lstrcpynA(firstDriverName, asioDeviceInfo.name, 23);
			if (usePrivateAsioOnly && IsVistaOrNewer() && deviceId && !strcmp(firstDriverName,"VSTDriver-ASIO2WASAPI")) break;
			if (!BASS_ASIO_Init(deviceId, 0)) continue;

			deviceName = CString(asioDeviceInfo.name);           
			BASS_ASIO_INFO info;
			BASS_ASIO_GetInfo(&info);
			drvChArr.push_back(info.outputs);
			BASS_ASIO_CHANNELINFO channelInfo;	

			for (unsigned int channel = 0; BASS_ASIO_ChannelGetInfo(FALSE, channel, &channelInfo); ++channel)
			{
				deviceItem.Format(_T("%02d.%02u - %s %s"), deviceId, channel, deviceName, CString(channelInfo.name));

				//if (playbackDevices.FindStringExact(0, deviceItem) == LB_ERR)
				{
					playbackDevices.AddString(deviceItem);
					if (!isSelected && ((selectedOutputDriver.IsEmpty() && channel == 0) || deviceItem.CompareNoCase(selectedOutputDriver) == 0))
					{                       
						playbackDevices.SelectString(0, deviceItem);
						isSelected = true;
					}
				}
			}

			BASS_ASIO_Free();
		}

		playbackDevices.SendMessage(WM_SETREDRAW, TRUE, 0);
		playbackDevices.RedrawWindow(NULL, NULL, RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN);

		DWORD drvId = _ttoi(selectedOutputDriver.Left(2));

		if (drvId >= drvChArr.size()) return;

		unsigned int portCount = drvChArr[drvId];
		portbOffset.ResetContent();
		TCHAR tmpBuff[8];
		for (unsigned int i = 1; i < portCount / 2; i++)
		{
			portbOffset.AddString(_ultot(i * 2, tmpBuff, 10));						
		}
		if (portbOffset.SelectString(-1,_ultot(portBOffsetVal, tmpBuff, 10)) ==  CB_ERR)
			portbOffset.SelectString(-1, _T("2"));

	}	

	LRESULT OnInitDialogView3(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/)
	{
		playbackDevices = GetDlgItem(IDC_LIST1);
		driverMode = GetDlgItem(IDC_COMBO1);
		portbOffset = GetDlgItem(IDC_COMBO_PORTB);
		portbOffsetText = GetDlgItem(IDC_STATIC_PORTB);
		asio_openctlp = GetDlgItem(IDC_ASIOCTRLP);

		driverMode.AddString(_T("WinMM WaveOut"));

		if (isASIO)	driverMode.AddString(_T("Bass ASIO"));

		CString selectedDriverMode;

		if (usingASIO) {
			selectedDriverMode = _T("Bass ASIO");
			driverMode.SelectString(0, _T("Bass ASIO"));
			LoadAsioDrivers();
			asio_openctlp.ShowWindow(SW_SHOW);
			if (is4chMode) {
				portbOffsetText.ShowWindow(SW_SHOW);
				portbOffset.ShowWindow(SW_SHOW);
			}


		}
		else {
			selectedDriverMode = _T("WinMM WaveOut");
			driverMode.SelectString(0, _T("WinMM WaveOut"));
			LoadWaveOutDrivers();
			asio_openctlp.ShowWindow(SW_HIDE);
		}     
		
		SaveOutputDriver(_T("Driver Mode"), selectedDriverMode);

		return 0;
	}

	LRESULT OnButtonOpen(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/ )
	{
		CString deviceName;
		CString selectedOutputDriver;

		int index = playbackDevices.GetCurSel();
		if (index == -1)
		{
			playbackDevices.SetCurSel(0);
			index = 0;
			BOOL dummy;
			OnLbnSelchangeList1(0, 0, 0, dummy);
		}
		int length = playbackDevices.GetTextLen(index);

		playbackDevices.GetText(index, selectedOutputDriver.GetBuffer(max(length, 2)));
		selectedOutputDriver.ReleaseBuffer();		

		int selectedOutputDriverInt = _ttoi(selectedOutputDriver.Left(2));	

		CRegKeyEx reg;
		DWORD value = 0;
		
		bool isAsio2Wasapi = selectedOutputDriver.Find(_T("VSTDriver-ASIO2WASAPI"), 0) != -1;
		if (isAsio2Wasapi)
		{
			reg.Open(HKEY_CURRENT_USER, _T("Software\\VSTi Driver\\Output Driver\\ASIO2WASAPI"), KEY_READ);
			reg.QueryDWORDValue(_T("Channels"), value);
		}

		BASS_ASIO_SetWindow(m_hWnd);
		BASS_ASIO_Init(selectedOutputDriverInt, 0);
		if (!BASS_ASIO_ControlPanel() && BASS_ASIO_ErrorGetCode() == BASS_ERROR_ALREADY) return 0;
		BASS_ASIO_Free();

		driverChanged = true;

		if (isAsio2Wasapi)
		{				
			DWORD newValue = 0;
			reg.QueryDWORDValue(_T("Channels"), newValue);
			reg.Close();			
			
			if (value != newValue)
			{
				resetList = true;
				BOOL dummyB;
				OnCbnSelchangeCombo1(1, 1, (HWND)1, dummyB);
				resetList = false;

				BASS_ASIO_INFO info = {0};
				BASS_ASIO_Init(0, 0);				
				BASS_ASIO_GetInfo(&info);	
				BASS_ASIO_Free();

				drvChArr[0] = info.outputs;
				portbOffset.ResetContent();
				TCHAR tmpBuff[8];
				for (unsigned int i = 1; i < info.outputs / 2; i++)
				{
					portbOffset.AddString(_ultot(i * 2, tmpBuff, 10));
				}
				if (portbOffset.SelectString(-1, _ultot(portBOffsetVal, tmpBuff, 10)) == CB_ERR)
					portbOffset.SelectString(-1, _T("2"));
			}
		}

		return 0;
	}


	LRESULT OnCbnSelchangeCombo1(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
	{
		CString newDriverMode;
		CString selectedDriverMode;

		int index = driverMode.GetCurSel();
		int length = driverMode.GetLBTextLen(index);

		if (driverMode.GetLBText(index, newDriverMode.GetBuffer(max(length, 1))))
		{
			newDriverMode.ReleaseBuffer();

			selectedDriverMode = LoadOutputDriver(_T("Driver Mode"));

			if (resetList || selectedDriverMode.CompareNoCase(newDriverMode) != 0)
			{				
				playbackDevices.ResetContent();

				driverChanged = true;

				if (newDriverMode.CompareNoCase(_T("Bass ASIO")) == 0)
				{
					usingASIO = true;
					LoadAsioDrivers();
					asio_openctlp.ShowWindow(SW_SHOW);
					if(is4chMode) ShowPortBControls(true);
				}
				else
				{
					usingASIO = false;
					LoadWaveOutDrivers();
					asio_openctlp.ShowWindow(SW_HIDE);
					ShowPortBControls(false);
				}

				SaveOutputDriver(_T("Driver Mode"), newDriverMode);
			}
		}        

		return 0;
	}

	LRESULT OnLbnSelchangeList1(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
	{   
		CString selectedOutputDriver;
		CString selectedDriverMode;

		int index = playbackDevices.GetCurSel();
		int length = playbackDevices.GetTextLen(index);

		if (playbackDevices.GetText(index, selectedOutputDriver.GetBuffer(max(length, 1))))
		{
			selectedOutputDriver.ReleaseBuffer();

			index = driverMode.GetCurSel();
			length = driverMode.GetLBTextLen(index);

			if (driverMode.GetLBText(index, selectedDriverMode.GetBuffer(max(length, 1))))
			{
				selectedDriverMode.ReleaseBuffer();

				if (selectedDriverMode.CompareNoCase(_T("Bass ASIO")) == 0)
				{

#ifdef WIN64
					SaveOutputDriver(_T("Bass ASIO x64"), selectedOutputDriver);
#else
					SaveOutputDriver(_T("Bass ASIO"), selectedOutputDriver);
#endif
					driverChanged = true;									

					DWORD drvId = _ttoi(selectedOutputDriver.Left(2));
					if (drvId >= drvChArr.size()) return 0;
					unsigned int portCount = drvChArr[drvId];
					portbOffset.ResetContent();
					TCHAR tmpBuff[8];
					for (unsigned int i = 1; i < portCount / 2; i++)
					{
						portbOffset.AddString(_ultot(i * 2, tmpBuff, 10));						
					}
					if (portbOffset.SelectString(-1,_ultot(portBOffsetVal, tmpBuff, 10)) ==  CB_ERR)
					{
						portbOffset.SelectString(-1, _T("2"));
						BOOL dummy;
						OnCbnSelchangeComboPortb(0, 0, 0, dummy);
					}

				}
				else
				{
					SaveOutputDriver(_T("WinMM WaveOut"), selectedOutputDriver);
					driverChanged = true;
				}
			}
		}

		return 0;
	}

	LRESULT CView3::OnCbnEditchangeComboPortb(WORD wNotifyCode, WORD wID, HWND hWndCtl, BOOL& bHandled)
	{	

		//OnCbnSelchangeComboPortb(wNotifyCode, wID, hWndCtl, bHandled);   
		return 0;
	}

	LRESULT CView3::OnCbnSelchangeComboPortb(WORD wNotifyCode, WORD wID, HWND hWndCtl, BOOL& bHandled)
	{	   
		TCHAR tmpBuff[8];	   
		portbOffset.GetWindowText(tmpBuff, 8);
		portBOffsetVal = _ttoi(tmpBuff);
		SaveDwordValue(_T("PortBOffset"), portBOffsetVal);
		driverChanged = true;	   

		return 0;
	}

};



class CView2 : public CDialogImpl<CView2>
{
	CComboBox synthlist;
	CButton apply;
	CStatic groupBox;

	//typedef DWORD(STDAPICALLTYPE * pmodMessage)(UINT uDeviceID, UINT uMsg, DWORD_PTR dwUser, DWORD_PTR dwParam1, DWORD_PTR dwParam2);

public:

	CView2() :
		synthlist(),
		apply(),
		groupBox() {}

	enum { IDD = IDD_ADVANCED };
	BEGIN_MSG_MAP(CView1)
		MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialogView2)
		COMMAND_ID_HANDLER(IDC_SNAPPLY, OnButtonApply)
	END_MSG_MAP()

	void SetGroupBoxCaption(TCHAR* caption) 
	{
		groupBox.SetWindowText(caption);
	}

	LRESULT OnInitDialogView2(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/)
	{
		synthlist = GetDlgItem(IDC_SYNTHLIST);
		apply = GetDlgItem(IDC_SNAPPLY);
		groupBox = GetDlgItem(IDC_GROUPBOX2);
		load_midisynths_mapper();
		return TRUE;
	}

	LRESULT OnButtonApply(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
	{
		set_midisynth_mapper();
		return 0;

	}

	/* These only work on Windows 6.1 and older (but not on XP...) */
	void set_midisynth_mapper()
	{
		CRegKeyEx reg;
		CRegKeyEx subkey;
		CString device_name;
		long lRet;
		int selection = synthlist.GetCurSel();
		int n = synthlist.GetLBTextLen(selection);
		synthlist.GetLBText(selection, device_name.GetBuffer(max(n, 1)));
		device_name.ReleaseBuffer(n);
		lRet = reg.Create(HKEY_CURRENT_USER, _T("Software\\Microsoft\\Windows\\CurrentVersion\\Multimedia"), REG_NONE, REG_OPTION_NON_VOLATILE, KEY_WRITE);
		if (lRet == ERROR_SUCCESS) lRet = reg.DeleteSubKey(_T("MIDIMap"));
		if (lRet == ERROR_SUCCESS) lRet = subkey.Create(reg, _T("MIDIMap"), REG_NONE, REG_OPTION_NON_VOLATILE, KEY_WRITE);
		if (lRet == ERROR_SUCCESS) lRet = subkey.SetStringValue(_T("szPname"), device_name);
		if (lRet == ERROR_SUCCESS)
		{
			MessageBox(_T("MIDI synth set!"), _T("VST MIDI Driver"), MB_ICONINFORMATION);
		}
		else
		{
			MessageBox(_T("Can't set MIDI registry key!"), _T("VST MIDI Driver"), MB_ICONSTOP);
		}
		subkey.Close();
		reg.Close();
	}

	void load_midisynths_mapper()
	{
		LONG lResult;
		CRegKeyEx reg;
		CString device_name = _T("");
		
		lResult = reg.Create(HKEY_CURRENT_USER, _T("Software\\Microsoft\\Windows\\CurrentVersion\\Multimedia\\MIDIMap"), REG_NONE, REG_OPTION_NON_VOLATILE, KEY_READ);
		if (lResult == ERROR_SUCCESS)
		{
			ULONG size = 128;
			lResult =  reg.QueryStringValue(_T("szPname"), device_name.GetBuffer(size), &size);
			reg.Close();
			if (lResult == ERROR_SUCCESS) device_name.ReleaseBuffer(size);
		}		
		int device_count = midiOutGetNumDevs();
		for (int i = 0; i < device_count; ++i) {
			MIDIOUTCAPS Caps;
			ZeroMemory(&Caps, sizeof(Caps));
			MMRESULT Error = midiOutGetDevCaps(i, &Caps, sizeof(Caps));
			if (Error != MMSYSERR_NOERROR)
				continue;
			if(_tcscmp(Caps.szPname, _T("CoolSoft MIDIMapper"))) synthlist.AddString(Caps.szPname);
		}
		int index = 0;
		index = synthlist.FindStringExact(-1, device_name);
		if (index == CB_ERR) index = 0;
		synthlist.SetCurSel(index);
	}
};


#endif // !defined(AFX_VIEWS_H__20020629_8D64_963C_A351_0080AD509054__INCLUDED_)

