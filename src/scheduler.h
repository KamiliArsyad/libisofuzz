#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "isofuzz_ctx.h"
#include "../include/isofuzz.h"

void scheduler_init();
void scheduler_shutdown();

// The scheduler now takes the library-internal transaction ID and the intent.
void scheduler_request(uint64_t trx_lib_id, IsoFuzzSchedulerIntent intent);

#endif // SCHEDULER_H
