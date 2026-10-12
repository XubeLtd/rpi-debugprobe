#ifndef SWO_UART_H_
#define SWO_UART_H_

// Provides the CMSIS drivers Driver_USART0 (probe A) and Driver_USART1
// (probe B) that SWO.c uses to capture UART (NRZ) SWO trace
void swo_uart_init(void);

#endif
