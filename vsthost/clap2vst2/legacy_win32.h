#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../targetver.h"
#include <tchar.h>
#include "../../external_packages/win32lock.h"
#include <string>
#include <vector>


typedef std::basic_string<TCHAR> tstring;

namespace LegacyWin32
{
    std::string pathToUtf8(const tstring &path);
    std::string widePathToUtf8(const wchar_t *path);
}
