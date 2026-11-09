#pragma once

#include "events.h"
#include "audio-buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
   CLAP_PROCESS_ERROR = 0,
   CLAP_PROCESS_CONTINUE = 1,
   CLAP_PROCESS_CONTINUE_IF_NOT_QUIET = 2,
   CLAP_PROCESS_TAIL = 3,
   CLAP_PROCESS_SLEEP = 4,
};
typedef int32_t clap_process_status;

typedef struct clap_process {
   int64_t steady_time;
   uint32_t frames_count;
   const clap_event_transport_t *transport;
   const clap_audio_buffer_t *audio_inputs;
   clap_audio_buffer_t *audio_outputs;
   uint32_t audio_inputs_count;
   uint32_t audio_outputs_count;
   const clap_input_events_t *in_events;
   const clap_output_events_t *out_events;
} clap_process_t;

#ifdef __cplusplus
}
#endif
