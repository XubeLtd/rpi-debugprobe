#include "tusb.h"
#include "pico/usb_reset.h"

#include "DAP_config.h"
#include "DAP.h"
#include "dual_probe.h"
#include "swo_uart.h"
#include "target_vsense.h"

TaskHandle_t dap_taskhandle_b;

extern usbd_class_driver_t const _dap_edpt_driver, _dap_edpt_driver_b;
void DAP_Setup_b(void);

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count)
{
    static usbd_class_driver_t drivers[3];

    drivers[0] = _dap_edpt_driver;
    drivers[1] = _dap_edpt_driver_b;
    drivers[2] = usb_reset_interface_driver;
    *driver_count = TU_ARRAY_SIZE(drivers);
    return drivers;
}

// ID_DAP_Vendor0 reports the target voltage in mV, as a little-endian uint16
static uint32_t process_vendor_command(uint probe, const uint8_t *request, uint8_t *response)
{
    uint32_t num = (1U << 16) | 1U;

    *response++ = *request;
    if (*request == ID_DAP_Vendor0) {
        uint16_t mv = target_vsense_get_mv(probe);

        *response++ = (uint8_t)mv;
        *response = (uint8_t)(mv >> 8);
        num += 2U;
    }
    return num;
}

uint32_t DAP_ProcessVendorCommand(const uint8_t *request, uint8_t *response)
{
    return process_vendor_command(0, request, response);
}

uint32_t DAP_ProcessVendorCommand_b(const uint8_t *request, uint8_t *response)
{
    return process_vendor_command(1, request, response);
}

void dual_probe_init(void)
{
    // The SWD engines use these state machines without claiming them, so
    // claim them before autobaud or SWO capture can be given them
    pio_sm_claim(pio0, PROBE_SM);
    pio_sm_claim(PROBE_B_PIO, PROBE_B_SM);
    target_vsense_init();
    swo_uart_init();
    DAP_Setup_b();
}
