#pragma once
#define VERSION_MAJOR 2
#define VERSION_MINOR 5
#define VERSION_PATCH 0
#define VERSION_BUILD 1

#define stringify(a) stringify_(a)
#define stringify_(a) #a

#pragma warning(disable:4996)
#pragma warning(disable:28159)

// {F7A1C2D2-90A5-45ED-871B-1AE799CF6968}
const GUID  VSTMidiDrvManufacturerGuid =
{ 0xf7a1c2d2, 0x90a5, 0x45ed, { 0x87, 0x1b, 0x1a, 0xe7, 0x99, 0xcf, 0x69, 0x68 } };

// {C1CAF763-36F0-41F5-B094-685FF9682717}
const GUID VSTMidiDrvPortAGuid =
{ 0xc1caf763, 0x36f0, 0x41f5, { 0xb0, 0x94, 0x68, 0x5f, 0xf9, 0x68, 0x27, 0x17 } };

// {78E9B96D-A2F7-439E-8DC5-6CAB06B81F23}
const GUID VSTMidiDrvPortBGuid =
{ 0x78e9b96d, 0xa2f7, 0x439e, { 0x8d, 0xc5, 0x6c, 0xab, 0x6, 0xb8, 0x1f, 0x23 } };

inline TCHAR* GetFileVersion(TCHAR* result)
{
	_tcscat(result, _T("version: ") _T(stringify(VERSION_MAJOR)) _T(".") _T(stringify(VERSION_MINOR)) _T(".") _T(stringify(VERSION_PATCH)));
	return result;
}

inline bool IsWinNT4()
{
	return (GetVersion() & 0x8000000F) == 4;
}



