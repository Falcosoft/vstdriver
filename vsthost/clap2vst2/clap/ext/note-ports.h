#pragma once
#include "../plugin.h"
#include "../string-sizes.h"
static CLAP_CONSTEXPR const char CLAP_EXT_NOTE_PORTS[] = "clap.note-ports";
#ifdef __cplusplus
extern "C" {
#endif
enum clap_note_dialect {
   CLAP_NOTE_DIALECT_CLAP = 1 << 0,
   CLAP_NOTE_DIALECT_MIDI = 1 << 1,
   CLAP_NOTE_DIALECT_MIDI_MPE = 1 << 2,
   CLAP_NOTE_DIALECT_MIDI2 = 1 << 3,
};
typedef struct clap_note_port_info {
   clap_id id;
   uint32_t supported_dialects;
   uint32_t preferred_dialect;
   char name[CLAP_NAME_SIZE];
} clap_note_port_info_t;
typedef struct clap_plugin_note_ports {
   uint32_t(CLAP_ABI *count)(const clap_plugin_t *plugin, bool is_input);
   bool(CLAP_ABI *get)(const clap_plugin_t *plugin, uint32_t index, bool is_input,
                       clap_note_port_info_t *info);
} clap_plugin_note_ports_t;
enum { CLAP_NOTE_PORTS_RESCAN_ALL = 1 << 0, CLAP_NOTE_PORTS_RESCAN_NAMES = 1 << 1 };
typedef struct clap_host_note_ports {
   uint32_t(CLAP_ABI *supported_dialects)(const clap_host_t *host);
   void(CLAP_ABI *rescan)(const clap_host_t *host, uint32_t flags);
} clap_host_note_ports_t;
#ifdef __cplusplus
}
#endif
