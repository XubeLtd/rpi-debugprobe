#ifndef PROBE_B_H_
#define PROBE_B_H_

// Builds probe B's copy of an unmodified single-probe source file: include
// this, then the source. The PROBE_* settings (probe A's) are switched to
// probe B's, and every externally visible symbol gets a _b suffix so both
// copies link into the same image.

#include "probe_config.h"
#include "hardware/pio.h"

#undef PROBE_SM
#define PROBE_SM PROBE_B_SM
#undef PROBE_PIN_OFFSET
#define PROBE_PIN_OFFSET PROBE_B_PIN_OFFSET
#undef PROBE_PIN_SWCLK
#define PROBE_PIN_SWCLK PROBE_B_PIN_SWCLK
#undef PROBE_PIN_SWDIO
#define PROBE_PIN_SWDIO PROBE_B_PIN_SWDIO
#undef PROBE_PIN_RESET
#define PROBE_PIN_RESET PROBE_B_PIN_RESET
#undef PROBE_PIN_SWO
#define PROBE_PIN_SWO PROBE_B_PIN_SWO
#undef PROBE_DAP_ITF
#define PROBE_DAP_ITF PROBE_B_DAP_ITF
#undef PROBE_SER_SUFFIX
#define PROBE_SER_SUFFIX PROBE_B_SER_SUFFIX

// SWO.c captures through CMSIS driver Driver_USART<USART_PORT>
#define USART_PORT 1

// probe.c and probe.pio use pio0 directly
#undef pio0
#define pio0 PROBE_B_PIO

// probe.c
#define probe_set_swclk_freq probe_set_swclk_freq_b
#define probe_write_bits probe_write_bits_b
#define probe_read_bits probe_read_bits_b
#define probe_hiz_clocks probe_hiz_clocks_b
#define probe_read_mode probe_read_mode_b
#define probe_write_mode probe_write_mode_b
#define probe_init probe_init_b
#define probe_deinit probe_deinit_b
#define probe_assert_reset probe_assert_reset_b
#define probe_reset_level probe_reset_level_b

// sw_dp_pio.c
#define cached_delay cached_delay_b
#define SWJ_Sequence SWJ_Sequence_b
#define SWD_Sequence SWD_Sequence_b
#define SWD_Transfer SWD_Transfer_b

// DAP.c
#define DAP_Data DAP_Data_b
#define DAP_TransferAbort DAP_TransferAbort_b
#define Delayms Delayms_b
#define DAP_ProcessVendorCommand DAP_ProcessVendorCommand_b
#define DAP_ProcessCommand DAP_ProcessCommand_b
#define DAP_ExecuteCommand DAP_ExecuteCommand_b
#define DAP_Setup DAP_Setup_b

// SWO.c
#define SWO_Transport SWO_Transport_b
#define SWO_Mode SWO_Mode_b
#define SWO_Baudrate SWO_Baudrate_b
#define SWO_Control SWO_Control_b
#define SWO_Status SWO_Status_b
#define SWO_ExtendedStatus SWO_ExtendedStatus_b
#define SWO_Data SWO_Data_b
#define UART_SWO_Mode UART_SWO_Mode_b
#define UART_SWO_Baudrate UART_SWO_Baudrate_b
#define UART_SWO_Control UART_SWO_Control_b
#define UART_SWO_Capture UART_SWO_Capture_b
#define UART_SWO_GetCount UART_SWO_GetCount_b

// tusb_edpt_handler.c
#define buffer_full buffer_full_b
#define buffer_empty buffer_empty_b
#define dap_edpt_init dap_edpt_init_b
#define dap_edpt_deinit dap_edpt_deinit_b
#define dap_edpt_reset dap_edpt_reset_b
#define dap_edpt_open dap_edpt_open_b
#define dap_edpt_control_xfer_cb dap_edpt_control_xfer_cb_b
#define dap_edpt_xfer_cb dap_edpt_xfer_cb_b
#define dap_cmd_string dap_cmd_string_b
#define dap_thread dap_thread_b
#define dap_taskhandle dap_taskhandle_b
#define _dap_edpt_driver _dap_edpt_driver_b

#endif
