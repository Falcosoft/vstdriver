#pragma once

// Including SDKDDKVer.h defines the highest available Windows platform.

// If you wish to build your application for a previous Windows platform, include WinSDKVer.h and
// set the _WIN32_WINNT macro to the platform you wish to support before including SDKDDKVer.h.

//#include <winsdkver.h>
// Only XP+ is supported by LoopMidi.
#define WINVER 0x0501	
#define _WIN32_WINNT 0x0501
#define _WIN32_IE 0x0500	

//#include <SDKDDKVer.h>
