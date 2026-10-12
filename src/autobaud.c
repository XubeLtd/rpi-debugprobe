#include <pico/stdlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include <hardware/pio.h>
#include <hardware/dma.h>
#include <hardware/clocks.h>

#include "autobaud.h"
#include "probe_config.h"
#include "autobaud.pio.h"

// DMA buffer size
#define BUF_SIZE 1024

// Size of hash table for sample occurrence counts
#define HASH_TBL_SIZE 500

// Minimum sample occurrence ratio to consider a baud rate value valid
#define MIN_FREQUENCY 0.05f

// PIO clock frequency in Hz
#define PIO_CLOCK_FREQUENCY 125000000

typedef struct {
    int key;
    int count;
} Entry;

typedef struct {
    Entry *entries;
    size_t size;
} HashTable;

typedef struct {
    // PIO instance
    PIO pio;
    // PIO state machine
    int sm;
    // PIO program offset
    int offset;
    // UART RX GPIO pin
    uint rx_pin;

    // Frequency hash table to store samples occurance
    HashTable *freq_table;

    // Estimated baud rate
    float baud;

    // shortest bit duration in PIO cycles
    uint32_t min_cycles_count;
    // longest bit duration in PIO cycles
    uint32_t max_cycles_count;

    uint32_t total_samples;  // total samples seen
    uint32_t bit_time_sum;   // sum of 1-bit times
    uint32_t bit_time_count; // total 1-bit times
    uint32_t outlier_count;  // total of 1-bit times outliers

    // DMA channels to read RX PIO line
    int ctrl_chan;
    int data_chan;

    // DMA ring buffer storing PIO RX FIFO data
    uint32_t *rx_buffer;
    uintptr_t last_write_addr;
    uintptr_t curr_write_addr;
} autobaud_t;

static uint32_t rx_buffers[PROBE_COUNT][BUF_SIZE] __attribute__((aligned(4096)));

#define AUTOBAUD_INSTANCE(_pio, _rx_pin, _rx_buffer) { \
    .pio = _pio, .sm = -1, .offset = -1, .rx_pin = _rx_pin, \
    .min_cycles_count = UINT32_MAX, .ctrl_chan = -1, .data_chan = -1, \
    .rx_buffer = _rx_buffer, \
    .last_write_addr = (uintptr_t)_rx_buffer, .curr_write_addr = (uintptr_t)_rx_buffer }

static autobaud_t autobauds[PROBE_COUNT] = {
    AUTOBAUD_INSTANCE(pio0, PROBE_UART_RX, rx_buffers[0]),
#ifdef DUAL_PROBE_PICO
    AUTOBAUD_INSTANCE(PROBE_B_PIO, PROBE_B_UART_RX, rx_buffers[1]),
#endif
};

// DMA control channel reads this value to reload transfer count
static const uint32_t dma_reload_count = BUF_SIZE;

volatile bool autobaud_running[PROBE_COUNT];
volatile bool autobaud_stopped[PROBE_COUNT] = { [0 ... PROBE_COUNT - 1] = true };

// Queue to hold new baud rate estimates
QueueHandle_t baudQueue[PROBE_COUNT];

TaskHandle_t autobaud_taskhandle[PROBE_COUNT];


uint32_t hash(uint32_t x, size_t size) {
    x = ((x >> 16) ^ x) * 0x45d9f3bu;
    x = ((x >> 16) ^ x) * 0x45d9f3bu;
    x = (x >> 16) ^ x;
    return x % size;
}

HashTable *create_table(size_t size) {
    HashTable *table = malloc(sizeof(HashTable));
    if (!table) return NULL;
    table->size = size;
    table->entries = calloc(size, sizeof(Entry));
    if (!table->entries) {
        free(table);
        return NULL;
    }
    return table;
}

void insert(HashTable *table, int key) {
    uint32_t idx = hash(key, table->size);
    while (table->entries[idx].key != 0) {
        if (table->entries[idx].key == key) {
            table->entries[idx].count++;
            return;
        }
        idx = (idx + 1) % table->size;
    }
    table->entries[idx].key = key;
    table->entries[idx].count = 1;
}

int get_count(HashTable *table, int key) {
    uint32_t idx = hash(key, table->size);
    while (table->entries[idx].key != 0) {
        if (table->entries[idx].key == key) {
            return table->entries[idx].count;
        }
        idx = (idx + 1) % table->size;
    }
    return 0;
}

void free_table(HashTable *table) {
    free(table->entries);
    free(table);
}

bool dma_configure(autobaud_t *ab) {
    // Configure two DMA channels for continuous RX FIFO monitoring:
    // - data_chan: Continuously reads from PIO RX FIFO into circular buffer
    // - ctrl_chan: Triggers data_chan to restart when it completes a transfer
    ab->ctrl_chan = dma_claim_unused_channel(true);
    if (ab->ctrl_chan < 0) return false;
    ab->data_chan = dma_claim_unused_channel(true);
    if (ab->data_chan < 0) return false;

    dma_channel_config data_cfg = dma_channel_get_default_config(ab->data_chan);
    channel_config_set_transfer_data_size(&data_cfg, DMA_SIZE_32);
    channel_config_set_read_increment(&data_cfg, false);
    channel_config_set_write_increment(&data_cfg, true);
    channel_config_set_dreq(&data_cfg, pio_get_dreq(ab->pio, ab->sm, false)); // Trigger when PIO RX FIFO has data to read
    channel_config_set_chain_to(&data_cfg, ab->ctrl_chan); // Chain to control channel when transfer completes
    channel_config_set_ring(&data_cfg, true, 12); // Ring buffer size 2^12 = 4096 bytes (1024 uint32_t)

    dma_channel_configure(
        ab->data_chan,
        &data_cfg,
        ab->rx_buffer,
        &ab->pio->rxf[ab->sm],
        BUF_SIZE,
        false
    );

    dma_channel_config ctrl_cfg = dma_channel_get_default_config(ab->ctrl_chan);
    channel_config_set_transfer_data_size(&ctrl_cfg, DMA_SIZE_32);
    channel_config_set_read_increment(&ctrl_cfg, false);
    channel_config_set_write_increment(&ctrl_cfg, false);

    dma_channel_configure(
        ab->ctrl_chan,
        &ctrl_cfg,
        &(dma_hw->ch[ab->data_chan].al1_transfer_count_trig), // Destination: data channel's transfer count register
        &dma_reload_count,                               // Source: reload transfer count value (BUF_SIZE)
        1,
        false
    );

    dma_channel_start(ab->data_chan);
    return true;
}

// Compare rounded integer parts of baud rates
// Tolerance of 0.5% in detected versus set
static inline bool baud_changed(float new_baud, float baud) {
    uint32_t hi = (uint32_t)(baud * 1.005f);
    uint32_t lo = (uint32_t)(baud * 0.995f);
    uint32_t new = (uint32_t)new_baud;

    return (new > hi || new < lo);
}

// Processes new DMA samples read from the PIO RX FIFO. Each DMA sample
// represents a timestamp (or cycle count) between edges on the input signal.
// Accumulates these durations, filters noise, and estimates baud rate.
uint estimate_baud_rate(uint8_t itf) {
    autobaud_t *ab = &autobauds[itf];
    uint old_progress = ab->total_samples;
    // Get current DMA write address
    ab->curr_write_addr = dma_hw->ch[ab->data_chan].write_addr;
    // Convert absolute addresses to buffer indices
    size_t curr_index = ((ab->curr_write_addr) - (uintptr_t)ab->rx_buffer) / sizeof(ab->rx_buffer[0]);
    size_t last_index = (ab->last_write_addr - (uintptr_t)ab->rx_buffer) / sizeof(ab->rx_buffer[0]);

    for (size_t i = last_index; i != curr_index; i = (i + 1) % BUF_SIZE) {
        uint32_t raw = ab->rx_buffer[i];
        uint32_t curr_cycles_count = (UINT32_MAX - raw) * 2;
        insert(ab->freq_table, curr_cycles_count);

        ab->total_samples++;
        if (curr_cycles_count > ab->max_cycles_count)
            ab->max_cycles_count = curr_cycles_count;
        float freq = (float) get_count(ab->freq_table, curr_cycles_count) / (float) ab->total_samples;
        // if sample is seen at least 5% of all samples,
        // it is assumed it's not a noisy value
        if (freq < MIN_FREQUENCY) continue;
        if (curr_cycles_count < ab->min_cycles_count) {
            ab->min_cycles_count = curr_cycles_count;
            ab->bit_time_sum = 0;
            ab->bit_time_count = 0;
            ab->outlier_count = 0;
            continue;
        }
        // If current duration is within +10% of min_cycles, treat it as a "1-bit period"
        if ((curr_cycles_count - ab->min_cycles_count) < ((float)ab->min_cycles_count * 0.1f)) {
            ab->bit_time_sum += curr_cycles_count;
            ab->bit_time_count++;
            // 1-bit period should not be less than 1/9th of the longest period
            if (curr_cycles_count < (ab->max_cycles_count / 9))
                ab->outlier_count++;
            // Calculate baud from average of 1-bit times
            float avg_bit_time = (float) ab->bit_time_sum / (float) ab->bit_time_count;
            float new_baud = PIO_CLOCK_FREQUENCY / avg_bit_time;
            // If baud has changed, send updated baud information to cdc_thread
            if (baud_changed(new_baud, ab->baud)) {
                float completeness = 1.0f - expf(-(float) ab->total_samples / 40.0f);
                float noise_ratio = (float) ab->outlier_count / (float) ab->bit_time_count;
                float consistency = 1.0f - fminf(noise_ratio * 2.0f, 1.0f);
                float validity = completeness * consistency;
                if (validity > 0.6f) {
                    ab->baud = new_baud;
                    BaudInfo_t new_baud_info;
                    new_baud_info.baud = (uint32_t)roundf(ab->baud);
                    new_baud_info.validity = validity;
                    xQueueOverwrite(baudQueue[itf], &new_baud_info);
                }
            }
        }
    }
    ab->last_write_addr = ab->curr_write_addr;
    return ab->total_samples - old_progress;
}

void autobaud_deinit(uint8_t itf) {
    autobaud_t *ab = &autobauds[itf];

    // Stop and release the DMA channels, so whoever claims them next doesn't
    // get a channel that is still armed
    if (ab->ctrl_chan >= 0) {
        dma_channel_abort(ab->ctrl_chan);
        dma_channel_unclaim(ab->ctrl_chan);
        ab->ctrl_chan = -1;
    }
    if (ab->data_chan >= 0) {
        dma_channel_abort(ab->data_chan);
        dma_channel_unclaim(ab->data_chan);
        ab->data_chan = -1;
    }

    // Remove PIO program
    if (ab->sm >= 0) {
        pio_sm_set_enabled(ab->pio, ab->sm, false);
        pio_sm_unclaim(ab->pio, ab->sm);
    }
    if (ab->offset >= 0) {
        pio_remove_program(ab->pio, &autobaud_program, ab->offset);
    }

    if (ab->freq_table) {
        free_table(ab->freq_table);
        ab->freq_table = NULL;
    }
    if (baudQueue[itf]) {
        vQueueDelete(baudQueue[itf]);
        baudQueue[itf] = NULL;
    }

    // Reset state
    ab->sm = ab->offset = -1;
    ab->baud = 0.0f;
    ab->min_cycles_count = UINT32_MAX;
    ab->max_cycles_count = 0;
    ab->total_samples = ab->bit_time_sum = ab->bit_time_count = ab->outlier_count = 0;
    ab->last_write_addr = (uintptr_t)ab->rx_buffer;
    ab->curr_write_addr = (uintptr_t)ab->rx_buffer;
}

bool autobaud_init(uint8_t itf) {
    autobaud_t *ab = &autobauds[itf];

    // Claim a free PIO state machine
    ab->sm = pio_claim_unused_sm(ab->pio, true);
    if (ab->sm < 0)
        return false;
    // Add PIO program
    ab->offset = pio_add_program(ab->pio, &autobaud_program);
    if (ab->offset < 0) {
        autobaud_deinit(itf);
        return false;
    }
    float div = (float)clock_get_hz(clk_sys) / PIO_CLOCK_FREQUENCY;
    autobaud_program_init(ab->pio, ab->sm, ab->offset, ab->rx_pin, div);
    pio_sm_set_enabled(ab->pio, ab->sm, true);

    // Create hash table to keep count of sample occurance
    ab->freq_table = create_table(HASH_TBL_SIZE);
    if (!ab->freq_table) {
        autobaud_deinit(itf);
        return false;
    }
    // Create queue to send baud information to cdc_thread
    baudQueue[itf] = xQueueCreate(1, sizeof(BaudInfo_t));
    if (!baudQueue[itf]) {
        autobaud_deinit(itf);
        return false;
    }
    // Set up DMA to continuously write PIO RX data into RAM
    if (!dma_configure(ab)) {
        autobaud_deinit(itf);
        return false;
    }
    return true;
}

void autobaud_start(uint8_t itf) {
    xTaskNotify(autobaud_taskhandle[itf], AUTOBAUD_CMD_START, eSetValueWithOverwrite);
}

void autobaud_wait_stop(uint8_t itf) {
    while (!autobaud_stopped[itf])
        xTaskNotify(autobaud_taskhandle[itf], AUTOBAUD_CMD_STOP, eSetValueWithOverwrite);
}

// FreeRTOS thread running if MAGIC_BAUD was set by host
void autobaud_thread(void * param) {
    uint8_t itf = (uintptr_t)param;
    TickType_t wake = xTaskGetTickCount();
    uint32_t cmd = AUTOBAUD_CMD_NONE;
    uint processed;

    while (true) {
        if (!autobaud_running[itf]) {
            // Idle state: thread is blocked here until start command is received
            xTaskNotifyWait(0, 0xFFFFFFFFu, &cmd, portMAX_DELAY);
            if (cmd == AUTOBAUD_CMD_START) {
                if (autobaud_init(itf)) {
                    autobaud_running[itf] = true;
                    autobaud_stopped[itf] = false;
                }
            }
        } else {
            // Check if host requested autobaud termination
            if (xTaskNotifyWait(0, 0xFFFFFFFFu, &cmd, 0) == pdTRUE) {
                if (cmd == AUTOBAUD_CMD_STOP) {
                    autobaud_running[itf] = false;
                    autobaud_deinit(itf);
                    autobaud_stopped[itf] = true;
                    continue;
                }
            }
            processed = estimate_baud_rate(itf);
            if (!processed)
                xTaskDelayUntil(&wake, 1);
        }
    }
}
