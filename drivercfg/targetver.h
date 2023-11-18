#pragma once

// Including SDKDDKVer.h defines the highest available Windows platform.

// If you wish to build your application for a previous Windows platform, include WinSDKVer.h and
// set the _WIN32_WINNT macro to the platform you wish to support before including SDKDDKVer.h.

//#include <SDKDDKVer.h>

// Change these values to use different versions
// For NT4 compatibility use VS2005 and make sure you do not use win2000+ APIs. 

//#include <winsdkver.h>
#if (defined(_MSC_VER) && (_MSC_VER < 1500)) // VS 2005 can compile NT4 compatible code.
	#define WINVER 0x0400	
	#define _WIN32_WINNT 0x0400
	#define _WIN32_IE 0x0400	
#elif (defined(_MSC_VER) && (_MSC_VER < 1600)) // VS 2005 and 2008 can compile Win2000 compatible code.
	#define WINVER 0x0500	
	#define _WIN32_WINNT 0x0500
	#define _WIN32_IE 0x0500	
#else									// Since VS 2010 only XP is supported.
	#define WINVER 0x0501	
	#define _WIN32_WINNT 0x0501
	#define _WIN32_IE 0x0500	
#endif

#define _RICHEDIT_VER	0x0200
