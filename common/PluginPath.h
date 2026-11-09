#pragma once

#include <tchar.h>

static inline const TCHAR* VSTDriverPluginExtension(const TCHAR* path)
{
    const TCHAR* dot = _tcsrchr(path, _T('.'));
    const TCHAR* backslash = _tcsrchr(path, _T('\\'));
    const TCHAR* slash = _tcsrchr(path, _T('/'));
    return dot && (!backslash || dot > backslash) && (!slash || dot > slash) ? dot : NULL;
}

static inline bool VSTDriverIsClapPluginPath(const TCHAR* path)
{
    const TCHAR* extension = VSTDriverPluginExtension(path);
    return extension && _tcsicmp(extension, _T(".clap")) == 0;
}

static inline bool VSTDriverMakeSettingsPath(TCHAR* path, size_t chars)
{
    size_t length = _tcslen(path);
    const TCHAR* extension = VSTDriverPluginExtension(path);
    // Keep the CLAP extension so Foo.clap and Foo.dll have separate settings.
    if (extension && _tcsicmp(extension, _T(".clap")) != 0)
        length = extension - path;
    const TCHAR suffix[] = _T(".set");
    if (length + _countof(suffix) > chars) return false;
    path[length] = 0;
    return _tcscat_s(path, chars, suffix) == 0;
}
