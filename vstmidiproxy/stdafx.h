// header.h : include file for standard system include files,
// or project specific include files
//

#pragma once

#define PSAPI_VERSION 1
#include "targetver.h"
#define WIN32_LEAN_AND_MEAN             // Exclude rarely-used stuff from Windows headers
// Windows Header Files
#include <windows.h>
#include <mmsystem.h>
#include <commdlg.h>
#include <ShellAPI.h>
#include <CommCtrl.h>
#include <Psapi.h>
#include <mmreg.h>
// C RunTime Header Files
#include <stdlib.h>
#include <malloc.h>
#include <memory.h>
#include <tchar.h>
#include "../external_packages/win32lock.h"


