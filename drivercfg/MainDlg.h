// MainDlg.h : interface of the CMainDlg class
//
/////////////////////////////////////////////////////////////////////////////
#include <atlframe.h>
#include <atlctrls.h>
#include <atldlgs.h>
#include <atlwin.h>
#include "resource.h"
#include "DlgTabCtrl.h"
#include "Views.h"


#pragma once

static void InvalidParamHandler(const wchar_t* expression, const wchar_t* function, const wchar_t* file, unsigned int line, uintptr_t pReserved)
{
	if (MessageBox(FindWindow(NULL, windowName), _T("An unexpected invalid parameter error occured.\r\nDo you want to try to continue?"), _T("VST MIDI Driver"), MB_YESNO | MB_ICONERROR | MB_SYSTEMMODAL) == IDNO)
		TerminateProcess(GetCurrentProcess(), 1);
}

BOOL IsWin8OrNewer()
{
	OSVERSIONINFOEX osvi;
	BOOL bOsVersionInfoEx;
	ZeroMemory(&osvi, sizeof(OSVERSIONINFOEX));
	osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEX);
	bOsVersionInfoEx = GetVersionEx((OSVERSIONINFO*)&osvi);
	if (bOsVersionInfoEx == FALSE) return FALSE;
	if (VER_PLATFORM_WIN32_NT == osvi.dwPlatformId &&
		(osvi.dwMajorVersion > 6 ||
		(osvi.dwMajorVersion == 6 && osvi.dwMinorVersion > 1)))
		return TRUE;
	return FALSE;
}

//Setting Midi mapper value this simple way does not work on Win XP either but on XP you can do it properly with built-in control panel anyway...
BOOL IsWinVistaOrWin7()  
{
	OSVERSIONINFOEX osvi;
	BOOL bOsVersionInfoEx;
	ZeroMemory(&osvi, sizeof(OSVERSIONINFOEX));
	osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEX);
	bOsVersionInfoEx = GetVersionEx((OSVERSIONINFO*)&osvi);
	if (bOsVersionInfoEx == FALSE) return FALSE;
	if (VER_PLATFORM_WIN32_NT == osvi.dwPlatformId &&
		((osvi.dwMajorVersion == 6 && osvi.dwMinorVersion == 0) ||
		(osvi.dwMajorVersion == 6 && osvi.dwMinorVersion == 1)))
		return TRUE;
	return FALSE;
}

BOOL IsCoolSoftMidiMapperInstalled() 
{
	MIDIOUTCAPS Caps;
	ZeroMemory(&Caps, sizeof(Caps));
	MMRESULT result = midiOutGetDevCaps(0, &Caps, sizeof(Caps));
	if (result != MMSYSERR_NOERROR)
		return FALSE;
	if (!_tcscmp(Caps.szPname, _T("CoolSoft MIDIMapper"))) 
		return TRUE;

	return FALSE;
}

class CMainDlg : public CDialogImpl<CMainDlg>, public CUpdateUI<CMainDlg>,
	public CMessageFilter, public CIdleHandler
{
public:
	enum { IDD = IDD_MAINDLG };
	CDialogTabCtrl m_ctrlTab;
	CView1 m_view1;
	CView2 m_view2;
	CView3 m_view3;
	CView4 m_view4;
	CButton alwaysOnTop;

	virtual BOOL PreTranslateMessage(MSG* pMsg)
	{
		return CWindow::IsDialogMessage(pMsg);
	}

	virtual BOOL OnIdle()
	{
		return FALSE;
	}

	BEGIN_UPDATE_UI_MAP(CMainDlg)
	END_UPDATE_UI_MAP()

	BEGIN_MSG_MAP(CMainDlg)
		MESSAGE_HANDLER(WM_HELP, OnHelp)
		MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
		MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
		COMMAND_ID_HANDLER(IDOK, OnOK)
		COMMAND_ID_HANDLER(IDCANCEL, OnCancel)
		NOTIFY_HANDLER(IDC_TAB, TCN_SELCHANGE, OnTcnSelchangeTab)
		COMMAND_HANDLER(IDC_ALWAYSONTOP, BN_CLICKED, OnBnClickedAlwaysOnTop)
		REFLECT_NOTIFICATIONS();		
	END_MSG_MAP()

		// Handler prototypes (uncomment arguments if needed):
		//	LRESULT MessageHandler(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/)
		//	LRESULT CommandHandler(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
		//	LRESULT NotifyHandler(int /*idCtrl*/, LPNMHDR /*pnmh*/, BOOL& /*bHandled*/)
	

	LRESULT OnHelp(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/)
	{
		BOOL dummy;
		m_view4.OnButtonHelp(0, 0, m_hWnd, dummy);
		return 1;
	}

	LRESULT OnInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/)
	{
		_set_invalid_parameter_handler(InvalidParamHandler);

		//only 1 instance of a kind		
#ifdef WIN64		
		SetWindowText(windowName);
		CreateMutex(NULL, true, _T("vstmididrvcfg64"));		
#else	
		
		CreateMutex(NULL, true, _T("vstmididrvcfg32"));
#endif	
				
		if(GetLastError() == ERROR_ALREADY_EXISTS) 
		{
			
			SetWindowText(_T("Closing"));			
			HWND winHandle = ::FindWindow(NULL, windowName);			
			if (winHandle) 
			{
				m_hWnd = winHandle; //hacky but works...
				CenterWindow();
				::SetForegroundWindow(winHandle);				
			}
				
			ExitProcess(0);
		}	
		
		// center the dialog on the screen
		alwaysOnTop = GetDlgItem(IDC_ALWAYSONTOP);
		alwaysOnTop.SetCheck(1);
		CenterWindow();
		// set icons
		HICON hIcon = AtlLoadIconImage(IDR_MAINFRAME, LR_DEFAULTCOLOR, ::GetSystemMetrics(SM_CXICON), ::GetSystemMetrics(SM_CYICON));
		SetIcon(hIcon, TRUE);
		HICON hIconSmall = AtlLoadIconImage(IDR_MAINFRAME, LR_DEFAULTCOLOR, ::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON));
		SetIcon(hIconSmall, FALSE);
		
		m_ctrlTab.SubclassWindow(GetDlgItem(IDC_TAB));
		m_view1.Create(m_hWnd);

		bool showWindowsMidiTab = IsWinVistaOrWin7() || (IsWin8OrNewer() && IsCoolSoftMidiMapperInstalled());
		if (showWindowsMidiTab)
		{
			m_view2.Create(m_hWnd);
		}		

		m_view3.Create(m_hWnd);
		
		m_view4.Create(m_hWnd);

		TCITEM tci = { 0 };
		tci.mask = TCIF_TEXT;

		tci.pszText = _T("VST settings");
		m_ctrlTab.InsertItem(0, &tci, m_view1);
		
		tci.pszText = _T("Audio devices");		
		m_ctrlTab.InsertItem(1, &tci, m_view3);

		tci.pszText = _T("Advanced");
		m_ctrlTab.InsertItem(2, &tci, m_view4);

		if (showWindowsMidiTab)
		{
			tci.pszText = _T("MIDI");
			m_ctrlTab.InsertItem(3, &tci, m_view2);
			if (!IsWinVistaOrWin7()) m_view2.SetGroupBoxCaption(_T("Default MIDI Synth (through Coolsoft MIDI Mapper)"));
		}


		m_ctrlTab.SetCurSel(0);

		// register object for message filtering and idle updates
		CMessageLoop* pLoop = _Module.GetMessageLoop();
		ATLASSERT(pLoop != NULL);
		pLoop->AddMessageFilter(this);
		pLoop->AddIdleHandler(this);

		UIAddChildWindowContainer(m_hWnd);

		return TRUE;
	}

	LRESULT OnTcnSelchangeTab(int idCtrl, LPNMHDR pNMHDR, BOOL& bHandled)
	{		
		if(m_ctrlTab.GetCurSel() == 0 && m_view3.GetDriverChanged())
		{
			BOOL dummy;
			m_view1.ResetDriverSettings();
			m_view1.OnCbnSelchangeBuffersize(0, 0, 0, dummy);
			m_view1.OnCbnSelchangeSamplerate(0, 0, 0, dummy);
			m_view3.SetDriverChanged(false);
		}
		else if (m_ctrlTab.GetCurSel() == 1)
		{			
			m_view3.ShowPortBControls(usingASIO && is4chMode);	
		}

		bHandled = false;
		return 1;
	}

	LRESULT OnDestroy(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/)
	{
		// unregister message filtering and idle updates
		CMessageLoop* pLoop = _Module.GetMessageLoop();
		ATLASSERT(pLoop != NULL);
		pLoop->RemoveMessageFilter(this);
		pLoop->RemoveIdleHandler(this);

		return 0;
	}

	LRESULT OnOK(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
	{
		// TODO: Add validation code 
		CloseDialog(wID);
		return 0;
	}

	LRESULT OnCancel(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
	{
		CloseDialog(wID);
		return 0;
	}

	void CloseDialog(int nVal)
	{
		DestroyWindow();
		::PostQuitMessage(nVal);
	}

	LRESULT CMainDlg::OnBnClickedAlwaysOnTop(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
	{
		if(alwaysOnTop.GetCheck())
			SetWindowPos(HWND_TOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
		else
			SetWindowPos(HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);		

		return 0;
	}
};
