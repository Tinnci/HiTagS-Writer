/**
 * @file hitag_s_transport.c
 * @brief RF field control and BPLM transmit transport for HiTag S.
 */

#include "hitag_s_proto.h"
#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_bus.h>

static bool hitag_s_field_active = false;

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

static void hitag_s_send_htu_sof(void) {
    uint32_t t_low = HITAG_S_T_LOW_CYCLES * HITAG_S_T0_US;
    uint32_t t_violation = HITAG_S_T_CODE_VIOLATION_CYCLES * HITAG_S_T0_US;

    hitag_s_send_bit(false);
    furi_hal_rfid_tim_read_pause();
    furi_delay_us(t_low);
    furi_hal_rfid_tim_read_continue();
    furi_delay_us(t_violation);
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

void hitag_s_send_frame_with_early_rx(
    const uint8_t* data,
    size_t bits,
    HitagSRxStartCallback start_rx,
    void* context) {
    uint32_t t_low = HITAG_S_T_LOW_CYCLES * HITAG_S_T0_US;
    uint32_t t_stop = HITAG_S_T_STOP_CYCLES * HITAG_S_T0_US;

    FURI_CRITICAL_ENTER();
    for(size_t i = 0; i < bits; i++) {
        uint8_t byte_idx = i / 8;
        uint8_t bit_idx = 7 - (i % 8);
        bool value = (data[byte_idx] >> bit_idx) & 1U;
        hitag_s_send_bit(value);
    }

    furi_hal_rfid_tim_read_pause();
    furi_delay_us(t_low);
    furi_hal_rfid_tim_read_continue();
    if(start_rx) start_rx(context);
    FURI_CRITICAL_EXIT();

    furi_delay_us(t_stop);
}

void hitag_s_send_htu_frame_with_early_rx(
    const uint8_t* data,
    size_t bits,
    HitagSRxStartCallback start_rx,
    void* context) {
    uint32_t t_low = HITAG_S_T_LOW_CYCLES * HITAG_S_T0_US;

    FURI_CRITICAL_ENTER();
    hitag_s_send_htu_sof();
    for(size_t i = 0; i < bits; i++) {
        uint8_t byte_idx = i / 8;
        uint8_t bit_idx = 7 - (i % 8);
        bool value = (data[byte_idx] >> bit_idx) & 1U;
        hitag_s_send_bit(value);
    }

    /* Hitag µ EOF: Proxmark sends one final low pulse, then opens RX on carrier. */
    furi_hal_rfid_tim_read_pause();
    furi_delay_us(t_low);
    furi_hal_rfid_tim_read_continue();
    if(start_rx) start_rx(context);
    FURI_CRITICAL_EXIT();
}

void hitag_s_field_on(void) {
    furi_hal_rfid_tim_read_start(125000, 0.5f);
    furi_hal_rfid_pin_pull_release();
    hitag_s_field_active = true;
    furi_delay_us(HITAG_S_T_WAIT_POWERUP_US);
}

void hitag_s_field_off(void) {
    if(furi_hal_bus_is_enabled(FuriHalBusTIM1)) {
        furi_hal_rfid_tim_read_stop();
    }
    furi_hal_rfid_pins_reset();
    hitag_s_field_active = false;
}

void hitag_s_field_reset_hard(uint32_t off_ms) {
    if(furi_hal_bus_is_enabled(FuriHalBusTIM1)) {
        furi_hal_rfid_tim_read_stop();
    }
    furi_hal_rfid_pins_reset();
    hitag_s_field_active = false;
    furi_delay_ms(off_ms);
}

void hitag_s_field_on_no_wait(void) {
    furi_hal_rfid_tim_read_start(125000, 0.5f);
    furi_hal_rfid_pin_pull_release();
    hitag_s_field_active = true;
}

void hitag_s_send_pause_us(uint32_t pause_us) {
    furi_hal_rfid_tim_read_pause();
    furi_delay_us(pause_us);
    furi_hal_rfid_tim_read_continue();
}
