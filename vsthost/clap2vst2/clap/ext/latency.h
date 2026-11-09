#pragma once
#include "../plugin.h"
static CLAP_CONSTEXPR const char CLAP_EXT_LATENCY[] = "clap.latency";
#ifdef __cplusplus
extern "C" {
#endif
typedef struct clap_plugin_latency {
   uint32_t(CLAP_ABI *get)(const clap_plugin_t *plugin);
} clap_plugin_latency_t;
typedef struct clap_host_latency {
   void(CLAP_ABI *changed)(const clap_host_t *host);
} clap_host_latency_t;
#ifdef __cplusplus
}
#endif
