#pragma once

#include "version.h"
#include "private/macros.h"

#ifdef __cplusplus
extern "C" {
#endif

// This interface is the entry point of the dynamic library.
//
// CLAP plugins standard search path:
// Windows
//   - %COMMONPROGRAMFILES%\CLAP
//   - %LOCALAPPDATA%\Programs\Common\CLAP
//
// In addition to the OS-specific default locations above, a CLAP host must query the environment
// for a CLAP_PATH variable, which is a list of directories separated by ';' on Windows.
typedef struct clap_plugin_entry {
   clap_version_t clap_version;
   bool(CLAP_ABI *init)(const char *plugin_path);
   void(CLAP_ABI *deinit)(void);
   const void *(CLAP_ABI *get_factory)(const char *factory_id);
} clap_plugin_entry_t;

CLAP_EXPORT extern const clap_plugin_entry_t clap_entry;

#ifdef __cplusplus
}
#endif
