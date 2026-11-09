#pragma once

#include "../plugin.h"

static const CLAP_CONSTEXPR char CLAP_PLUGIN_FACTORY_ID[] = "clap.plugin-factory";

#ifdef __cplusplus
extern "C" {
#endif

typedef struct clap_plugin_factory {
   uint32_t(CLAP_ABI *get_plugin_count)(const struct clap_plugin_factory *factory);
   const clap_plugin_descriptor_t *(CLAP_ABI *get_plugin_descriptor)(
      const struct clap_plugin_factory *factory, uint32_t index);
   const clap_plugin_t *(CLAP_ABI *create_plugin)(const struct clap_plugin_factory *factory,
                                                  const clap_host_t *host,
                                                  const char *plugin_id);
} clap_plugin_factory_t;

#ifdef __cplusplus
}
#endif
