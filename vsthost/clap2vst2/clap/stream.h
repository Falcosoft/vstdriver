#pragma once

#include "private/std.h"
#include "private/macros.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct clap_istream {
   void *ctx;
   int64_t(CLAP_ABI *read)(const struct clap_istream *stream, void *buffer, uint64_t size);
} clap_istream_t;

typedef struct clap_ostream {
   void *ctx;
   int64_t(CLAP_ABI *write)(const struct clap_ostream *stream, const void *buffer, uint64_t size);
} clap_ostream_t;

#ifdef __cplusplus
}
#endif
