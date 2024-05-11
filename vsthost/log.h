#pragma once

#include "stdafx.h"

// #define LOG
// #define LOG_EXCHANGE
// #define MINIDUMP

#ifdef LOG
void Log(LPCTSTR szFormat, ...);
#endif

#ifdef LOG_EXCHANGE
static unsigned exchange_count = 0;
#endif

#ifdef MINIDUMP
#include <dbghelp.h>
typedef BOOL(__stdcall* PMiniDumpWriteDump)(IN HANDLE hProcess, IN DWORD ProcessId, IN HANDLE hFile, IN MINIDUMP_TYPE DumpType, IN CONST PMINIDUMP_EXCEPTION_INFORMATION ExceptionParam, OPTIONAL IN CONST PMINIDUMP_USER_STREAM_INFORMATION UserStreamParam, OPTIONAL IN CONST PMINIDUMP_CALLBACK_INFORMATION CallbackParam OPTIONAL);
static PMiniDumpWriteDump pMiniDumpWriteDump = NULL;
static TCHAR szExeName[_MAX_PATH] = _T("");

bool LoadMiniDump();
void MiniDump(EXCEPTION_POINTERS* ExceptionInfo);
#endif
