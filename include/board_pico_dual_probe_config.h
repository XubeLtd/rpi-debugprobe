#ifndef BOARD_PICO_DUAL_PROBE_CONFIG_H_
#define BOARD_PICO_DUAL_PROBE_CONFIG_H_

// Two independent probes on one Pico. Probe A uses the regular PROBE_*
// settings; probe B's copy of the probe sources is built with the PROBE_B_*
// settings by src/dual/probe_b.h.

#define PROBE_COUNT 2

#define PROBE_IO_RAW
#define PROBE_CDC_UART

// Probe A (pio0)
#define PROBE_SM 0
#define PROBE_PIN_OFFSET 2
#define PROBE_PIN_SWCLK (PROBE_PIN_OFFSET + 0) // 2
#define PROBE_PIN_SWDIO (PROBE_PIN_OFFSET + 1) // 3
#define PROBE_PIN_RESET 6
#define PROBE_PIN_SWO 7
#define PROBE_PIN_VSENSE 26
#define PROBE_UART_TX 4
#define PROBE_UART_RX 5
#define PROBE_UART_INTERFACE uart1
#define PROBE_DAP_ITF 0
#define PROBE_SER_SUFFIX "-A"
#define PROBE_DAP_CONNECTED_LED 25
#define PROBE_DAP_RUNNING_LED 25

// Probe B
#define PROBE_B_PIO pio1
#define PROBE_B_SM 0
#define PROBE_B_PIN_OFFSET 18
#define PROBE_B_PIN_SWCLK (PROBE_B_PIN_OFFSET + 0) // 18
#define PROBE_B_PIN_SWDIO (PROBE_B_PIN_OFFSET + 1) // 19
#define PROBE_B_PIN_RESET 20
#define PROBE_B_PIN_SWO 22
#define PROBE_B_PIN_VSENSE 27
#define PROBE_B_UART_TX 16
#define PROBE_B_UART_RX 17
#define PROBE_B_UART_INTERFACE uart0
#define PROBE_B_DAP_ITF 3
#define PROBE_B_SER_SUFFIX "-B"
#define PROBE_B_DAP_CONNECTED_LED 25
#define PROBE_B_DAP_RUNNING_LED 25

#define PROBE_UART_BAUDRATE 115200

// 100K/100K divider between the target supply and the V-sense pins
#define PROBE_VSENSE_DIVIDER 2

#define PROBE_PRODUCT_STRING "Dual Debugprobe (CMSIS-DAP)"

#endif
