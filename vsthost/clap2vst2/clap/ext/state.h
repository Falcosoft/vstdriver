#pragma once
#include "../plugin.h"
#include "../stream.h"
static CLAP_CONSTEXPR const char CLAP_EXT_STATE[] = "clap.state";
#ifdef __cplusplus
extern "C" {
#endif
typedef struct clap_plugin_state {
   bool(CLAP_ABI *save)(const clap_plugin_t *plugin, const clap_ostream_t *stream);
   bool(CLAP_ABI *load)(const clap_plugin_t *plugin, const clap_istream_t *stream);
} clap_plugin_state_t;
typedef struct clap_host_state {
   void(CLAP_ABI *mark_dirty)(const clap_host_t *host);
} clap_host_state_t;
#ifdef __cplusplus
}
#endif
