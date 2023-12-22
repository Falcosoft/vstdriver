#pragma once

// Including SDKDDKVer.h defines the highest available Windows platform.

// If you wish to build your application for a previous Windows platform, include WinSDKVer.h and
// set the _WIN32_WINNT macro to the platform you wish to support before including SDKDDKVer.h.

//#include <winsdkver.h>
#if (defined(_MSC_VER) && (_MSC_VER < 1500)) // VS 2005 can compile NT4 compatible code.
	#define WINVER 0x0400	
	#define _WIN32_WINNT 0x0400
	#define _WIN32_IE 0x0400	
	#define WINBASE_DECLARE_GET_MODULE_HANDLE_EX
#elif (defined(_MSC_VER) && (_MSC_VER < 1600)) // VS 2005 and 2008 can compile Win2000 compatible code.
	#define WINVER 0x0500	
	#define _WIN32_WINNT 0x0500
	#define _WIN32_IE 0x0500	
	#define WINBASE_DECLARE_GET_MODULE_HANDLE_EX
#else									// Since VS 2010 only XP is supported.
	#define WINVER 0x0501	
	#define _WIN32_WINNT 0x0501
	#define _WIN32_IE 0x0500	
#endif

//#include <SDKDDKVer.h>
