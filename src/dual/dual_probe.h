#ifndef DUAL_PROBE_H_
#define DUAL_PROBE_H_

#include "FreeRTOS.h"
#include "task.h"

// Probe B's copy of the DAP thread (see probe_b.h)
extern TaskHandle_t dap_taskhandle_b;
void dap_thread_b(void *ptr);

void dual_probe_init(void);

#endif
