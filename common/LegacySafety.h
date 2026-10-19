// Shared bounds for the legacy driver, bridge and configuration program.
#ifndef VSTDRIVER_LEGACY_SAFETY_H
#define VSTDRIVER_LEGACY_SAFETY_H

#include <windows.h>
#include <tchar.h>
#include <string.h>
#include <vector>
#include <new>
#include <stdexcept>

// Applies to serialized .set files/chunks, not plugin sample memory.
static const unsigned VSTDriverMaxChunkBytes = 256U * 1024U * 1024U;
static const unsigned VSTDriverMaxSysExBytes = 0x00FFFFFFU;
static const unsigned VSTDriverInvalidChunk = 13;

static bool VSTDriverReadRegistryString(HKEY key, LPCTSTR name, TCHAR* out, DWORD chars)
{
    if (!out || !chars || chars > MAXDWORD / sizeof(TCHAR)) return false;
    memset(out, 0, chars * sizeof(TCHAR));
    DWORD bytes = chars * sizeof(TCHAR), type = 0;
    LONG result = RegQueryValueEx(key, name, NULL, &type, (LPBYTE)out, &bytes);
    if (result != ERROR_SUCCESS || type != REG_SZ || bytes > chars * sizeof(TCHAR) ||
        bytes % sizeof(TCHAR) || (bytes == chars * sizeof(TCHAR) && out[chars - 1])) {
        out[0] = 0;
        return false;
    }
    // Space was reserved by the byte count, or the final character was checked.
    if (bytes < chars * sizeof(TCHAR)) out[bytes / sizeof(TCHAR)] = 0;
    return true;
}

template<class T>
static bool VSTDriverResizeChunk(std::vector<T>& out, size_t bytes)
{
    if (bytes > VSTDriverMaxChunkBytes || bytes > out.max_size()) return false;
    try { out.resize(bytes); }
    catch (const std::bad_alloc&) { return false; }
    catch (const std::length_error&) { return false; }
    return true;
}

#endif
