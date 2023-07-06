#if !defined(AFX_VIEWS_H__20020629_8D64_963C_A351_0080AD509054__INCLUDED_)
#define AFX_VIEWS_H__20020629_8D64_963C_A351_0080AD509054__INCLUDED_

#include <iostream>
#include <fstream> 
#include "utf8conv.h"
#include "../external_packages/mmddk.h"
using namespace std;
using namespace utf8util;
#include "../driver/VSTDriver.h"
typedef AEffect* (*PluginEntryProc) (audioMasterCallback audioMaster);
static INT_PTR CALLBACK EditorProc (HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// for VSTDriver
extern "C" HINSTANCE hinst_vst_driver = NULL;

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

void settings_load(VSTDriver * effect)
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

void settings_save(VSTDriver * effect)
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

using namespace std;
using namespace utf8util;
#if _MSC_VER > 1000
#pragma once
#endif // _MSC_VER > 1000

class CView1 : public CDialogImpl<CView1>
{
	
	CEdit vst_info;
	CButton vst_load, vst_configure, vst_showvst;
	CStatic vst_vendor, vst_effect, vst_product;
	TCHAR vst_path[MAX_PATH];
	VSTDriver * effect;
public:
   enum { IDD = IDD_MAIN };
   BEGIN_MSG_MAP(CView1)
	   MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialogView1)
	   COMMAND_ID_HANDLER(IDC_VSTLOAD,OnButtonAdd)
	   COMMAND_ID_HANDLER(IDC_VSTCONFIG,OnButtonConfig)
	   COMMAND_HANDLER(IDC_SHOWVST, BN_CLICKED, OnClickedSHOWVST)
   END_MSG_MAP()

   CView1() { effect = NULL; }
   ~CView1() { free_vst(); }

   void load_settings()
   {
	   long lResult;
	   vst_path[0] = 0;
	   ULONG size;
	   DWORD showVstDialog; 
	   CRegKeyEx reg;
	   lResult = reg.Create(HKEY_CURRENT_USER, L"Software\\VSTi Driver");
	   if (lResult == ERROR_SUCCESS){
		   lResult = reg.QueryStringValue(L"plugin",NULL,&size);
		   if (lResult == ERROR_SUCCESS) {
			   reg.QueryStringValue(L"plugin",vst_path,&size);
		   }
		   lResult = reg.QueryDWORDValue(L"ShowVstDialog",showVstDialog);
		   if (lResult == ERROR_SUCCESS) {
			   vst_showvst.SetCheck(showVstDialog);
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
		   vst_product.SetWindowText(L"No VSTi loaded");
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
	   effect->getProductString(conv);
	   wstring product_str = utf16_from_ansi(conv);
	   vst_product.SetWindowText(product_str.c_str());

	   settings_load( effect );

	   return TRUE;
   }

	LRESULT OnInitDialogView1(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/)
	{
		effect = NULL;
		vst_showvst = GetDlgItem(IDC_SHOWVST);
		vst_load = GetDlgItem(IDC_VSTLOAD);
		vst_info = GetDlgItem(IDC_VSTLOADED);
		vst_configure = GetDlgItem(IDC_VSTCONFIG);
		vst_effect = GetDlgItem(IDC_EFFECT);
		vst_vendor = GetDlgItem(IDC_VENDOR);
		vst_product = GetDlgItem(IDC_PRODUCT);
		vst_effect.SetWindowText(L"No VSTi loaded");
		vst_vendor.SetWindowText(L"No VSTi loaded");
		vst_product.SetWindowText(L"No VSTi loaded");
		vst_info.SetWindowText(L"No VSTi loaded");
		load_settings();
		return TRUE;
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

	/* These only work on Windows 6.1 and older */
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

