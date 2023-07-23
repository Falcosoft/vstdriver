#if !defined(AFX_VIEWS_H__20020629_8D64_963C_A351_0080AD509054__INCLUDED_)
#define AFX_VIEWS_H__20020629_8D64_963C_A351_0080AD509054__INCLUDED_

#include <iostream>
#include <fstream> 
#include "utf8conv.h"
#include "../external_packages/mmddk.h"
#include "../driver/VSTDriver.h"

/// Define BASSASIO functions as pointers
#define BASSASIODEF(f) (WINAPI *f)
#define LOADBASSASIOFUNCTION(f) *((void**)&f)=GetProcAddress(bassasio,#f)

#include "../external_packages/bassasio.h"

using namespace std;
using namespace utf8util;

typedef AEffect* (*PluginEntryProc) (audioMasterCallback audioMaster);
static INT_PTR CALLBACK EditorProc (HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// for VSTDriver
extern "C" HINSTANCE hinst_vst_driver = NULL;

static HINSTANCE bassasio = NULL;       // bassasio handle  

static BOOL isASIO = false;
static bool is4chMode = false;
static DWORD portBOffsetVal = 2;


struct MyDLGTEMPLATE: DLGTEMPLATE
{
	WORD ext[3];
	MyDLGTEMPLATE ()
	{ memset (this, 0, sizeof(*this)); };
};

std::wstring stripExtension(const std::wstring& fileName)
{
	const size_t length = fileName.length();
	for (int i=0; i!=length; ++i)
	{
		if (fileName[i]=='.') 
		{
			return fileName.substr(0,i);
		}
	}
	return fileName;
}

static BOOL IsASIO() 
{		
	TCHAR installpath[MAX_PATH];        
	TCHAR bassasiopath[MAX_PATH];	

	GetModuleFileName(hinst_vst_driver, installpath, MAX_PATH);
	PathRemoveFileSpec(installpath);        

	// Load Bass Asio
	lstrcpy(bassasiopath, installpath);
	lstrcat(bassasiopath, L"\\bassasio.dll");
	bassasio = LoadLibrary(bassasiopath);        

	if (bassasio)
	{          
		LOADBASSASIOFUNCTION(BASS_ASIO_Init);
		LOADBASSASIOFUNCTION(BASS_ASIO_Free);
		LOADBASSASIOFUNCTION(BASS_ASIO_GetInfo);
		LOADBASSASIOFUNCTION(BASS_ASIO_GetDeviceInfo);
		LOADBASSASIOFUNCTION(BASS_ASIO_ChannelGetInfo);
		LOADBASSASIOFUNCTION(BASS_ASIO_ControlPanel);
		LOADBASSASIOFUNCTION(BASS_ASIO_CheckRate);			

		BASS_ASIO_DEVICEINFO info;
		return BASS_ASIO_GetDeviceInfo(0, &info);			
	}	

	return FALSE;	
}

static void settings_load(VSTDriver * effect)
{
	ifstream file;
	long lResult;
	TCHAR vst_path[256] = {0};
	ULONG size;
	CRegKeyEx reg;
	wstring fname;
	lResult = reg.Create(HKEY_CURRENT_USER, L"Software\\VSTi Driver", 0, 0, KEY_READ | KEY_WOW64_32KEY);
	if (lResult == ERROR_SUCCESS){
		lResult = reg.QueryStringValue(L"plugin",NULL,&size);
		if (lResult == ERROR_SUCCESS) {
			reg.QueryStringValue(L"plugin",vst_path,&size);
			wstring ext = vst_path;
			fname = stripExtension(ext);
			fname += L".set";
            file.open(fname.c_str(),ifstream::binary);
			if (file.good())
			{
				file.seekg(0,ifstream::end);
				size_t chunk_size = file.tellg();
				file.seekg(0);
				vector<uint8_t> chunk;
				chunk.resize( chunk_size );
#if (defined(_MSC_VER) && (_MSC_VER < 1600))
				if (chunk_size) {
					file.read( (char*) &chunk.front(), chunk_size );
					if (effect) effect->setChunk( &chunk.front(), (unsigned int)chunk_size );
				}
#else
				if (chunk_size) {
					file.read( (char*) chunk.data(), chunk_size );
					if (effect) effect->setChunk( chunk.data(), (unsigned int)chunk_size );
				}
#endif
			}
			file.close();
		}
		reg.Close();
	}
}

static void settings_save(VSTDriver * effect)
{
	ofstream file;
	long lResult;
	TCHAR vst_path[256] = {0};
	ULONG size;
	CRegKeyEx reg;
	wstring fname;
	lResult = reg.Create(HKEY_CURRENT_USER, L"Software\\VSTi Driver", 0, 0, KEY_READ | KEY_WOW64_32KEY); // falco fix: otherwise reg.QueryStringValue gets back an ACCESS_DENIED(5) error.
	if (lResult == ERROR_SUCCESS){
		lResult = reg.QueryStringValue(L"plugin",NULL,&size);
		if (lResult == ERROR_SUCCESS) {
			reg.QueryStringValue(L"plugin",vst_path,&size);
			wstring ext = vst_path;
			fname = stripExtension(ext);
			fname += L".set";
			file.open(fname.c_str(),ofstream::binary);
			if (file.good())
			{
				vector<uint8_t> chunk;
				if (effect) effect->getChunk( chunk );
#if (defined(_MSC_VER) && (_MSC_VER < 1600))
				if (chunk.size()) file.write( ( const char * ) &chunk.front(), chunk.size() );
#else
				if (chunk.size()) file.write( ( const char * ) chunk.data(), chunk.size() );
#endif
			}
			file.close();
		}
		reg.Close();
	}
}

static CString LoadOutputDriver(CString valueName)
{
	CRegKeyEx reg;
	CString value;

	long result = reg.Open(HKEY_CURRENT_USER, L"Software\\VSTi Driver\\Output Driver", KEY_READ | KEY_WOW64_32KEY);
	if (result != NO_ERROR)
	{
		return value;
	}

	ULONG size;

	result = reg.QueryStringValue(valueName, NULL, &size);
	if (result == NO_ERROR && size > 0)
	{
		reg.QueryStringValue(valueName, value.GetBuffer(size), &size);
		value.ReleaseBuffer();
	}

	reg.Close();

	return value;
}

#if _MSC_VER > 1000
#pragma once
#endif // _MSC_VER > 1000

class CView1 : public CDialogImpl<CView1>
{
	
	CEdit vst_info;
	CComboBox vst_buffer_size, vst_sample_rate;
	CButton vst_load, vst_configure, vst_showvst, vst_4chmode;
	CStatic vst_vendor, vst_effect, file_info;
	CTrackBarCtrl volume_slider;
	TCHAR vst_path[MAX_PATH];

	VSTDriver * effect;
public:
   enum { IDD = IDD_MAIN };
   BEGIN_MSG_MAP(CView1)
	   MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialogView1)
	   COMMAND_ID_HANDLER(IDC_VSTLOAD,OnButtonAdd)
	   COMMAND_ID_HANDLER(IDC_VSTCONFIG,OnButtonConfig)
	   COMMAND_HANDLER(IDC_SHOWVST, BN_CLICKED, OnClickedSHOWVST)
	   COMMAND_HANDLER(IDC_SAMPLERATE, CBN_SELCHANGE, OnCbnSelchangeSamplerate)
	   COMMAND_HANDLER(IDC_BUFFERSIZE, CBN_SELCHANGE, OnCbnSelchangeBuffersize)
	   MESSAGE_HANDLER(WM_HSCROLL, OnHScroll)
	   COMMAND_HANDLER(IDC_USE4CH, BN_CLICKED, OnBnClickedUse4ch)
   END_MSG_MAP()

   CView1() { effect = NULL; }
 
   ~CView1()
   { 
	   free_vst(); 
	   if (bassasio)
        {         
		    FreeLibrary(bassasio);
            bassasio = NULL;
        }
   }

   void load_settings()
   {
	   long lResult;
	   vst_path[0] = 0;
	   ULONG size;
	   DWORD reg_value; 
	   wchar_t tmpBuff[34];
	   CRegKeyEx reg;
	   lResult = reg.Create(HKEY_CURRENT_USER, L"Software\\VSTi Driver");
	   if (lResult == ERROR_SUCCESS){
		   
		   lResult = reg.QueryStringValue(L"plugin",NULL,&size);
		   if (lResult == ERROR_SUCCESS) {
			   reg.QueryStringValue(L"plugin",vst_path,&size);
		   }
		   lResult = reg.QueryDWORDValue(L"ShowVstDialog",reg_value);
		   if (lResult == ERROR_SUCCESS) {
			   vst_showvst.SetCheck(reg_value);
		   }
		   lResult = reg.QueryDWORDValue(L"Use4ChannelMode",reg_value);
		   if (lResult == ERROR_SUCCESS) {
			   vst_4chmode.SetCheck(reg_value);
			   if (reg_value) is4chMode = true;
		   }		   
		   lResult = reg.QueryDWORDValue(L"SampleRate",reg_value);
		   if (lResult == ERROR_SUCCESS) {			   
			   vst_sample_rate.SelectString(-1, _ultow(reg_value, tmpBuff, 10));			   
		   }
		   lResult = reg.QueryDWORDValue(L"BufferSize",reg_value);
		   if (lResult == ERROR_SUCCESS) {
			    vst_buffer_size.SelectString(-1, _ultow(reg_value, tmpBuff, 10));
		   }
		   lResult = reg.QueryDWORDValue(L"Gain",reg_value);
		   if (lResult == ERROR_SUCCESS) {
			   volume_slider.SetPos((int)reg_value);
		   }
		   lResult = reg.QueryDWORDValue(L"PortBOffset",reg_value);
		   if (lResult == ERROR_SUCCESS) {
			   portBOffsetVal = reg_value;
		   }

		   reg.Close();
		   vst_info.SetWindowText(vst_path);
		  load_vst(vst_path);
		 if(effect) vst_configure.EnableWindow(effect->hasEditor());
	   }
	   
   }

   LRESULT OnButtonAdd(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/ )
   {
	   TCHAR szFileName[MAX_PATH];
	   LPCTSTR sFiles = 
		   L"VSTi instruments (*.dll)\0*.dll\0"
		   L"All Files (*.*)\0*.*\0\0";
	   CFileDialog dlg( TRUE, NULL, vst_path, OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT, sFiles);
	   if (dlg.DoModal() == IDOK)
	   {
		   lstrcpy(szFileName,dlg.m_szFileName);
		   if (load_vst(szFileName))
		   {
			   // HKEY hKey, hSubKey;
			   long lResult;
			   CRegKeyEx reg;
			   lResult = reg.Create(HKEY_CURRENT_USER, L"Software\\VSTi Driver", 0, 0, KEY_WRITE | KEY_WOW64_32KEY);
			   reg.SetStringValue(L"plugin",szFileName);			   
			   reg.Close();
			   vst_info.SetWindowText(szFileName);
			   vst_configure.EnableWindow(effect->hasEditor());
		   }
		   // do stuff
	   }
	   return 0;
   }
   
   LRESULT OnClickedSHOWVST(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
   {
	   long lResult;
	   CRegKeyEx reg;
	   lResult = reg.Create(HKEY_CURRENT_USER, L"Software\\VSTi Driver", 0, 0, KEY_WRITE | KEY_WOW64_32KEY);
	   reg.SetDWORDValue(L"ShowVstDialog",vst_showvst.GetCheck());
	   reg.Close();
	
	   return 0;
   }

   LRESULT OnBnClickedUse4ch(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
   {
	   long lResult;
	   CRegKeyEx reg;
	   lResult = reg.Create(HKEY_CURRENT_USER, L"Software\\VSTi Driver", 0, 0, KEY_WRITE | KEY_WOW64_32KEY);
	   reg.SetDWORDValue(L"Use4ChannelMode",vst_4chmode.GetCheck());
	   reg.Close();	
	   is4chMode = vst_4chmode.GetCheck() !=  BST_UNCHECKED;

       return 0;
   }

   LRESULT OnCbnSelchangeSamplerate(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
   {
	   long lResult;
	   CRegKeyEx reg;
	   wchar_t tmpBuff[8];
	   lResult = reg.Create(HKEY_CURRENT_USER, L"Software\\VSTi Driver", 0, 0, KEY_WRITE | KEY_WOW64_32KEY);
	   vst_sample_rate.GetWindowTextW(tmpBuff, 8);
	   reg.SetDWORDValue(L"SampleRate",wcstol(tmpBuff, NULL, 10));
	   reg.Close();

	   return 0;
   } 

   LRESULT OnCbnSelchangeBuffersize(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
   {
	   long lResult;
	   CRegKeyEx reg;
	   wchar_t tmpBuff[8];
	   lResult = reg.Create(HKEY_CURRENT_USER, L"Software\\VSTi Driver", 0, 0, KEY_WRITE | KEY_WOW64_32KEY);
	   vst_buffer_size.GetWindowTextW(tmpBuff, 8);
	   reg.SetDWORDValue(L"BufferSize",wcstol(tmpBuff, NULL, 10));
	   reg.Close();

	   return 0;
   } 

   LRESULT CView1::OnHScroll(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled) 
   {	   
	   if ((HWND)lParam == volume_slider.m_hWnd && LOWORD(wParam) == SB_ENDSCROLL)
	   {
		   int vol;
		   long lResult;
		   CRegKeyEx reg;	       
	       lResult = reg.Create(HKEY_CURRENT_USER, L"Software\\VSTi Driver", 0, 0, KEY_WRITE | KEY_WOW64_32KEY);
	       vol = volume_slider.GetPos();
	       reg.SetDWORDValue(L"Gain", (DWORD)vol);
	       reg.Close();
	   }
	   return 0;
   }

   LRESULT OnButtonConfig(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/ )
   {
	   if(effect && effect->hasEditor())
	   {
		   HWND m_hWnd = GetAncestor(this->m_hWnd, GA_ROOT);
		   ::EnableWindow(m_hWnd, FALSE);
		   effect->displayEditorModal();
		   ::EnableWindow(m_hWnd, TRUE);
	   }
	   return 0;
   }

   void free_vst()
   {
	   settings_save( effect );
	   delete effect;
	   effect = NULL;
   }

   BOOL load_vst(TCHAR * szPluginPath)
   {
	   if(effect) free_vst(); // falco fix: otherwise an empty save occures at every start.
	   effect = new VSTDriver;
	   if (!effect->OpenVSTDriver(szPluginPath))
	   {
		   delete effect;
		   effect = NULL;
		   if (szPluginPath && *szPluginPath)
			   MessageBox(L"This is NOT a VSTi synth!");
		   vst_effect.SetWindowText(L"No VSTi loaded");
		   vst_vendor.SetWindowText(L"No VSTi loaded");		   
		   vst_info.SetWindowText(L"No VSTi loaded");
		   return FALSE;
	   }

	   string conv;
	   effect->getEffectName(conv);
	   wstring effect_str = utf16_from_ansi(conv);
	   vst_effect.SetWindowText(effect_str.c_str());
	   effect->getVendorString(conv);
	   wstring vendor_str = utf16_from_ansi(conv);
	   vst_vendor.SetWindowText(vendor_str.c_str());	  

	   settings_load( effect );

	   return TRUE;
   }

    #pragma comment(lib,"Version.lib") 
    wchar_t* GetFileVersion(wchar_t* Result)
    {
        DWORD               dwSize = 0;
        BYTE* pVersionInfo = NULL;
        VS_FIXEDFILEINFO* pFileInfo = NULL;
        UINT                pLenFileInfo = 0;
        wchar_t tmpBuff[MAX_PATH];

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

        lstrcat(Result, L"version: ");
        lstrcat(Result, _ultow((pFileInfo->dwFileVersionMS >> 16) & 0xffff, tmpBuff, 10));
        lstrcat(Result, L".");
        lstrcat(Result, _ultow((pFileInfo->dwFileVersionMS) & 0xffff, tmpBuff, 10));
        lstrcat(Result, L".");
        lstrcat(Result, _ultow((pFileInfo->dwFileVersionLS >> 16) & 0xffff, tmpBuff, 10));
        lstrcat(Result, L".");
        lstrcat(Result, _ultow((pFileInfo->dwFileVersionLS) & 0xffff, tmpBuff, 10));
        
        return Result;
    }

    void ResetBufferSizes() 
	{			
		vst_buffer_size.ResetContent();
		vst_sample_rate.ResetContent();

		CString selectedDriverMode = LoadOutputDriver(L"Driver Mode");
		if (isASIO && selectedDriverMode.CompareNoCase(L"Bass ASIO") == 0) 
		{
			vst_buffer_size.AddString(L"Default");
			vst_buffer_size.AddString(L"2");
			vst_buffer_size.AddString(L"5");
			vst_buffer_size.AddString(L"10");
			vst_buffer_size.AddString(L"15");
			vst_buffer_size.AddString(L"20");
			vst_buffer_size.AddString(L"30");
			vst_buffer_size.AddString(L"40");			
			vst_buffer_size.SelectString(-1, L"Default");			

#ifdef WIN64
			CString selectedOutputDriver = LoadOutputDriver(L"Bass ASIO x64");
#else
			CString selectedOutputDriver = LoadOutputDriver(L"Bass ASIO");
#endif
			CString selectedOutputChannel = selectedOutputDriver.Mid(3, 2);
			int selectedOutputChannelInt = _wtoi(selectedOutputChannel.GetString());
			selectedOutputDriver = selectedOutputDriver.Left(2);
			int selectedOutputDriverInt = _wtoi(selectedOutputDriver.GetString());
			
			wchar_t tmpBuff[64];
			swprintf(tmpBuff, 64, L"4 channel mode (port A: ASIO Ch %d/%d; port B: ASIO Ch %d/%d)", selectedOutputChannelInt, selectedOutputChannelInt + 1, selectedOutputChannelInt + portBOffsetVal, selectedOutputChannelInt + portBOffsetVal + 1); 
			vst_4chmode.SetWindowTextW(tmpBuff);
			
			if (BASS_ASIO_Init(selectedOutputDriverInt, 0))
			{
				if(BASS_ASIO_CheckRate(22050.0)) vst_sample_rate.AddString(L"22050");
				if(BASS_ASIO_CheckRate(32000.0))vst_sample_rate.AddString(L"32000");
				if(BASS_ASIO_CheckRate(44100.0))vst_sample_rate.AddString(L"44100");
				if(BASS_ASIO_CheckRate(48000.0))vst_sample_rate.AddString(L"48000");
				if(BASS_ASIO_CheckRate(49716.0))vst_sample_rate.AddString(L"49716");
				if(BASS_ASIO_CheckRate(96000.0))vst_sample_rate.AddString(L"96000");
				if(BASS_ASIO_CheckRate(192000.0))vst_sample_rate.AddString(L"192000");
				vst_sample_rate.SelectString(-1, L"48000");

				BASS_ASIO_Free();
			}

		}
		else
		{
			vst_buffer_size.AddString(L"40");
			vst_buffer_size.AddString(L"60");
			vst_buffer_size.AddString(L"80");
			vst_buffer_size.AddString(L"100");
			vst_buffer_size.AddString(L"120");
			vst_buffer_size.AddString(L"140");
			vst_buffer_size.AddString(L"160");
			vst_buffer_size.AddString(L"180");
			vst_buffer_size.AddString(L"200");
			vst_buffer_size.SelectString(-1, L"80");
			vst_4chmode.SetWindowTextW(L"4 channel mode (port A: Front speakers; port B: Rear speakers)");

			vst_sample_rate.AddString(L"22050");
			vst_sample_rate.AddString(L"32000");
			vst_sample_rate.AddString(L"44100");
			vst_sample_rate.AddString(L"48000");
			vst_sample_rate.AddString(L"49716");
			vst_sample_rate.AddString(L"96000");
			vst_sample_rate.AddString(L"192000");
			vst_sample_rate.SelectString(-1, L"48000");			
		}

	}

	LRESULT OnInitDialogView1(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/)
	{
		wchar_t fileversionBuff[32] = { 0 };
		effect = NULL;

		isASIO = IsASIO();

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

		file_info.SetWindowText(GetFileVersion(fileversionBuff));
		vst_effect.SetWindowText(L"No VSTi loaded");
		vst_vendor.SetWindowText(L"No VSTi loaded");		
		vst_info.SetWindowText(L"No VSTi loaded");
        
		vst_sample_rate.ResetContent();
		vst_buffer_size.ResetContent();
		
		CString selectedDriverMode = LoadOutputDriver(L"Driver Mode");
		CString selectedOutputDriver;
		CString selectedOutputChannel;
		int selectedOutputChannelInt;
		int selectedOutputDriverInt;

		if (isASIO && selectedDriverMode.CompareNoCase(L"Bass ASIO") == 0) 
		{
			vst_buffer_size.AddString(L"Default");
			vst_buffer_size.AddString(L"2");
			vst_buffer_size.AddString(L"5");
			vst_buffer_size.AddString(L"10");
			vst_buffer_size.AddString(L"15");
			vst_buffer_size.AddString(L"20");
			vst_buffer_size.AddString(L"30");
			vst_buffer_size.AddString(L"40");			
			vst_buffer_size.SelectString(-1, L"Default");
			
#ifdef WIN64
			selectedOutputDriver = LoadOutputDriver(L"Bass ASIO x64");
#else
			selectedOutputDriver = LoadOutputDriver(L"Bass ASIO");
#endif
			selectedOutputChannel = selectedOutputDriver.Mid(3, 2);
			selectedOutputChannelInt = _wtoi(selectedOutputChannel.GetString());
			selectedOutputDriver = selectedOutputDriver.Left(2);
			selectedOutputDriverInt = _wtoi(selectedOutputDriver.GetString());
				
			if (BASS_ASIO_Init(selectedOutputDriverInt, 0))	
			{
				if(BASS_ASIO_CheckRate(22050.0)) vst_sample_rate.AddString(L"22050");
				if(BASS_ASIO_CheckRate(32000.0))vst_sample_rate.AddString(L"32000");
				if(BASS_ASIO_CheckRate(44100.0))vst_sample_rate.AddString(L"44100");
				if(BASS_ASIO_CheckRate(48000.0))vst_sample_rate.AddString(L"48000");
				if(BASS_ASIO_CheckRate(49716.0))vst_sample_rate.AddString(L"49716");
				if(BASS_ASIO_CheckRate(96000.0))vst_sample_rate.AddString(L"96000");
				if(BASS_ASIO_CheckRate(192000.0))vst_sample_rate.AddString(L"192000");
				vst_sample_rate.SelectString(-1, L"48000");

				BASS_ASIO_Free();
			}
		}
		else
		{
			vst_buffer_size.AddString(L"40");
			vst_buffer_size.AddString(L"60");
			vst_buffer_size.AddString(L"80");
			vst_buffer_size.AddString(L"100");
			vst_buffer_size.AddString(L"120");
			vst_buffer_size.AddString(L"140");
			vst_buffer_size.AddString(L"160");
			vst_buffer_size.AddString(L"180");
			vst_buffer_size.AddString(L"200");
			vst_buffer_size.SelectString(-1, L"80");
			vst_4chmode.SetWindowTextW(L"4 channel mode (port A: Front speakers; port B: Rear speakers)");

			vst_sample_rate.AddString(L"22050");
			vst_sample_rate.AddString(L"32000");
			vst_sample_rate.AddString(L"44100");
			vst_sample_rate.AddString(L"48000");
			vst_sample_rate.AddString(L"49716");
			vst_sample_rate.AddString(L"96000");
			vst_sample_rate.AddString(L"192000");
			vst_sample_rate.SelectString(-1, L"48000");
		}

        volume_slider.SetRange(-12, 12);
		volume_slider.SetPos(0);	
		
		load_settings();

		if (isASIO && selectedDriverMode.CompareNoCase(L"Bass ASIO") == 0) 
		{
			wchar_t tmpBuff[64];
			swprintf(tmpBuff, 64, L"4 channel mode (port A: ASIO Ch %d/%d; port B: ASIO Ch %d/%d)", selectedOutputChannelInt, selectedOutputChannelInt + 1, selectedOutputChannelInt + portBOffsetVal, selectedOutputChannelInt + portBOffsetVal + 1); 
			vst_4chmode.SetWindowTextW(tmpBuff);
		}
        
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
	std::vector<int> drvChArr;
           
public:	

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

    CView3()
    {		
		driverChanged = false;
		drvChArr.reserve(8); //reasonable starting capacity
    }

    ~CView3()
    {		
		        
    }

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
        long result = reg.Create(HKEY_CURRENT_USER, L"Software\\VSTi Driver\\Output Driver", 0, 0, KEY_WRITE | KEY_WOW64_32KEY);

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
		WAVEOUTCAPSW caps;
        CString selectedOutputDriver = LoadOutputDriver(L"WinMM WaveOut");

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
        CString selectedOutputDriver = LoadOutputDriver(L"Bass ASIO x64");
#else
		CString selectedOutputDriver = LoadOutputDriver(L"Bass ASIO");
#endif       
		
		/* not needed for a vector
		DWORD deviceCount = 0;
		for (int i = 0; BASS_ASIO_GetDeviceInfo(i, &asioDeviceInfo); i++)
		{
			deviceCount++;
		}
		*/
		
		playbackDevices.InitStorage(64, 64 * sizeof(TCHAR));
		playbackDevices.SendMessageW(WM_SETREDRAW, FALSE, 0); //let's speed up ListBox drawing in case of many ASIO drivers/channels 

        BASS_ASIO_DEVICEINFO asioDeviceInfo;
		for (int deviceId = 0; BASS_ASIO_Init(deviceId, 0); ++deviceId)
        {
            BASS_ASIO_GetDeviceInfo(deviceId, &asioDeviceInfo);

            deviceName = CString(asioDeviceInfo.name);            

            BASS_ASIO_INFO info;
            BASS_ASIO_GetInfo(&info);
			drvChArr.push_back(info.outputs);

            BASS_ASIO_CHANNELINFO channelInfo;

            for (DWORD channel = 0; BASS_ASIO_ChannelGetInfo(FALSE, channel, &channelInfo); ++channel)
            {
                deviceItem.Format(L"%02d.%02d - %s %s", deviceId, channel, deviceName, CString(channelInfo.name));
               
                //if (playbackDevices.FindStringExact(0, deviceItem) == LB_ERR)
                {
                    playbackDevices.AddString(deviceItem);
                    if ((selectedOutputDriver.IsEmpty() && channel == 0) || deviceItem.CompareNoCase(selectedOutputDriver) == 0)
                    {                       
                        playbackDevices.SelectString(0, deviceItem);
                    }
                }
            }

            BASS_ASIO_Free();
		}

		playbackDevices.SendMessageW(WM_SETREDRAW, TRUE, 0);
		playbackDevices.RedrawWindow(NULL, NULL, RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN);
		
		DWORD drvId = wcstol(selectedOutputDriver.Left(2), NULL, 10);
		
		if (drvId >= drvChArr.size()) return;
		
		unsigned int portCount = drvChArr[drvId];
		portbOffset.ResetContent();
		wchar_t tmpBuff[8];
		for (unsigned int i = 1; i < portCount / 2; i++)
		{
			portbOffset.AddString(_ultow(i * 2, tmpBuff, 10));						
		}
		if (portbOffset.SelectString(-1,_ultow(portBOffsetVal, tmpBuff, 10)) ==  CB_ERR)
			portbOffset.SelectString(-1, L"2");

	}	

    LRESULT OnInitDialogView3(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/)
    {
        playbackDevices = GetDlgItem(IDC_LIST1);
        driverMode = GetDlgItem(IDC_COMBO1);
		portbOffset = GetDlgItem(IDC_COMBO_PORTB);
		portbOffsetText = GetDlgItem(IDC_STATIC_PORTB);
		asio_openctlp = GetDlgItem(IDC_ASIOCTRLP);
        
		driverMode.AddString(L"WinMM WaveOut"); 
			
		if (isASIO)	driverMode.AddString(L"Bass ASIO");
	
		CString selectedDriverMode = LoadOutputDriver(L"Driver Mode");
	
		if (isASIO && selectedDriverMode.CompareNoCase(L"Bass ASIO") == 0) {
			selectedDriverMode = L"Bass ASIO";
			driverMode.SelectString(0, L"Bass ASIO");
			LoadAsioDrivers();
			asio_openctlp.ShowWindow(SW_SHOW);
			if (is4chMode) {
				portbOffsetText.ShowWindow(SW_SHOW);
				portbOffset.ShowWindow(SW_SHOW);
			}


		}
		else {
			selectedDriverMode = L"WinMM WaveOut";
			driverMode.SelectString(0, L"WinMM WaveOut");
			LoadWaveOutDrivers();
			asio_openctlp.ShowWindow(SW_HIDE);
		}        

        return 0;
    }

	LRESULT OnButtonOpen(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/ )
	{
		CString deviceName;
#ifdef WIN64
        CString selectedOutputDriver = LoadOutputDriver(L"Bass ASIO x64");
#else
		CString selectedOutputDriver = LoadOutputDriver(L"Bass ASIO");
#endif

		 BASS_ASIO_DEVICEINFO asioDeviceInfo;

        for (int deviceId = 0; BASS_ASIO_Init(deviceId, 0); ++deviceId)
        {
            BASS_ASIO_GetDeviceInfo(deviceId, &asioDeviceInfo);

			deviceName = CString(asioDeviceInfo.name);

			if (selectedOutputDriver.Find(deviceName) != -1) 
			{				
				BASS_ASIO_ControlPanel();
				BASS_ASIO_Free();
				break;
			}

			BASS_ASIO_Free();
		}

		return 0;		
	}
	

    LRESULT OnCbnSelchangeCombo1(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
    {
        CString newDriverMode;
        CString selectedDriverMode;

        int index = driverMode.GetCurSel();
        int length = driverMode.GetLBTextLen(index);

        if (driverMode.GetLBText(index, newDriverMode.GetBuffer(length)))
        {
            newDriverMode.ReleaseBuffer();

            selectedDriverMode = LoadOutputDriver(L"Driver Mode");

            if (selectedDriverMode.CompareNoCase(newDriverMode) != 0)
            {
                playbackDevices.ResetContent();

				driverChanged = true;

                if (newDriverMode.CompareNoCase(L"Bass ASIO") == 0)
                {
                    LoadAsioDrivers();
					asio_openctlp.ShowWindow(SW_SHOW);
					if(is4chMode) ShowPortBControls(true);
                }
                else
                {
                    LoadWaveOutDrivers();
					asio_openctlp.ShowWindow(SW_HIDE);
					ShowPortBControls(false);
                }

                SaveOutputDriver(L"Driver Mode", newDriverMode);
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

        if (playbackDevices.GetText(index, selectedOutputDriver.GetBuffer(length)))
        {
            selectedOutputDriver.ReleaseBuffer();

            int index = driverMode.GetCurSel();
            int length = driverMode.GetLBTextLen(index);

            if (driverMode.GetLBText(index, selectedDriverMode.GetBuffer(length)))
            {
                selectedDriverMode.ReleaseBuffer();

                if (selectedDriverMode.CompareNoCase(L"Bass ASIO") == 0)
                {
                    
#ifdef WIN64
					SaveOutputDriver(L"Bass ASIO x64", selectedOutputDriver);
#else
					SaveOutputDriver(L"Bass ASIO", selectedOutputDriver);
#endif
					driverChanged = true;									
	   
	                DWORD drvId = wcstol(selectedOutputDriver.Left(2), NULL, 10);
					if (drvId >= drvChArr.size()) return 0;
					unsigned int portCount = drvChArr[drvId];
					portbOffset.ResetContent();
					wchar_t tmpBuff[8];
					for (unsigned int i = 1; i < portCount / 2; i++)
					{
						portbOffset.AddString(_ultow(i * 2, tmpBuff, 10));						
					}
					if (portbOffset.SelectString(-1,_ultow(portBOffsetVal, tmpBuff, 10)) ==  CB_ERR)
					{
						portbOffset.SelectString(-1, L"2");
						BOOL dummy;
						OnCbnSelchangeComboPortb(0, 0, 0, dummy);
					}
										
                }
                else
                {
                    SaveOutputDriver(L"WinMM WaveOut", selectedOutputDriver);
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
	   long lResult;
	   CRegKeyEx reg;
	   wchar_t tmpBuff[8];
	   lResult = reg.Create(HKEY_CURRENT_USER, L"Software\\VSTi Driver", 0, 0, KEY_WRITE | KEY_WOW64_32KEY);
	   portbOffset.GetWindowTextW(tmpBuff, 8);
	   portBOffsetVal = wcstol(tmpBuff, NULL, 10);
	   reg.SetDWORDValue(L"PortBOffset", portBOffsetVal);
	   reg.Close();
	   driverChanged = true;
	   
	   return 0;
	}

};



class CView2 : public CDialogImpl<CView2>
{
	CComboBox synthlist;
	CButton apply;

	typedef DWORD(STDAPICALLTYPE * pmodMessage)(UINT uDeviceID, UINT uMsg, DWORD_PTR dwUser, DWORD_PTR dwParam1, DWORD_PTR dwParam2);

public:
	enum { IDD = IDD_ADVANCED };
	BEGIN_MSG_MAP(CView1)
		MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialogView2)
		COMMAND_ID_HANDLER(IDC_SNAPPLY, OnButtonApply)
	END_MSG_MAP()

	LRESULT OnInitDialogView2(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/)
	{
		synthlist = GetDlgItem(IDC_SYNTHLIST);
		apply = GetDlgItem(IDC_SNAPPLY);
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
		synthlist.GetLBText(selection, device_name.GetBuffer(n));
		device_name.ReleaseBuffer(n);
		lRet = reg.Create(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Multimedia", REG_NONE, REG_OPTION_NON_VOLATILE, KEY_WRITE | KEY_WOW64_32KEY);
		lRet = reg.DeleteSubKey(L"MIDIMap");
		lRet = subkey.Create(reg, L"MIDIMap", REG_NONE, REG_OPTION_NON_VOLATILE, KEY_WRITE);
		lRet = subkey.SetStringValue(L"szPname", device_name);
		if (lRet == ERROR_SUCCESS)
		{
			MessageBox(L"MIDI synth set!", L"Notice.", MB_ICONINFORMATION);
		}
		else
		{
			MessageBox(L"Can't set MIDI registry key", L"Damn!", MB_ICONSTOP);
		}
		subkey.Close();
		reg.Close();
	}

	void load_midisynths_mapper()
	{
		LONG lResult;
		CRegKeyEx reg;
		CString device_name;
		ULONG size = 128;
		lResult = reg.Create(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Multimedia\\MIDIMap", REG_NONE, REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WOW64_32KEY);
		reg.QueryStringValue(L"szPname", device_name.GetBuffer(size), &size);
		reg.Close();
		device_name.ReleaseBuffer(size);
		int device_count = midiOutGetNumDevs();
		for (int i = 0; i < device_count; ++i) {
			MIDIOUTCAPS Caps;
			ZeroMemory(&Caps, sizeof(Caps));
			MMRESULT Error = midiOutGetDevCaps(i, &Caps, sizeof(Caps));
			if (Error != MMSYSERR_NOERROR)
				continue;
			synthlist.AddString(Caps.szPname);
		}
		int index = 0;
		index = synthlist.FindStringExact(-1, device_name);
		if (index == CB_ERR) index = 0;
		synthlist.SetCurSel(index);
	}
};


#endif // !defined(AFX_VIEWS_H__20020629_8D64_963C_A351_0080AD509054__INCLUDED_)

