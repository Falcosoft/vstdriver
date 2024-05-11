///
#include "stdafx.h"
#include "log.h"

#ifdef LOG
void Log(LPCTSTR szFormat, ...)
{
	static FILE* fpLog = NULL;
	va_list l;
	va_start(l, szFormat);

	if (!fpLog)
	{
		TCHAR logfile[MAX_PATH];
		GetEnvironmentVariable(_T("TEMP"), logfile, _countof(logfile));
		_tcscat_s(logfile, _T("\\vsthost.log"));
		fpLog = _tfopen(logfile, _T("a"));
	}
	if (fpLog)
	{
		_vftprintf(fpLog, szFormat, l);
		fflush(fpLog);  // make sure it's out!
	}
	va_end(l);
}
//#else
//void NoLog(LPCTSTR szFormat, ...) {}
//#define Log while(0) NoLog
#endif

#ifdef MINIDUMP
/*****************************************************************************/
/* LoadMiniDump : tries to load minidump functionality                       */
/*****************************************************************************/

bool LoadMiniDump()
{
	static HMODULE hmodDbgHelp = NULL;

	if (!pMiniDumpWriteDump && !hmodDbgHelp)
	{
		TCHAR szBuf[_MAX_PATH];
		szBuf[0] = _T('\0');
		::GetEnvironmentVariable(_T("ProgramFiles"), szBuf, MAX_PATH);
		UINT omode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
		TCHAR* p = szBuf + _tcslen(szBuf);
		if (*szBuf && !hmodDbgHelp)
		{
#ifdef _M_IX86
			_tcscpy(p, _T("\\Debugging Tools for Windows (x86)\\dbghelp.dll"));
#elif defined(_M_X64)
			_tcscpy(p, _T("\\Debugging Tools for Windows (x64)\\dbghelp.dll"));
#endif
			if (!(hmodDbgHelp = LoadLibrary(szBuf))) *p = '\0';
		}
		if (*szBuf && !hmodDbgHelp)
		{
			_tcscpy(p, _T("\\Debugging Tools for Windows\\dbghelp.dll"));
			if (!(hmodDbgHelp = LoadLibrary(szBuf))) *p = '\0';
		}
#if defined _M_X64
		if (*szBuf && !hmodDbgHelp)       /* still not found?                  */
		{                               /* try 64-bit version                */
			_tcscpy(p, _T("\\Debugging Tools for Windows 64-Bit\\dbghelp.dll"));
			if (!(hmodDbgHelp = LoadLibrary(szBuf))) *p = '\0';
		}
#endif
		if (!hmodDbgHelp)      // if not already loaded, try to load a default-one
			hmodDbgHelp = LoadLibrary(_T("dbghelp.dll"));
		SetErrorMode(omode);

		if (!hmodDbgHelp)                 /* if STILL not loaded,              */
			return false;                   /* return with error                 */

		pMiniDumpWriteDump = (PMiniDumpWriteDump)GetProcAddress(hmodDbgHelp, "MiniDumpWriteDump");
	}
	if (!pMiniDumpWriteDump)
		return false;

	if (!*szExeName)
	{
		TCHAR szPath[_MAX_PATH];
		TCHAR* lbsl = NULL, * ldot = NULL;
		_tcscpy_s(szExeName, _T("vsthost"));
		GetModuleFileName(NULL, szPath, _MAX_PATH);
		for (TCHAR* p = szPath; *p; p++)
			if (*p == _T('\\'))
				lbsl = p;
			else if (*p == _T('.'))
				ldot = p;
		if (lbsl && ldot && ldot > lbsl + 1)
		{
			TCHAR* s, * t;
			for (s = lbsl + 1, t = szExeName; s != ldot; s++)
				*t++ = *s;
			*t = _T('\0');
		}
	}

	return true;
}

/*****************************************************************************/
/* MiniDump : writes out a minidump                                          */
/*****************************************************************************/

void MiniDump(EXCEPTION_POINTERS* ExceptionInfo)
{
	// all variables static so they're preallocated - stack might be damaged!
	// doesn't catch everything, but should get most.
	static TCHAR szPath[_MAX_PATH];
	if (!pMiniDumpWriteDump)
	{
		Log(_T("MiniDump impossible!\n"));
		return;
	}

	static DWORD dwExeLen;
	dwExeLen = (DWORD)_tcslen(szExeName);
	if (!(dwExeLen = GetTempPath(_countof(szPath) - dwExeLen - 21, szPath)) ||
		(dwExeLen > _countof(szPath) - dwExeLen - 22))
	{
		Log(_T("TempPath allocation error 1\n"));
		return;
	}
	static SYSTEMTIME st;
	GetLocalTime(&st);
	_tcscpy(szPath + dwExeLen, szExeName);
	dwExeLen += (DWORD)_tcslen(szExeName);
	wsprintf(szPath + dwExeLen, _T(".%04d%02d%02d-%02d%02d%02d.mdmp"),
		//szExeName,
		st.wYear, st.wMonth, st.wDay,
		st.wHour, st.wMinute, st.wSecond);
	static HANDLE hFile;                    /* create the minidump file          */
	Log(_T("Dumping to %s ...\n"), szPath);
	hFile = ::CreateFile(szPath,
		GENERIC_WRITE, FILE_SHARE_WRITE,
		NULL, CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile != INVALID_HANDLE_VALUE)
	{
		static _MINIDUMP_EXCEPTION_INFORMATION ExInfo = { 0 };
		ExInfo.ThreadId = ::GetCurrentThreadId();
		ExInfo.ExceptionPointers = ExceptionInfo;
		pMiniDumpWriteDump(GetCurrentProcess(),
			GetCurrentProcessId(),
			hFile,
			MiniDumpNormal,
			&ExInfo,
			NULL, NULL);
		::CloseHandle(hFile);
		Log(_T("Dumped to %s\n"), szPath);
	}
	else
		Log(_T("Could not dump to %s!\n"), szPath);
}
#endif
