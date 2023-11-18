#pragma once

// Including SDKDDKVer.h defines the highest available Windows platform.

// If you wish to build your application for a previous Windows platform, include WinSDKVer.h and
// set the _WIN32_WINNT macro to the platform you wish to support before including SDKDDKVer.h.

//#include <SDKDDKVer.h>

// Change these values to use different versions
//later ATL versions does not compile with NT4 compatible defines (WINVER 0x0400). For NT4 compatibility use VS2005 and make sure you do not use win2000+ APIs. 

#define WINVER		0x0500  
#define _WIN32_WINNT	0x0400
#define _WIN32_IE	0x0400
#define _RICHEDIT_VER	0x0200
