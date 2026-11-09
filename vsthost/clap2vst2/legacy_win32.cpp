#include "legacy_win32.h"

namespace LegacyWin32
{
std::string pathToUtf8(const tstring &path)
{
    if (path.empty()) return std::string();

#ifdef UNICODE
    return widePathToUtf8(path.c_str());
#else
    int wideCount = MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, NULL, 0);
    if (wideCount <= 0) return std::string(path.c_str());
    std::vector<wchar_t> wideBuffer((size_t)wideCount);
    if (!MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, &wideBuffer[0], wideCount))
        return std::string(path.c_str());
    return widePathToUtf8(&wideBuffer[0]);
#endif
}

std::string widePathToUtf8(const wchar_t *path)
{
    if (!path || !*path) return std::string();
    int bytes = WideCharToMultiByte(CP_UTF8, 0, path, -1, NULL, 0, NULL, NULL);
    if (bytes <= 0) return std::string();

    std::vector<char> utf8((size_t)bytes);
    if (!WideCharToMultiByte(CP_UTF8, 0, path, -1, &utf8[0], bytes, NULL, NULL))
        return std::string();
    return std::string(&utf8[0]);
}
}
