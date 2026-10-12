#include "hardware/adc.h"

#include "probe_config.h"
#include "target_vsense.h"

#define ADC_VREF_MV 3300
#define ADC_RANGE 4096

static const uint vsense_pins[PROBE_COUNT] = { PROBE_PIN_VSENSE, PROBE_B_PIN_VSENSE };
static volatile uint16_t vsense_mv[PROBE_COUNT];
static uint next_probe;

void target_vsense_init(void)
{
    adc_init();
    for (uint i = 0; i < PROBE_COUNT; i++)
        adc_gpio_init(vsense_pins[i]);
}

// The probes share the single ADC, so they are sampled one at a time
void target_vsense_poll(void)
{
    uint i = next_probe;

    adc_select_input(vsense_pins[i] - ADC_BASE_PIN);
    vsense_mv[i] = adc_read() * ADC_VREF_MV * PROBE_VSENSE_DIVIDER / ADC_RANGE;
    next_probe = (i + 1) % PROBE_COUNT;
}

uint16_t target_vsense_get_mv(uint probe)
{
    return vsense_mv[probe];
}
