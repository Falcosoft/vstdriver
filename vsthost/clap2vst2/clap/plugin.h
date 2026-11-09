#pragma once

#include "private/macros.h"
#include "host.h"
#include "process.h"
#include "plugin-features.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct clap_plugin_descriptor {
   clap_version_t clap_version;
   const char *id;
   const char *name;
   const char *vendor;
   const char *url;
   const char *manual_url;
   const char *support_url;
   const char *version;
   const char *description;
   const char *const *features;
} clap_plugin_descriptor_t;

typedef struct clap_plugin {
   const clap_plugin_descriptor_t *desc;
   void *plugin_data;
   bool(CLAP_ABI *init)(const struct clap_plugin *plugin);
   void(CLAP_ABI *destroy)(const struct clap_plugin *plugin);
   bool(CLAP_ABI *activate)(const struct clap_plugin *plugin,
                            double sample_rate,
                            uint32_t min_frames_count,
                            uint32_t max_frames_count);
   void(CLAP_ABI *deactivate)(const struct clap_plugin *plugin);
   bool(CLAP_ABI *start_processing)(const struct clap_plugin *plugin);
   void(CLAP_ABI *stop_processing)(const struct clap_plugin *plugin);
   void(CLAP_ABI *reset)(const struct clap_plugin *plugin);
   clap_process_status(CLAP_ABI *process)(const struct clap_plugin *plugin,
                                          const clap_process_t *process);
   const void *(CLAP_ABI *get_extension)(const struct clap_plugin *plugin, const char *id);
   void(CLAP_ABI *on_main_thread)(const struct clap_plugin *plugin);
} clap_plugin_t;

#ifdef __cplusplus
}
#endif
