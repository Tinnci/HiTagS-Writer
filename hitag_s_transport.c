/**
 * @file hitag_s_transport.c
 * @brief RF field control and BPLM transmit transport for HiTag S.
 */

#include "hitag_s_proto.h"
#include <furi.h>
#include <furi_hal.h>

#define TAG "HitagSTransport"

static void hitag_s_send_bit(bool value) {
    uint32_t t_low = HITAG_S_T_LOW_CYCLES * HITAG_S_T0_US;
    uint32_t t_total = value ? HITAG_S_T_1_CYCLES * HITAG_S_T0_US :
                               HITAG_S_T_0_CYCLES * HITAG_S_T0_US;

    furi_hal_rfid_tim_read_pause();
    furi_delay_us(t_low);
    furi_hal_rfid_tim_read_continue();
    furi_delay_us(t_total - t_low);
}

static void hitag_s_send_stop(void) {
    uint32_t t_low = HITAG_S_T_LOW_CYCLES * HITAG_S_T0_US;
    uint32_t t_stop = HITAG_S_T_STOP_CYCLES * HITAG_S_T0_US;

    furi_hal_rfid_tim_read_pause();
    furi_delay_us(t_low);
    furi_hal_rfid_tim_read_continue();
    furi_delay_us(t_stop);
}

void hitag_s_send_frame(const uint8_t* data, size_t bits) {
    for(size_t i = 0; i < bits; i++) {
        uint8_t byte_idx = i / 8;
        uint8_t bit_idx = 7 - (i % 8);
        bool value = (data[byte_idx] >> bit_idx) & 1U;
        hitag_s_send_bit(value);
    }
    hitag_s_send_stop();
}

void hitag_s_field_on(void) {
    furi_hal_rfid_tim_read_start(125000, 0.5f);
    furi_hal_rfid_pin_pull_pulldown();
    furi_delay_us(HITAG_S_T_WAIT_POWERUP_US);
    FURI_LOG_D(TAG, "Field ON, carrier 125kHz");
}

void hitag_s_field_off(void) {
    furi_hal_rfid_tim_read_stop();
    furi_hal_rfid_pins_reset();
    FURI_LOG_D(TAG, "Field OFF");
}
