/* ummidiplg.cpp : User Mode MIDI NSIS Plugin
 *
 * Copyright (C) 2021 Hermann Seib
 * Based on Sergey V. Mikayev's drvsetup in Munt; see
 *   https://github.com/munt/munt
 * for details. So, partially:
 * Copyright (C) 2011-2021 Sergey V. Mikayev
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation, either version 2.1 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

// Require Windows 2K API
#define _WIN32_WINNT 0x0500

#include <windows.h>
#include <Setupapi.h>
#include <Devguid.h>
#include <RegStr.h>

#include <string>
#include <vector>

#include "nsis/pluginapi.h" // nsis plugin

#ifndef _countof
#define _countof(a)  (sizeof(a)/sizeof(a[0]))
#endif

// To work with Unicode version of NSIS, please use TCHAR-type
// functions for accessing the variables and the stack.

HINSTANCE g_hInstance;

// internally used string type - either wide for Unicode build or normal for ANSI build
typedef std::basic_string<TCHAR> tstring;
// little helper to get byte size for a null-terminated string
inline size_t bytesize(tstring s) { return (s.size() + 1) * sizeof(TCHAR); }

enum RegisterDriverResult
  {
  regdrvOK,
  regdrvFailed,
  regdrvExists
  };

enum FindDeviceResult
  {
  fnddevFound,
  fnddevNotFound,
  fnddevFailed
  };

enum ProcessReturnCode
  {
  ProcessReturnCode_OK = 0,
  ProcessReturnCode_ERR_UNRECOGNISED_OPERATION_MODE = 1,
  ProcessReturnCode_ERR_PATH_TOO_LONG = 2,
  ProcessReturnCode_ERR_REGISTERING_DRIVER = 3,
  ProcessReturnCode_ERR_COPYING_FILE = 4,
  ProcessReturnCode_ERR_REGISTERING_DRIVER_CLASS = 5,
  ProcessReturnCode_ERR_REGISTERING_DEVICE = 6,
  ProcessReturnCode_ERR_REMOVING_DEVICE = 7
  };


/*****************************************************************************/
/* isWow64Process : determines whether on 64-bit system                      */
/*****************************************************************************/

typedef WINBASEAPI BOOL (WINAPI *PIsWow64Process)(HANDLE hProcess, PBOOL wow64Process);
static bool isWow64Process()
{
const PIsWow64Process pIsWow64Process =
    (PIsWow64Process)GetProcAddress(GetModuleHandle(_T("kernel32")), "IsWow64Process");
BOOL wow64Process = FALSE;
if (pIsWow64Process &&
    !pIsWow64Process(GetCurrentProcess(), &wow64Process))
  return false;
return !!wow64Process;
}

/*****************************************************************************/
/* getMidiRegEntryName : retrieve one of the possible MIDI reg. entry names  */
/*****************************************************************************/

static tstring getMidiRegEntryName(int index = 0)
{
tstring name(_T("midi"));
if (index > 0 && index < 10)
  name += _T('0') + index;
return name;
}

// Ideally, there should be a free entry to use but WDM entries can also be used when they fill up all available entries.
// Although, the first entry shouldn't be modified. Besides, this is not 100% safe since the WDM entry may be removed
// by the system when the mapped WDM driver is deinstalled. But this is better than nothing in this case.
static bool findFreeMidiRegEntry(tstring driverName, int &entryIx, tstring &orgName, HKEY hReg)
{
int freeEntryIx = -1;
int wdmEntryIx = -1;
TCHAR str[255];
entryIx = -1;
for (int i = 0; i < 10; i++)
  {
  DWORD len = sizeof(str);
  LONG res = RegQueryValueEx(hReg, getMidiRegEntryName(i).c_str(), NULL, NULL, (LPBYTE)str, &len);
  if (res != ERROR_SUCCESS)
    {
    if (res == ERROR_FILE_NOT_FOUND && freeEntryIx < 0)
      freeEntryIx = i;
    continue;
    }
  if (!_tcsicmp(str, driverName.c_str()))
    {
    entryIx = i;
    return false;
    }
  if (freeEntryIx < 0)
    {
    if (!*str)
      {
      freeEntryIx = i;
      continue;
      }
    if (i > 0 && wdmEntryIx == -1 && !_tcsicmp(str, _T("wdmaud.drv")))
      wdmEntryIx = i;
    }
  }
// Fall back to using a WDM entry if there is no free one.
entryIx = freeEntryIx != -1 ? freeEntryIx : wdmEntryIx;
if (entryIx != -1)
  {
  DWORD len = sizeof(str);
  RegQueryValueEx(hReg, getMidiRegEntryName(entryIx).c_str(), NULL, NULL, (LPBYTE)str, &len);
  orgName = str;
  }
return entryIx != -1;
}

/*****************************************************************************/
/* findDriverRegEntry : find driver index in registry, if registered         */
/*****************************************************************************/

static int findDriverRegEntry(tstring driverName, HKEY hReg)
{
int entryIx;
tstring orgName;  // not needed outside
return findFreeMidiRegEntry(driverName, entryIx, orgName, hReg) ? -1 : entryIx;
}

/*****************************************************************************/
/* registerDriverInWow : register 32bit driver in 64bit system               */
/*****************************************************************************/

static bool registerDriverInWow(tstring driverName, tstring driverEntryName)
{
HKEY hReg;
if (RegOpenKeyEx(HKEY_LOCAL_MACHINE,
                 _T("SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Drivers32"), 0L,
                 KEY_ALL_ACCESS | KEY_WOW64_32KEY, &hReg))
    return false;
LSTATUS res = RegSetValueEx(hReg, driverEntryName.c_str(), NULL, REG_SZ,
                            (const BYTE *)driverName.c_str(),
                            bytesize(driverName));
RegCloseKey(hReg);
return res == ERROR_SUCCESS;
}

/*****************************************************************************/
/* registerDriver : register user mode MIDI driver in registry               */
/*****************************************************************************/

static RegisterDriverResult registerDriver
    (
    tstring driverName /*=_T("mt32emu.dll")*/,
    tstring driverSubdir /*=T("")*/,
    const bool wow64Process,
    std::vector<tstring> &regs
    )
{
regs.clear();
for (int i = 0; i < 2; i++)
  regs.push_back(_T(""));

HKEY hReg;
int entryIx;
if (RegOpenKeyEx(HKEY_LOCAL_MACHINE,
                 _T("SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Drivers32"),
                 0L, (wow64Process ? (KEY_ALL_ACCESS | KEY_WOW64_64KEY) : KEY_ALL_ACCESS), &hReg))
  return regdrvFailed;
tstring driverSubdirName(driverSubdir);
if (driverSubdirName.size())
  driverSubdirName += _T("\\");
driverSubdirName += driverName;
tstring orgname;
if (!findFreeMidiRegEntry(driverSubdirName, entryIx, orgname, hReg))
  {
  RegCloseKey(hReg);
  if (entryIx == -1)
    return regdrvFailed;
  // OK, so it's already there.
#if 0 // don't think that's necessary
  if (wow64Process && !registerDriverInWow(driverSubdirName, getMidiRegEntryName(entryIx)))
    return regdrvFailed;
#endif
  return regdrvExists;
  }

tstring freeEntryName = getMidiRegEntryName(entryIx);
LONG res = RegSetValueEx(hReg, freeEntryName.c_str(), NULL, REG_SZ,
                         (const BYTE *)driverSubdirName.c_str(),
                         bytesize(driverSubdirName));
RegCloseKey(hReg);
if (res != ERROR_SUCCESS)
  return regdrvFailed;
if (wow64Process && !registerDriverInWow(driverSubdirName, freeEntryName))
  return regdrvFailed;

TCHAR sIdx[20];
_itot(entryIx, sIdx, 10);
regs[0] = sIdx;
regs[1] = orgname;
return regdrvOK;
}

/*****************************************************************************/
/* unregisterDriverInWow : unregister 32bit driver in 64bit system           */
/*****************************************************************************/

static bool unregisterDriverInWow
    (
    tstring driverName,
    tstring driverEntryName
    )
{
HKEY hReg;
if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, 
                 _T("SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Drivers32"),
                 0L, KEY_ALL_ACCESS | KEY_WOW64_32KEY, &hReg))
  return false;
TCHAR str[255];
DWORD len = sizeof(str);
LONG res = RegQueryValueEx(hReg, driverEntryName.c_str(), NULL, NULL, (LPBYTE)str, &len);
if (res != ERROR_SUCCESS || _tcsicmp(str, driverName.c_str()))
  {
  RegCloseKey(hReg);
  return false;
  }
res = RegDeleteValue(hReg, driverEntryName.c_str());
RegCloseKey(hReg);
return res == ERROR_SUCCESS;
}

/*****************************************************************************/
/* unregisterDriver : remov user mode MIDI driver from registry              */
/*****************************************************************************/

static bool unregisterDriver
    (
    tstring driverName,
    const bool wow64Process
    )
{
HKEY hReg;
if (RegOpenKeyEx(HKEY_LOCAL_MACHINE,
                 _T("SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Drivers32"),
                 0L, (wow64Process ? (KEY_ALL_ACCESS | KEY_WOW64_64KEY) : KEY_ALL_ACCESS),
                 &hReg) != ERROR_SUCCESS)
  return false;
const int entryIx = findDriverRegEntry(driverName, hReg);
if (entryIx == -1)
  {
  RegCloseKey(hReg);
  return false;
  }
tstring driverEntryName = getMidiRegEntryName(entryIx);
LONG res = RegDeleteValue(hReg, driverEntryName.c_str());
RegCloseKey(hReg);
if (wow64Process)
  unregisterDriverInWow(driverName, driverEntryName);
return res == ERROR_SUCCESS;
}

/*****************************************************************************/
/* getSystemRoot : retrieve system root (normally C:\Windows)                */
/*****************************************************************************/

static tstring getSystemRoot()
{
TCHAR sysRoot[MAX_PATH + 1];
sysRoot[0] = _T('\0');
GetEnvironmentVariable(_T("SystemRoot"), sysRoot, _countof(sysRoot));
return sysRoot;
}

/*****************************************************************************/
/* constructFullSystemDirName : create full system directory name            */
/*****************************************************************************/

static tstring constructFullSystemDirName(tstring systemDirName)
{
tstring sysRoot = getSystemRoot();
tstring pathName(sysRoot);
pathName += _T('\\');
pathName += systemDirName;
pathName += _T('\\');
return pathName;
}

/*****************************************************************************/
/* registerDriverClass : registers driver class (Win10 >= 2004 requirement)  */
/*****************************************************************************/

static bool registerDriverClass
    (
    tstring driverName /*=_T("mt32emu.dll")*/,
    tstring driverSubdir /*=T("")*/,
    tstring deviceDesc /*= _T("MT-32 Synth Emulator")*/,
    tstring providerName /*= _T("muntemu.org")*/,
    HKEY devRegKey,
    int legacyMidiEntryIx
    )
{
if (RegSetValueEx(devRegKey, _T("DriverDesc"), NULL, REG_SZ,
                  (const BYTE *)deviceDesc.c_str(),
                  bytesize(deviceDesc)) != ERROR_SUCCESS)
  return false;
if (RegSetValueEx(devRegKey, _T("ProviderName"), NULL, REG_SZ,
                  (const BYTE *)providerName.c_str(),
                  bytesize(providerName)) != ERROR_SUCCESS)
  return false;
HKEY driversSubkey;
if (RegCreateKeyEx(devRegKey, _T("Drivers"), NULL, NULL, 0, KEY_ALL_ACCESS,
                   NULL, &driversSubkey, NULL) != ERROR_SUCCESS)
  return false;
tstring driverSubclass(_T("MIDI"));
if (RegSetValueEx(driversSubkey, _T("SubClasses"), NULL, REG_SZ,
                  (const BYTE *)driverSubclass.c_str(),
                  bytesize(driverSubclass)) != ERROR_SUCCESS)
  {
  RegCloseKey(driversSubkey);
  return false;
  }
HKEY driverSubkey;
tstring subclassSubkeys = driverSubclass + _T('\\') + driverName;
if (RegCreateKeyEx(driversSubkey, subclassSubkeys.c_str(), NULL, NULL, 0,
               KEY_ALL_ACCESS, NULL, &driverSubkey, NULL) != ERROR_SUCCESS)
  {
  RegCloseKey(driversSubkey);
  return false;
  }
RegCloseKey(driversSubkey);
tstring driverSubdirName(driverSubdir);
if (driverSubdir.size())
  driverSubdirName += _T("\\");
driverSubdirName += driverName;
if (RegSetValueEx(driverSubkey, _T("Driver"), NULL, REG_SZ,
                  (const BYTE *)driverSubdirName.c_str(),
                  bytesize(driverSubdirName)) != ERROR_SUCCESS)
  {
  RegCloseKey(driverSubkey);
  return false;
  }
if (RegSetValueEx(driverSubkey, _T("Description"), NULL, REG_SZ,
                  (const BYTE *)deviceDesc.c_str(),
                  bytesize(deviceDesc)) != ERROR_SUCCESS)
  {
  RegCloseKey(driverSubkey);
  return false;
  }
if (legacyMidiEntryIx >= 0)
  {
  tstring driverEntryName = getMidiRegEntryName(legacyMidiEntryIx);
  if (RegSetValueEx(driverSubkey, _T("Alias"), NULL, REG_SZ,
                    (const BYTE *)driverEntryName.c_str(),
                    bytesize(driverEntryName)) != ERROR_SUCCESS)
    {
    RegCloseKey(driverSubkey);
    return false;
    }
  }
return true;
}

/*****************************************************************************/
/* findDriverDevice : find driver device                                     */
/*****************************************************************************/

static FindDeviceResult findDriverDevice
    (
    tstring devHwIds /*=_T("ROOT\\mt32emu")*/,
    tstring deviceDesc /*=_T("MT-32 Synth Emulator")*/,
    HDEVINFO &hDevInfo,
    SP_DEVINFO_DATA &deviceInfoData
    )
{
TCHAR prop[1024];
for (int deviceIx = 0; SetupDiEnumDeviceInfo(hDevInfo, deviceIx, &deviceInfoData); ++deviceIx)
  {
  if (SetupDiGetDeviceRegistryProperty(hDevInfo, &deviceInfoData, SPDRP_HARDWAREID, NULL,
                                       (BYTE *)prop, sizeof(prop), NULL))
    {
    if (!_tcsnicmp(prop, devHwIds.c_str(), devHwIds.size() + 1))
      return fnddevFound;
    continue;
    }
  if (GetLastError() == ERROR_INVALID_DATA)
    {
    if (SetupDiGetDeviceRegistryProperty(hDevInfo, &deviceInfoData, SPDRP_DEVICEDESC, NULL,
                                         (BYTE *)prop, sizeof(prop), NULL))
      {
      if (_tcsnicmp(prop, deviceDesc.c_str(), deviceDesc.size() + 1))
        continue;
      SetupDiSetDeviceRegistryProperty(hDevInfo, &deviceInfoData, SPDRP_HARDWAREID,
                                       (BYTE *)devHwIds.c_str(), bytesize(devHwIds));
      return fnddevFound;
      }
    }
  }
return GetLastError() == ERROR_NO_MORE_ITEMS ? fnddevNotFound : fnddevFailed;
}

/*****************************************************************************/
/* registerDevice : registers the device                                     */
/*****************************************************************************/

static ProcessReturnCode registerDevice
    (
    tstring devHwIds /*=_T("ROOT\\mt32emu")*/,
    tstring deviceDesc /*=_T("MT-32 Synth Emulator")*/,
    HDEVINFO &hDevInfo,
    SP_DEVINFO_DATA &deviceInfoData,
    bool wow64Process
    )
{
hDevInfo = SetupDiGetClassDevs(&GUID_DEVCLASS_MEDIA, NULL, NULL, 0);
if (hDevInfo == INVALID_HANDLE_VALUE)
  return ProcessReturnCode_ERR_REGISTERING_DEVICE;

FindDeviceResult findDeviceResult = findDriverDevice(devHwIds, deviceDesc, hDevInfo, deviceInfoData);
if (findDeviceResult == fnddevFailed)
  return ProcessReturnCode_ERR_REGISTERING_DEVICE;
if (findDeviceResult == fnddevFound)
  return ProcessReturnCode_OK;

if (!SetupDiCreateDeviceInfo(hDevInfo, _T("MEDIA"), &GUID_DEVCLASS_MEDIA, deviceDesc.c_str(),
                             NULL, DICD_GENERATE_ID, &deviceInfoData))
  {
  SetupDiDestroyDeviceInfoList(hDevInfo);
  return ProcessReturnCode_ERR_REGISTERING_DEVICE;
  }
if (!SetupDiSetDeviceRegistryProperty(hDevInfo, &deviceInfoData, SPDRP_HARDWAREID,
                                      (BYTE *)devHwIds.c_str(), bytesize(devHwIds)))
  {
  SetupDiDestroyDeviceInfoList(hDevInfo);
  return ProcessReturnCode_ERR_REGISTERING_DEVICE;
  }

// The proper way to register device in PnP manager is to call SetupDiCallClassInstaller but that doesn't work in WOW.
BOOL deviceRegistrationResult;
if (wow64Process)
  deviceRegistrationResult = SetupDiRegisterDeviceInfo(hDevInfo, &deviceInfoData, 0, NULL, NULL, NULL);
else
  deviceRegistrationResult = SetupDiCallClassInstaller(DIF_REGISTERDEVICE, hDevInfo, &deviceInfoData);
if (!deviceRegistrationResult)
  {
  SetupDiDestroyDeviceInfoList(hDevInfo);
  return ProcessReturnCode_ERR_REGISTERING_DEVICE;
  }
return ProcessReturnCode_OK;
}

/*****************************************************************************/
/* registerDeviceAndDriverClass                                              */
/*****************************************************************************/

static ProcessReturnCode registerDeviceAndDriverClass
    (
    tstring driverName /* =_T("mt32emu.dll")*/,
    tstring driverSubdir /*=T("")*/,
    tstring deviceDesc /* = "_T("MT-32 Synth Emulator") */,
    tstring providerName /* = _T("muntemu.org") */,
    tstring devHwIds /*=_T("ROOT\\mt32emu")*/,
    bool wow64Process,
    int legacyMidiEntryIx
    )
{
HDEVINFO hDevInfo;
SP_DEVINFO_DATA deviceInfoData;
deviceInfoData.cbSize = sizeof(SP_DEVINFO_DATA);

ProcessReturnCode returnCode = registerDevice(devHwIds, deviceDesc, hDevInfo,
                                              deviceInfoData, wow64Process);
if (returnCode != ProcessReturnCode_OK)
  return returnCode;

DWORD configClass = CONFIGFLAG_MANUAL_INSTALL | CONFIGFLAG_NEEDS_FORCED_CONFIG;
if (!SetupDiSetDeviceRegistryProperty(hDevInfo, &deviceInfoData, SPDRP_CONFIGFLAGS,
                                      (BYTE *)&configClass, sizeof(configClass)))
  {
  SetupDiDestroyDeviceInfoList(hDevInfo);
  return ProcessReturnCode_ERR_REGISTERING_DEVICE;
  }
if (!SetupDiSetDeviceRegistryProperty(hDevInfo, &deviceInfoData, SPDRP_MFG,
                                      (BYTE *)providerName.c_str(), bytesize(providerName)))
  {
  SetupDiDestroyDeviceInfoList(hDevInfo);
  return ProcessReturnCode_ERR_REGISTERING_DEVICE;
  }

HKEY devRegKey = SetupDiCreateDevRegKey(hDevInfo, &deviceInfoData, DICS_FLAG_GLOBAL,
                                        0, DIREG_DRV, NULL, NULL);
SetupDiDestroyDeviceInfoList(hDevInfo);
if (devRegKey == INVALID_HANDLE_VALUE)
  return ProcessReturnCode_ERR_REGISTERING_DRIVER_CLASS;
if (!registerDriverClass(driverName, driverSubdir, deviceDesc, providerName, 
                         devRegKey, legacyMidiEntryIx))
  {
  RegCloseKey(devRegKey);
  return ProcessReturnCode_ERR_REGISTERING_DRIVER_CLASS;
  }
RegCloseKey(devRegKey);

return ProcessReturnCode_OK;
}

/*****************************************************************************/
/* removeDevice                                                              */
/*****************************************************************************/

static ProcessReturnCode removeDevice
    (
    tstring devHwIds /*=_T("ROOT\\mt32emu")*/,
    tstring deviceDesc /*=_T("MT-32 Synth Emulator")*/,
    bool wow64Process
    )
{
HDEVINFO hDevInfo;
SP_DEVINFO_DATA deviceInfoData;
deviceInfoData.cbSize = sizeof(SP_DEVINFO_DATA);

hDevInfo = SetupDiGetClassDevs(&GUID_DEVCLASS_MEDIA, NULL, NULL, 0);
if (hDevInfo == INVALID_HANDLE_VALUE)
  return ProcessReturnCode_ERR_REMOVING_DEVICE;

FindDeviceResult findDeviceResult = findDriverDevice(devHwIds, deviceDesc,
                                                     hDevInfo, deviceInfoData);
if (findDeviceResult == fnddevFailed)
  {
  SetupDiDestroyDeviceInfoList(hDevInfo);
  return ProcessReturnCode_ERR_REMOVING_DEVICE;
  }
else if (findDeviceResult == fnddevNotFound)
  {
  SetupDiDestroyDeviceInfoList(hDevInfo);
  return ProcessReturnCode_OK;
  }

// The proper way to remove device in PnP manager is to call SetupDiCallClassInstaller but that doesn't work in WOW.
BOOL deviceRemovalResult;
if (wow64Process)
  deviceRemovalResult = SetupDiRemoveDevice(hDevInfo, &deviceInfoData);
else
  deviceRemovalResult = SetupDiCallClassInstaller(DIF_REMOVE, hDevInfo, &deviceInfoData);
SetupDiDestroyDeviceInfoList(hDevInfo);
return ProcessReturnCode_OK;
}

/*****************************************************************************/
/* popParms : generalized parameter popper                                   */
/*****************************************************************************/

static int popParms(int nParms, /*out*/ std::vector<tstring> &strings)
{
// ATTENTION: EXDLL_INIT(); MUST have been done before calling this!

TCHAR buf[1024];

for (; nParms > 0; nParms--)
  if (!popstringn(buf, _countof(buf)))
    strings.push_back(buf);

return (int)strings.size();
}

/*****************************************************************************/
/* pushRets : generalized parameter pusher                                   */
/*****************************************************************************/

static int pushRets(/*in*/ std::vector<tstring> &strings)
{
// ATTENTION: EXDLL_INIT(); MUST have been done before calling this!

// strings need to be pushed in reversed order
for (int nRet = (int)strings.size() - 1; nRet >= 0; nRet--)
  pushstring(strings[nRet].c_str());

return (int)strings.size();
}

/*****************************************************************************/
/* SetupRegistry : installs a new user-mode MIDI driver in the registry      */
/*****************************************************************************/
/*
   NSIS Syntax:
     UMMidiPlg::SetupRegistry driverName deviceDesc providerName dwvHwIds [driverSubdir]
     pop $returnmsg
     pop $orgdrv
     pop $orgnum
   returns "OK" or an error message

   Example: UMMidiPlg::SetupRegistry "mt32emu.dll" "MT-32 Synth Emulator" "muntemu.org" "ROOT\\mt32emu"


   Note: this function relies on the NSIS installer to put the files into the correct
         subdirectories; only the registry setup is done here.
*/

extern "C"
void __declspec(dllexport) SetupRegistry
    (
    HWND hwndParent,
    int string_size,
    TCHAR *variables,
    stack_t **stacktop,
    extra_parameters *extra
    )
{
EXDLL_INIT();

std::vector<tstring> vars;
int nParms = popParms(5, vars);
if (nParms < 4)
  {
  pushstring(_T("Error: not enough parameters!"));
  return;
  }
if (nParms < 5)                         /* optional subdirectory parameter   */
  vars.push_back(_T(""));

/* vars[0] = driverName
 * vars[1] = deviceDesc
 * vars[2] = providerName
 * vars[3] = devHwIds
 * vars[4] = driverSubdir
 */

bool wow64Process = isWow64Process();

std::vector<tstring> rets;

#if 0 // TEST TEST TEST TEST TEST EST TEST
rets.push_back(_T("-1"));
rets.push_back(_T(""));
tstring t(_T("SetupRegistry("));
for (int i = 0; i < (int)vars.size(); i++)
  {
  if (i)
    t += _T(", ");
  t += vars[i];
  }
t += _T(")");
rets.insert(rets.begin(), t);
pushRets(rets);
return;
#endif

int legacyMidiEntryIx;
const RegisterDriverResult res = registerDriver(vars[0], vars[4],
                                                wow64Process, rets);
if (res == regdrvFailed)
  {
  pushstring(_T("Error: driver could not be registered!"));
  return;
  }
ProcessReturnCode returnCode = registerDeviceAndDriverClass(vars[0], vars[4], vars[1], vars[2], vars[3],
                                                            wow64Process, legacyMidiEntryIx);
if (returnCode != ProcessReturnCode_OK)
  {
  pushstring(_T("Error: driver could not be registered!"));
  return;
  }

rets.insert(rets.begin(), _T("OK"));
pushRets(rets);
}

/*****************************************************************************/
/* CleanupRegistry : removes an installed user-mode MIDI driver from registry*/
/*****************************************************************************/
/*
   NSIS Syntax:
     UMMidiPlg::CleanupRegistry driverName devHwIds deviceDesc [driverSubdir]
     Pop $returnmsg
   returns "OK" or an error message

   Example: UMMidiPlg::CleanupRegistry "mt32emu.dll" "MT-32 Synth Emulator" "ROOT\\mt32emu"

   Note: this function relies on the NSIS installer to remove the files from the correct
         subdirectories; only the registry cleanup is done here.

*/

extern "C"
void __declspec(dllexport) CleanupRegistry
    (
    HWND hwndParent,
    int string_size,
    TCHAR *variables,
    stack_t **stacktop,
    extra_parameters *extra
    )
{
EXDLL_INIT();

std::vector<tstring> vars;
int nParms = popParms(4, vars);
if (nParms < 3)
  {
  pushstring(_T("Error: not enough parameters!"));
  return;
  }
if (nParms < 4)                         /* optional subdirectory parameter   */
  vars.push_back(_T(""));

/* vars[0] = driverName
 * vars[1] = deviceDesc
 * vars[2] = devHwIds
 * vars[3] = driverSubdir
 */

#if 0 // TEST TEST TEST TEST TEST EST TEST
tstring t(_T("CleanupRegistry("));
for (int i = 0; i < (int)vars.size(); i++)
  {
  if (i)
    t += _T(", ");
  t += vars[i];
  }
t += _T(")");
pushstring(t.c_str());
return;
#endif

bool wow64Process = isWow64Process();
if (!unregisterDriver(vars[0], wow64Process))
  {
  pushstring(_T("OK"));
  return;
  }

ProcessReturnCode returnCode = removeDevice(vars[2], vars[1], wow64Process);
if (returnCode != ProcessReturnCode_OK)
  {
  pushstring(_T("Error: driver could not be unregistered!"));
  return;
  }

pushstring(_T("OK"));
}

/*****************************************************************************/
/* DllMain : minimal DLL main function                                       */
/*****************************************************************************/

extern "C"
BOOL WINAPI DllMain(HINSTANCE hInst, ULONG ul_reason_for_call, LPVOID lpReserved)
{
g_hInstance = hInst;
return TRUE;
}
