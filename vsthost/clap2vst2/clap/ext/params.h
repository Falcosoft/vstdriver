#pragma once
#include "../plugin.h"
#include "../string-sizes.h"
static CLAP_CONSTEXPR const char CLAP_EXT_PARAMS[] = "clap.params";
#ifdef __cplusplus
extern "C" {
#endif
enum {
   CLAP_PARAM_IS_STEPPED = 1 << 0,
   CLAP_PARAM_IS_PERIODIC = 1 << 1,
   CLAP_PARAM_IS_HIDDEN = 1 << 2,
   CLAP_PARAM_IS_READONLY = 1 << 3,
   CLAP_PARAM_IS_BYPASS = 1 << 4,
   CLAP_PARAM_IS_AUTOMATABLE = 1 << 5,
   CLAP_PARAM_IS_AUTOMATABLE_PER_NOTE_ID = 1 << 6,
   CLAP_PARAM_IS_AUTOMATABLE_PER_KEY = 1 << 7,
   CLAP_PARAM_IS_AUTOMATABLE_PER_CHANNEL = 1 << 8,
   CLAP_PARAM_IS_AUTOMATABLE_PER_PORT = 1 << 9,
   CLAP_PARAM_IS_MODULATABLE = 1 << 10,
   CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID = 1 << 11,
   CLAP_PARAM_IS_MODULATABLE_PER_KEY = 1 << 12,
   CLAP_PARAM_IS_MODULATABLE_PER_CHANNEL = 1 << 13,
   CLAP_PARAM_IS_MODULATABLE_PER_PORT = 1 << 14,
   CLAP_PARAM_REQUIRES_PROCESS = 1 << 15,
   CLAP_PARAM_IS_ENUM = 1 << 16,
};
typedef uint32_t clap_param_info_flags;
typedef struct clap_param_info {
   clap_id id;
   clap_param_info_flags flags;
   void *cookie;
   char name[CLAP_NAME_SIZE];
   char module[CLAP_PATH_SIZE];
   double min_value;
   double max_value;
   double default_value;
} clap_param_info_t;
typedef struct clap_plugin_params {
   uint32_t(CLAP_ABI *count)(const clap_plugin_t *plugin);
   bool(CLAP_ABI *get_info)(const clap_plugin_t *plugin, uint32_t param_index,
                            clap_param_info_t *param_info);
   bool(CLAP_ABI *get_value)(const clap_plugin_t *plugin, clap_id param_id, double *out_value);
   bool(CLAP_ABI *value_to_text)(const clap_plugin_t *plugin, clap_id param_id, double value,
                                 char *out_buffer, uint32_t out_buffer_capacity);
   bool(CLAP_ABI *text_to_value)(const clap_plugin_t *plugin, clap_id param_id,
                                 const char *param_value_text, double *out_value);
   void(CLAP_ABI *flush)(const clap_plugin_t *plugin, const clap_input_events_t *in,
                         const clap_output_events_t *out);
} clap_plugin_params_t;
enum {
   CLAP_PARAM_RESCAN_VALUES = 1 << 0,
   CLAP_PARAM_RESCAN_TEXT = 1 << 1,
   CLAP_PARAM_RESCAN_INFO = 1 << 2,
   CLAP_PARAM_RESCAN_ALL = 1 << 3,
};
typedef uint32_t clap_param_rescan_flags;
enum {
   CLAP_PARAM_CLEAR_ALL = 1 << 0,
   CLAP_PARAM_CLEAR_AUTOMATIONS = 1 << 1,
   CLAP_PARAM_CLEAR_MODULATIONS = 1 << 2,
};
typedef uint32_t clap_param_clear_flags;
typedef struct clap_host_params {
   void(CLAP_ABI *rescan)(const clap_host_t *host, clap_param_rescan_flags flags);
   void(CLAP_ABI *clear)(const clap_host_t *host, clap_id param_id, clap_param_clear_flags flags);
   void(CLAP_ABI *request_flush)(const clap_host_t *host);
} clap_host_params_t;
#ifdef __cplusplus
}
#endif
