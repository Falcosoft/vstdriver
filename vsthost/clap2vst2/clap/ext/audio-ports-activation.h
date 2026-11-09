#pragma once
#include "../plugin.h"
static CLAP_CONSTEXPR const char CLAP_EXT_AUDIO_PORTS_ACTIVATION[] =
   "clap.audio-ports-activation/2";
static CLAP_CONSTEXPR const char CLAP_EXT_AUDIO_PORTS_ACTIVATION_COMPAT[] =
   "clap.audio-ports-activation/draft-2";
#ifdef __cplusplus
extern "C" {
#endif
typedef struct clap_plugin_audio_ports_activation {
   bool(CLAP_ABI *can_activate_while_processing)(const clap_plugin_t *plugin);
   bool(CLAP_ABI *set_active)(const clap_plugin_t *plugin, bool is_input,
                              uint32_t port_index, bool is_active, uint32_t sample_size);
} clap_plugin_audio_ports_activation_t;
#ifdef __cplusplus
}
#endif
