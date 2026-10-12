#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/pio.h"

#include "Driver_USART.h"
#include "probe_config.h"
#include "swo_uart.h"
#include "swo_uart.pio.h"

typedef struct {
    PIO pio;
    uint pin;
    int sm;
    int offset;
    int dma_chan;
    uint dma_irq;
    ARM_USART_SignalEvent_t cb_event;
    uint32_t rx_num;
    volatile bool rx_busy;
} swo_uart_t;

static swo_uart_t swo_uarts[PROBE_COUNT] = {
    { .pio = pio0, .pin = PROBE_PIN_SWO, .sm = -1, .offset = -1, .dma_chan = -1 },
    { .pio = PROBE_B_PIO, .pin = PROBE_B_PIN_SWO, .sm = -1, .offset = -1, .dma_chan = -1 },
};

static void swo_dma_irq_handler(uint irq)
{
    for (uint i = 0; i < PROBE_COUNT; i++) {
        swo_uart_t *u = &swo_uarts[i];

        if (u->dma_chan >= 0 && u->dma_irq == irq && dma_irqn_get_channel_status(irq, u->dma_chan)) {
            dma_irqn_acknowledge_channel(irq, u->dma_chan);
            u->rx_busy = false;
            u->cb_event(ARM_USART_EVENT_RECEIVE_COMPLETE);
        }
    }
}

static void swo_dma_irq0_handler(void)
{
    swo_dma_irq_handler(0);
}

static void swo_dma_irq1_handler(void)
{
    swo_dma_irq_handler(1);
}

void swo_uart_init(void)
{
    irq_add_shared_handler(dma_get_irq_num(0), swo_dma_irq0_handler, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
    irq_add_shared_handler(dma_get_irq_num(1), swo_dma_irq1_handler, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
}

static int32_t swo_initialize(swo_uart_t *u, ARM_USART_SignalEvent_t cb_event)
{
    // SWO.c expects its callback to interrupt the thread that drives it, as on
    // a single core. So completions go to the DMA IRQ of the calling (DAP
    // thread's) core, enabled on that core only.
    u->cb_event = cb_event;
    u->dma_irq = get_core_num();
    irq_set_priority(dma_get_irq_num(u->dma_irq), PICO_HIGHEST_IRQ_PRIORITY);
    irq_set_enabled(dma_get_irq_num(u->dma_irq), true);
    return ARM_DRIVER_OK;
}

static int32_t swo_uninitialize(swo_uart_t *u)
{
    u->cb_event = NULL;
    return ARM_DRIVER_OK;
}

static void swo_abort_receive(swo_uart_t *u)
{
    // Keep the channel's IRQ off across the abort (RP2040-E13)
    dma_irqn_set_channel_enabled(u->dma_irq, u->dma_chan, false);
    dma_channel_abort(u->dma_chan);
    dma_irqn_acknowledge_channel(u->dma_irq, u->dma_chan);
    u->rx_busy = false;
}

static void swo_power_off(swo_uart_t *u)
{
    if (u->sm >= 0) {
        pio_sm_set_enabled(u->pio, u->sm, false);
        pio_sm_unclaim(u->pio, u->sm);
        u->sm = -1;
    }
    if (u->offset >= 0) {
        pio_remove_program(u->pio, &swo_uart_rx_program, u->offset);
        u->offset = -1;
    }
    if (u->dma_chan >= 0) {
        swo_abort_receive(u);
        dma_channel_unclaim(u->dma_chan);
        u->dma_chan = -1;
    }
    gpio_deinit(u->pin);
    gpio_disable_pulls(u->pin);
}

static int32_t swo_power_control(swo_uart_t *u, ARM_POWER_STATE state)
{
    switch (state) {
    case ARM_POWER_OFF:
        swo_power_off(u);
        return ARM_DRIVER_OK;
    case ARM_POWER_FULL:
        if (u->sm >= 0)
            return ARM_DRIVER_OK;
        gpio_init(u->pin);
        gpio_pull_up(u->pin);
        u->sm = pio_claim_unused_sm(u->pio, false);
        u->offset = pio_add_program(u->pio, &swo_uart_rx_program);
        u->dma_chan = dma_claim_unused_channel(false);
        if (u->sm < 0 || u->offset < 0 || u->dma_chan < 0) {
            swo_power_off(u);
            return ARM_DRIVER_ERROR;
        }
        return ARM_DRIVER_OK;
    default:
        return ARM_DRIVER_ERROR_UNSUPPORTED;
    }
}

static int32_t swo_set_baudrate(swo_uart_t *u, uint32_t baudrate)
{
    if (baudrate == 0)
        return ARM_USART_ERROR_BAUDRATE;
    // The receiver program takes 8 cycles per bit
    float div = (float)clock_get_hz(clk_sys) / (8.0f * baudrate);
    if (div < 1.0f || div > 65536.0f)
        return ARM_USART_ERROR_BAUDRATE;
    swo_uart_rx_program_init(u->pio, u->sm, u->offset, u->pin, div);
    return ARM_DRIVER_OK;
}

static void swo_enable_rx(swo_uart_t *u, bool enable)
{
    if (enable) {
        // The receiver may have been stopped mid-byte
        pio_sm_restart(u->pio, u->sm);
        pio_sm_clear_fifos(u->pio, u->sm);
        pio_sm_exec(u->pio, u->sm, pio_encode_jmp(u->offset));
    }
    pio_sm_set_enabled(u->pio, u->sm, enable);
}

static int32_t swo_control(swo_uart_t *u, uint32_t control, uint32_t arg)
{
    switch (control & ARM_USART_CONTROL_Msk) {
    case ARM_USART_MODE_ASYNCHRONOUS:
        // Only 8N1, which is all-zero mode parameters
        if (control != ARM_USART_MODE_ASYNCHRONOUS)
            return ARM_DRIVER_ERROR_UNSUPPORTED;
        return swo_set_baudrate(u, arg);
    case ARM_USART_CONTROL_RX:
        swo_enable_rx(u, arg != 0);
        return ARM_DRIVER_OK;
    case ARM_USART_ABORT_RECEIVE:
        swo_abort_receive(u);
        return ARM_DRIVER_OK;
    default:
        return ARM_DRIVER_ERROR_UNSUPPORTED;
    }
}

static int32_t swo_receive(swo_uart_t *u, void *data, uint32_t num)
{
    dma_channel_config c = dma_channel_get_default_config(u->dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(u->pio, u->sm, false));

    u->rx_num = num;
    u->rx_busy = true;
    dma_irqn_acknowledge_channel(u->dma_irq, u->dma_chan);
    dma_irqn_set_channel_enabled(u->dma_irq, u->dma_chan, true);
    // Read the byte the receiver left-justified in the FIFO entry
    dma_channel_configure(u->dma_chan, &c, data, (io_rw_8 *)&u->pio->rxf[u->sm] + 3, num, true);
    return ARM_DRIVER_OK;
}

static uint32_t swo_get_rx_count(swo_uart_t *u)
{
    return u->rx_num - dma_channel_hw_addr(u->dma_chan)->transfer_count;
}

static ARM_USART_STATUS swo_get_status(swo_uart_t *u)
{
    ARM_USART_STATUS status = { .rx_busy = u->rx_busy };
    return status;
}

// Only the functions SWO.c uses
#define SWO_USART_DRIVER(n) \
    static int32_t usart##n##_initialize(ARM_USART_SignalEvent_t cb_event) { return swo_initialize(&swo_uarts[n], cb_event); } \
    static int32_t usart##n##_uninitialize(void) { return swo_uninitialize(&swo_uarts[n]); } \
    static int32_t usart##n##_power_control(ARM_POWER_STATE state) { return swo_power_control(&swo_uarts[n], state); } \
    static int32_t usart##n##_receive(void *data, uint32_t num) { return swo_receive(&swo_uarts[n], data, num); } \
    static uint32_t usart##n##_get_rx_count(void) { return swo_get_rx_count(&swo_uarts[n]); } \
    static int32_t usart##n##_control(uint32_t control, uint32_t arg) { return swo_control(&swo_uarts[n], control, arg); } \
    static ARM_USART_STATUS usart##n##_get_status(void) { return swo_get_status(&swo_uarts[n]); } \
    ARM_DRIVER_USART Driver_USART##n = { \
        .Initialize = usart##n##_initialize, \
        .Uninitialize = usart##n##_uninitialize, \
        .PowerControl = usart##n##_power_control, \
        .Receive = usart##n##_receive, \
        .GetRxCount = usart##n##_get_rx_count, \
        .Control = usart##n##_control, \
        .GetStatus = usart##n##_get_status, \
    };

SWO_USART_DRIVER(0)
SWO_USART_DRIVER(1)
