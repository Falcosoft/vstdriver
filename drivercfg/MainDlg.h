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


/*BOOL IsWin8OrNewer()
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
}*/

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

class CMainDlg : public CDialogImpl<CMainDlg>, public CUpdateUI<CMainDlg>,
		public CMessageFilter, public CIdleHandler
{
public:
	enum { IDD = IDD_MAINDLG };
	CDialogTabCtrl m_ctrlTab;
	CView1 m_view1;
	CView2 m_view2;
	CView3 m_view3;

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
		MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
		MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
		COMMAND_ID_HANDLER(IDOK, OnOK)
		COMMAND_ID_HANDLER(IDCANCEL, OnCancel)
		NOTIFY_HANDLER(IDC_TAB, TCN_SELCHANGE, OnTcnSelchangeTab)
		REFLECT_NOTIFICATIONS();		
	END_MSG_MAP()

// Handler prototypes (uncomment arguments if needed):
//	LRESULT MessageHandler(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/)
//	LRESULT CommandHandler(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
//	LRESULT NotifyHandler(int /*idCtrl*/, LPNMHDR /*pnmh*/, BOOL& /*bHandled*/)

	LRESULT OnInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/)
	{
		// center the dialog on the screen
		CenterWindow();
		// set icons
		HICON hIcon = AtlLoadIconImage(IDR_MAINFRAME, LR_DEFAULTCOLOR, ::GetSystemMetrics(SM_CXICON), ::GetSystemMetrics(SM_CYICON));
		SetIcon(hIcon, TRUE);
		HICON hIconSmall = AtlLoadIconImage(IDR_MAINFRAME, LR_DEFAULTCOLOR, ::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON));
		SetIcon(hIconSmall, FALSE);
#ifdef WIN64

		SetWindowText(L"VSTi Driver Configuration (x64)");
#endif		
		m_ctrlTab.SubclassWindow(GetDlgItem(IDC_TAB));
		m_view1.Create(m_hWnd);
		
		if (IsWinVistaOrWin7()) m_view2.Create(m_hWnd);
		
		m_view3.Create(m_hWnd);
		TCITEM tci = { 0 };
		tci.mask = TCIF_TEXT;
		tci.pszText = _T("VST settings");
		m_ctrlTab.InsertItem(0, &tci, m_view1);
		tci.pszText = _T("OutPut devices");
		m_ctrlTab.InsertItem(1, &tci, m_view3);
		if (IsWinVistaOrWin7())
		{
			tci.pszText = _T("Windows MIDI");
			m_ctrlTab.InsertItem(2, &tci, m_view2);
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
	
		BOOL dummy;
		if(m_ctrlTab.GetCurSel() == 0 && m_view3.GetDriverChanged())
		{
			m_view1.ResetBufferSizes();
			m_view1.OnCbnSelchangeBuffersize(0, 0, 0, dummy);
			m_view1.OnCbnSelchangeSamplerate(0, 0, 0, dummy);
			m_view3.SetDriverChanged(false);
		}
		else if (m_ctrlTab.GetCurSel() == 1)
		{
			CString selectedDriverMode = LoadOutputDriver(L"Driver Mode");
		  	m_view3.ShowPortBControls(isASIO && selectedDriverMode.CompareNoCase(L"Bass ASIO") == 0 && is4chMode);	
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
};
