#ifndef TARGET_VSENSE_H_
#define TARGET_VSENSE_H_

#include "pico/types.h"

void target_vsense_init(void);
// Samples the next probe's V-sense input; call periodically
void target_vsense_poll(void);
// Last sampled target voltage of probe 0 (A) or 1 (B), in mV
uint16_t target_vsense_get_mv(uint probe);

#endif
