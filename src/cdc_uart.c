/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2021 Raspberry Pi (Trading) Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include <pico/stdlib.h>
#include "FreeRTOS.h"
#include "task.h"
#include "tusb.h"
#include "autobaud.h"

#include "probe_config.h"

static_assert(CFG_TUD_CDC == PROBE_COUNT, "Each probe needs a CDC interface");

TaskHandle_t uart_taskhandle[PROBE_COUNT];

typedef struct {
    uart_inst_t *uart;
    uint tx_pin;
    uint rx_pin;
    TickType_t interval;
    volatile TickType_t break_expiry;
    volatile bool timed_break;
    /* Max 1 FIFO worth of data */
    uint8_t tx_buf[32];
    uint8_t rx_buf[32];
    uint debounce_ticks;
    int was_connected;
    uint cdc_tx_oe;
} cdc_uart_t;

static cdc_uart_t cdc_uarts[PROBE_COUNT] = {
    { .uart = PROBE_UART_INTERFACE, .tx_pin = PROBE_UART_TX, .rx_pin = PROBE_UART_RX, .interval = 100, .debounce_ticks = 5 },
#ifdef DUAL_PROBE_PICO
    { .uart = PROBE_B_UART_INTERFACE, .tx_pin = PROBE_B_UART_TX, .rx_pin = PROBE_B_UART_RX, .interval = 100, .debounce_ticks = 5 },
#endif
};

// Actually s^-1 so 25ms
#define DEBOUNCE_MS 40

#ifdef PROBE_UART_TX_LED
static volatile uint tx_led_debounce;
#endif

#ifdef PROBE_UART_RX_LED
static uint rx_led_debounce;
#endif

void cdc_uart_init(void) {
    for (uint i = 0; i < PROBE_COUNT; i++) {
        cdc_uart_t *u = &cdc_uarts[i];

        gpio_set_function(u->tx_pin, GPIO_FUNC_UART);
        gpio_set_function(u->rx_pin, GPIO_FUNC_UART);
        gpio_set_pulls(u->tx_pin, 1, 0);
        gpio_set_pulls(u->rx_pin, 1, 0);
        uart_init(u->uart, PROBE_UART_BAUDRATE);
    }

#ifdef PROBE_UART_TX_LED
    tx_led_debounce = 0;
    gpio_init(PROBE_UART_TX_LED);
    gpio_set_dir(PROBE_UART_TX_LED, GPIO_OUT);
#endif
#ifdef PROBE_UART_RX_LED
    rx_led_debounce = 0;
    gpio_init(PROBE_UART_RX_LED);
    gpio_set_dir(PROBE_UART_RX_LED, GPIO_OUT);
#endif

#ifdef PROBE_UART_HWFC
    /* HWFC implies that hardware flow control is implemented and the
     * UART operates in "full-duplex" mode (See USB CDC PSTN120 6.3.12).
     * Default to pulling in the active direction, so an unconnected CTS
     * behaves the same as if CTS were not enabled. */
    gpio_set_pulls(PROBE_UART_CTS, 0, 1);
    gpio_set_function(PROBE_UART_RTS, GPIO_FUNC_UART);
    gpio_set_function(PROBE_UART_CTS, GPIO_FUNC_UART);
    uart_set_hw_flow(PROBE_UART_INTERFACE, true, true);
#else
#ifdef PROBE_UART_RTS
    gpio_init(PROBE_UART_RTS);
    gpio_set_dir(PROBE_UART_RTS, GPIO_OUT);
    gpio_put(PROBE_UART_RTS, 1);
#endif
#endif

#ifdef PROBE_UART_DTR
    gpio_init(PROBE_UART_DTR);
    gpio_set_dir(PROBE_UART_DTR, GPIO_OUT);
    gpio_put(PROBE_UART_DTR, 1);
#endif
}

bool cdc_task(uint8_t itf)
{
    cdc_uart_t *u = &cdc_uarts[itf];
    uint rx_len = 0;
    bool keep_alive = false;

    // Consume uart fifo regardless even if not connected
    while(uart_is_readable(u->uart) && (rx_len < sizeof(u->rx_buf))) {
        u->rx_buf[rx_len++] = uart_getc(u->uart);
    }

    if (tud_cdc_n_connected(itf)) {
        u->was_connected = 1;
        int written = 0;
        /* Implicit overflow if we don't write all the bytes to the host.
         * Also throw away bytes if we can't write... */
        if (rx_len) {
#ifdef PROBE_UART_RX_LED
          gpio_put(PROBE_UART_RX_LED, 1);
          rx_led_debounce = u->debounce_ticks;
#endif
          written = MIN(tud_cdc_n_write_available(itf), rx_len);
          if (rx_len > written)
              u->cdc_tx_oe++;

          if (written > 0) {
            tud_cdc_n_write(itf, u->rx_buf, written);
            tud_cdc_n_write_flush(itf);
          }
        } else {
#ifdef PROBE_UART_RX_LED
          if (rx_led_debounce)
            rx_led_debounce--;
          else
            gpio_put(PROBE_UART_RX_LED, 0);
#endif
        }

      /* Reading from a firehose and writing to a FIFO. */
      size_t watermark = MIN(tud_cdc_n_available(itf), sizeof(u->tx_buf));
      if (watermark > 0) {
        size_t tx_len;
#ifdef PROBE_UART_TX_LED
        gpio_put(PROBE_UART_TX_LED, 1);
        tx_led_debounce = u->debounce_ticks;
#endif
        /* Batch up to half a FIFO of data - don't clog up on RX */
        watermark = MIN(watermark, 16);
        tx_len = tud_cdc_n_read(itf, u->tx_buf, watermark);
        uart_write_blocking(u->uart, u->tx_buf, tx_len);
      } else {
#ifdef PROBE_UART_TX_LED
          if (tx_led_debounce)
            tx_led_debounce--;
          else
            gpio_put(PROBE_UART_TX_LED, 0);
#endif
      }
      /* Pending break handling */
      if (u->timed_break) {
        if (((int)u->break_expiry - (int)xTaskGetTickCount()) < 0) {
          u->timed_break = false;
          uart_set_break(u->uart, false);
#ifdef PROBE_UART_TX_LED
          tx_led_debounce = 0;
#endif
        } else {
          keep_alive = true;
        }
      }
    } else if (u->was_connected) {
      tud_cdc_n_write_clear(itf);
      uart_set_break(u->uart, false);
      u->timed_break = false;
      u->was_connected = 0;
#ifdef PROBE_UART_TX_LED
      tx_led_debounce = 0;
#endif
      u->cdc_tx_oe = 0;
    }
    return keep_alive;
}

void cdc_uart_set_baudrate(uint8_t itf, uint32_t baudrate) {
  cdc_uart_t *u = &cdc_uarts[itf];
  /* Set the tick thread interval to the amount of time it takes to
   * fill up half a FIFO. Millis is too coarse for integer divide.
   */
  uint32_t micros = (1000 * 1000 * 16 * 10) / MAX(baudrate, 1);
  u->interval = MAX(1, micros / ((1000 * 1000) / configTICK_RATE_HZ));
  u->debounce_ticks = MAX(1, configTICK_RATE_HZ / (u->interval * DEBOUNCE_MS));
  probe_info("New baud rate %ld micros %ld interval %lu\n",
              baudrate, micros, u->interval);
  uart_deinit(u->uart);
  tud_cdc_n_write_clear(itf);
  tud_cdc_n_read_flush(itf);

  uart_init(u->uart, baudrate);
}

void cdc_thread(void *ptr)
{
  uint8_t itf = (uintptr_t)ptr;
  cdc_uart_t *u = &cdc_uarts[itf];
  BaseType_t delayed;
  TickType_t last_wake = xTaskGetTickCount();
  BaudInfo_t baud_info;
  bool keep_alive;
  /* Threaded with a polling interval that scales according to linerate */
  while (1) {
    keep_alive = cdc_task(itf);
    if (!keep_alive) {
      delayed = xTaskDelayUntil(&last_wake, u->interval);
      if (delayed == pdFALSE)
        last_wake = xTaskGetTickCount();
      if (autobaud_running[itf]) {
        // Receive baud information from autobaud thread
        if (xQueueReceive(baudQueue[itf], &baud_info, 0) == pdTRUE) {
          cdc_uart_set_baudrate(itf, baud_info.baud);
          // Assume 8N1
          uart_set_format(u->uart, 8, 1, UART_PARITY_NONE);
        }
      }
    }
  }
}

void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const* line_coding)
{
  if (line_coding->bit_rate == MAGIC_BAUD) {
    if (!autobaud_running[itf])
      autobaud_start(itf);
    return;
  }
  else if (autobaud_running[itf]) {
    autobaud_wait_stop(itf);
  }
  uart_parity_t parity;
  uint data_bits, stop_bits;

  /* Modifying state, so park the thread before changing it. */
  if (tud_cdc_n_connected(itf))
    vTaskSuspend(uart_taskhandle[itf]);

  cdc_uart_set_baudrate(itf, line_coding->bit_rate);

  switch (line_coding->parity) {
  case CDC_LINE_CODING_PARITY_ODD:
    parity = UART_PARITY_ODD;
    break;
  case CDC_LINE_CODING_PARITY_EVEN:
    parity = UART_PARITY_EVEN;
    break;
  default:
    probe_info("invalid parity setting %u\n", line_coding->parity);
    /* fallthrough */
  case CDC_LINE_CODING_PARITY_NONE:
    parity = UART_PARITY_NONE;
    break;
  }

  switch (line_coding->data_bits) {
  case 5:
  case 6:
  case 7:
  case 8:
    data_bits = line_coding->data_bits;
    break;
  default:
    probe_info("invalid data bits setting: %u\n", line_coding->data_bits);
    data_bits = 8;
    break;
  }

  /* The PL011 only supports 1 or 2 stop bits. 1.5 stop bits is translated to 2,
   * which is safer than the alternative. */
  switch (line_coding->stop_bits) {
  case CDC_LINE_CONDING_STOP_BITS_1_5:
  case CDC_LINE_CONDING_STOP_BITS_2:
    stop_bits = 2;
  break;
  default:
    probe_info("invalid stop bits setting: %u\n", line_coding->stop_bits);
    /* fallthrough */
  case CDC_LINE_CONDING_STOP_BITS_1:
    stop_bits = 1;
  break;
  }

  uart_set_format(cdc_uarts[itf].uart, data_bits, stop_bits, parity);
  /* Windows likes to arbitrarily set/get line coding after dtr/rts changes, so
   * don't resume if we shouldn't */
  if(tud_cdc_n_connected(itf))
    vTaskResume(uart_taskhandle[itf]);
}

void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
#ifdef PROBE_UART_RTS
  gpio_put(PROBE_UART_RTS, !rts);
#endif
#ifdef PROBE_UART_DTR
  gpio_put(PROBE_UART_DTR, !dtr);
#endif

  /* CDC drivers use linestate as a bodge to activate/deactivate the interface.
   * Resume our UART polling on activate, stop on deactivate */
  if (!dtr) {
    vTaskSuspend(uart_taskhandle[itf]);
#ifdef PROBE_UART_RX_LED
    gpio_put(PROBE_UART_RX_LED, 0);
    rx_led_debounce = 0;
#endif
#ifdef PROBE_UART_TX_LED
    gpio_put(PROBE_UART_TX_LED, 0);
    tx_led_debounce = 0;
#endif
  } else
    vTaskResume(uart_taskhandle[itf]);
}

void tud_cdc_send_break_cb(uint8_t itf, uint16_t wValue) {
  cdc_uart_t *u = &cdc_uarts[itf];

  switch(wValue) {
    case 0:
    uart_set_break(u->uart, false);
    u->timed_break = false;
#ifdef PROBE_UART_TX_LED
    tx_led_debounce = 0;
#endif
    break;
    case 0xffff:
    uart_set_break(u->uart, true);
    u->timed_break = false;
#ifdef PROBE_UART_TX_LED
    gpio_put(PROBE_UART_TX_LED, 1);
    tx_led_debounce = 1 << 30;
#endif
    break;
    default:
    uart_set_break(u->uart, true);
    u->timed_break = true;
#ifdef PROBE_UART_TX_LED
    gpio_put(PROBE_UART_TX_LED, 1);
    tx_led_debounce = 1 << 30;
#endif
    u->break_expiry = xTaskGetTickCount() + (wValue * (configTICK_RATE_HZ / 1000));
    break;
  }
}
