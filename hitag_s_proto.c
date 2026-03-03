/**
 * @file hitag_s_proto.c
 * @brief Hitag S protocol implementation for 8268 magic chips
 *
 * Uses furi_hal_rfid HAL APIs to generate BPLM encoded commands and
 * receive Manchester encoded responses from Hitag S tags.
 */

#include "hitag_s_proto.h"
#include <furi.h>
#include <furi_hal.h>
#include <toolbox/manchester_decoder.h>

#define TAG "HitagS"

/* ============================================================
 * CRC-8 Hitag S (polynomial 0x1D, init 0xFF)
 * ============================================================ */

uint8_t hitag_s_crc8(const uint8_t* data, size_t bits) {
    uint8_t crc = 0xFF;

    for(size_t i = 0; i < bits; i++) {
        uint8_t byte_idx = i / 8;
        uint8_t bit_idx = 7 - (i % 8); /* MSB first */
        uint8_t bit = (data[byte_idx] >> bit_idx) & 0x01;

        if((crc >> 7) ^ bit) {
            crc = (crc << 1) ^ 0x1D;
        } else {
            crc = crc << 1;
        }
    }

    return crc;
}

/* ============================================================
 * BPLM Transmit — Send bits to tag via field modulation
 * ============================================================ */

/**
 * Send a single BPLM bit: gap (field off) + field on for appropriate duration.
 *   '0': T_LOW gap, then total T_0 period
 *   '1': T_LOW gap, then total T_1 period
 */
static void hitag_s_send_bit(bool value) {
    uint32_t t_low = HITAG_S_T_LOW_CYCLES * HITAG_S_T0_US;
    uint32_t t_total;

    if(value) {
        t_total = HITAG_S_T_1_CYCLES * HITAG_S_T0_US;
    } else {
        t_total = HITAG_S_T_0_CYCLES * HITAG_S_T0_US;
    }

    /* Gap: carrier off */
    furi_hal_rfid_tim_read_pause();
    furi_delay_us(t_low);

    /* Carrier on for remainder of bit period */
    furi_hal_rfid_tim_read_continue();
    furi_delay_us(t_total - t_low);
}

/** Send a stop/EOF marker */
static void hitag_s_send_stop(void) {
    uint32_t t_stop = HITAG_S_T_STOP_CYCLES * HITAG_S_T0_US;

    furi_hal_rfid_tim_read_pause();
    furi_delay_us(t_stop);
    furi_hal_rfid_tim_read_continue();
}

void hitag_s_send_frame(const uint8_t* data, size_t bits) {
    for(size_t i = 0; i < bits; i++) {
        uint8_t byte_idx = i / 8;
        uint8_t bit_idx = 7 - (i % 8); /* MSB first */
        bool value = (data[byte_idx] >> bit_idx) & 0x01;
        hitag_s_send_bit(value);
    }
    hitag_s_send_stop();
}

/* ============================================================
 * Manchester Receive — Capture tag response via edge timing
 *
 * Uses Flipper's built-in manchester_advance() state machine
 * with adaptive threshold calibration from captured edges.
 *
 * Hitag S MC4K (4 kbit/s at 125kHz carrier):
 *   Bit period = 250µs = 32 carrier cycles
 *   Half-bit   = 125µs = 16 carrier cycles
 *   Short pulse ≈ 125µs (half-bit)
 *   Long pulse  ≈ 250µs (full-bit = two half-bits)
 *
 * The capture callback stores both level and duration for
 * accurate ManchesterEvent classification.
 * ============================================================ */

/* Maximum edges we can capture (128 bits × 2 edges/bit + SOF + margin) */
#define HITAG_S_MAX_EDGES 512

/* Edge capture context */
typedef struct {
    volatile uint32_t durations[HITAG_S_MAX_EDGES];
    volatile bool levels[HITAG_S_MAX_EDGES];
    volatile size_t edge_count;
    volatile bool overflow;
} HitagSCapture;

static HitagSCapture hs_capture;

static void hitag_s_capture_callback(bool level, uint32_t duration, void* context) {
    HitagSCapture* cap = context;

    if(cap->edge_count < HITAG_S_MAX_EDGES) {
        size_t idx = cap->edge_count;
        cap->durations[idx] = duration;
        cap->levels[idx] = level;
        cap->edge_count++;
    } else {
        cap->overflow = true;
    }
}

/**
 * @brief Auto-calibrate short/long pulse threshold from captured edges
 *
 * Strategy: Find the median of the smallest cluster of durations (short pulses)
 * and use 1.5× that as threshold. For Hitag S SOF in ADV mode, the first
 * data edges should be short pulses corresponding to '1' bits.
 *
 * @param cap       Capture context
 * @param threshold Output: threshold duration (short < threshold <= long)
 * @return true if calibration succeeded
 */
static bool hitag_s_calibrate_threshold(const HitagSCapture* cap, uint32_t* threshold) {
    if(cap->edge_count < 4) return false;

    /* Collect the first N durations and find min/max */
    uint32_t min_dur = UINT32_MAX;
    uint32_t max_dur = 0;
    uint32_t sum_short = 0;
    size_t count_short = 0;

    /* First pass: find the approximate short pulse duration range */
    size_t sample_count = (cap->edge_count < 20) ? cap->edge_count : 20;
    for(size_t i = 0; i < sample_count; i++) {
        uint32_t d = cap->durations[i];
        if(d < min_dur) min_dur = d;
        if(d > max_dur) max_dur = d;
    }

    if(min_dur == 0 || max_dur == 0) return false;

    /* The midpoint between min and max should roughly separate short from long.
     * Short ≈ half-bit, Long ≈ full-bit (2× short).
     * Use midpoint: (min + max) / 2 as initial separator, then refine. */
    uint32_t mid = (min_dur + max_dur) / 2;

    /* Second pass: average the "short" cluster (durations < mid) */
    for(size_t i = 0; i < sample_count; i++) {
        uint32_t d = cap->durations[i];
        if(d < mid) {
            sum_short += d;
            count_short++;
        }
    }

    if(count_short == 0) {
        /* All durations similar — use min × 1.5 as threshold */
        *threshold = min_dur * 3 / 2;
    } else {
        uint32_t avg_short = sum_short / count_short;
        /* Threshold = 1.5 × average short pulse duration */
        *threshold = avg_short * 3 / 2;
    }

    FURI_LOG_D(
        TAG,
        "MC calibrate: min=%lu max=%lu threshold=%lu (from %d edges)",
        (unsigned long)min_dur,
        (unsigned long)max_dur,
        (unsigned long)*threshold,
        (int)sample_count);

    return true;
}

/**
 * @brief Decode captured edges using Flipper's manchester_advance() state machine
 *
 * Classifies each edge as Short/Long + High/Low → ManchesterEvent,
 * then feeds into the built-in state machine which handles all
 * state transitions and outputs decoded bits.
 *
 * @param cap       Capture context with raw edges
 * @param out_data  Output buffer for decoded bits (packed, MSB first)
 * @param max_bits  Maximum bits to decode
 * @return Number of decoded bits
 */
static size_t hitag_s_decode_manchester(
    const HitagSCapture* cap,
    uint8_t* out_data,
    size_t max_bits) {
    if(cap->edge_count < 4) return 0;

    memset(out_data, 0, (max_bits + 7) / 8);

    /* Auto-calibrate threshold */
    uint32_t threshold = 0;
    if(!hitag_s_calibrate_threshold(cap, &threshold)) {
        FURI_LOG_W(TAG, "MC: threshold calibration failed");
        return 0;
    }

    ManchesterState state = ManchesterStateStart1;
    size_t bit_count = 0;

    for(size_t i = 0; i < cap->edge_count && bit_count < max_bits; i++) {
        uint32_t dur = cap->durations[i];
        bool level = cap->levels[i];
        bool is_short = (dur < threshold);

        /* Build ManchesterEvent from level + short/long */
        ManchesterEvent event;
        if(is_short) {
            event = level ? ManchesterEventShortHigh : ManchesterEventShortLow;
        } else {
            event = level ? ManchesterEventLongHigh : ManchesterEventLongLow;
        }

        /* Advance state machine */
        ManchesterState next_state;
        bool data_bit;

        if(manchester_advance(state, event, &next_state, &data_bit)) {
            /* A complete bit was decoded */
            if(data_bit) {
                out_data[bit_count / 8] |= (1 << (7 - (bit_count % 8)));
            }
            bit_count++;
        }

        state = next_state;
    }

    FURI_LOG_D(
        TAG,
        "MC decode: %d edges → %d bits (threshold=%lu)",
        (int)cap->edge_count,
        (int)bit_count,
        (unsigned long)threshold);

    /* Log first few decoded bytes for debugging */
    if(bit_count > 0) {
        size_t bytes = (bit_count + 7) / 8;
        if(bytes >= 4) {
            FURI_LOG_D(
                TAG,
                "MC data: %02X %02X %02X %02X (%d bits)",
                out_data[0],
                out_data[1],
                out_data[2],
                out_data[3],
                (int)bit_count);
        }
    }

    return bit_count;
}

size_t hitag_s_receive_frame(uint8_t* data, size_t max_bits, uint32_t timeout_us) {
    /* Reset capture state */
    hs_capture.edge_count = 0;
    hs_capture.overflow = false;

    /* Start capture */
    furi_hal_rfid_tim_read_capture_start(hitag_s_capture_callback, (void*)&hs_capture);

    /* Wait for response */
    furi_delay_us(timeout_us);

    /* Stop capture */
    furi_hal_rfid_tim_read_capture_stop();

    if(hs_capture.edge_count == 0) {
        FURI_LOG_D(TAG, "RX: no edges captured (timeout)");
        return 0;
    }

    FURI_LOG_D(
        TAG,
        "RX: captured %d edges%s",
        (int)hs_capture.edge_count,
        hs_capture.overflow ? " [OVERFLOW]" : "");

    /* Log raw edges for debugging (first 16) */
    size_t log_count = (hs_capture.edge_count < 16) ? hs_capture.edge_count : 16;
    for(size_t i = 0; i < log_count; i++) {
        FURI_LOG_D(
            TAG,
            "  edge[%d]: %s %lu",
            (int)i,
            hs_capture.levels[i] ? "HIGH" : "LOW ",
            (unsigned long)hs_capture.durations[i]);
    }

    /* Decode Manchester using Flipper's state machine */
    size_t bits = hitag_s_decode_manchester(&hs_capture, data, max_bits);

    return bits;
}

/* ============================================================
 * Field Control
 * ============================================================ */

void hitag_s_field_on(void) {
    furi_hal_rfid_tim_read_start(125000, 0.5f);
    furi_hal_rfid_pin_pull_release();
    furi_delay_us(HITAG_S_T_WAIT_POWERUP_US);
    FURI_LOG_I(TAG, "Field ON, carrier 125kHz");
}

void hitag_s_field_off(void) {
    furi_hal_rfid_tim_read_stop();
    furi_hal_rfid_pins_reset();
    FURI_LOG_I(TAG, "Field OFF");
}

/* ============================================================
 * Hitag S Command Builders
 * ============================================================ */

/**
 * Pack bits into byte array (MSB first).
 * Helper to build command frames.
 */
static void pack_bits(uint8_t* buf, size_t* bit_pos, uint32_t value, size_t n_bits) {
    for(size_t i = 0; i < n_bits; i++) {
        size_t pos = *bit_pos + i;
        uint8_t byte_idx = pos / 8;
        uint8_t bit_idx = 7 - (pos % 8);
        bool bit_val = (value >> (n_bits - 1 - i)) & 0x01;
        if(bit_val) {
            buf[byte_idx] |= (1 << bit_idx);
        } else {
            buf[byte_idx] &= ~(1 << bit_idx);
        }
    }
    *bit_pos += n_bits;
}

HitagSResult hitag_s_uid_request(uint32_t* uid) {
    /* UID_REQ_ADV1 = 0xC8 = 11001 (5 bits) */
    uint8_t cmd[1] = {0};
    size_t bit_pos = 0;
    pack_bits(cmd, &bit_pos, 0x19, 5); /* 11001 = 0x19 */

    FURI_LOG_D(TAG, "TX: UID_REQ_ADV1 (5 bits)");

    {
        FURI_CRITICAL_ENTER();
        hitag_s_send_frame(cmd, 5);
        FURI_CRITICAL_EXIT();
    }

    /* Receive 32-bit UID */
    uint8_t rx[4] = {0};
    size_t rx_bits = hitag_s_receive_frame(rx, 32, HITAG_S_T_WAIT_RESP_US * 20);

    if(rx_bits < 32) {
        FURI_LOG_W(TAG, "UID request: only %d bits received", (int)rx_bits);
        return HitagSResultTimeout;
    }

    *uid = ((uint32_t)rx[0] << 24) | ((uint32_t)rx[1] << 16) |
           ((uint32_t)rx[2] << 8) | (uint32_t)rx[3];

    FURI_LOG_I(TAG, "UID: %08lX", (unsigned long)*uid);
    return HitagSResultOk;
}

HitagSResult hitag_s_select(uint32_t uid, uint32_t* config) {
    /* SELECT = 00000 (5 bits) + UID (32 bits) + CRC8 (8 bits) = 45 bits */
    uint8_t cmd[6] = {0}; /* 48 bits capacity */
    size_t bit_pos = 0;

    /* 5-bit command: 00000 */
    pack_bits(cmd, &bit_pos, 0x00, 5);
    /* 32-bit UID */
    pack_bits(cmd, &bit_pos, uid, 32);
    /* CRC8 over first 37 bits */
    uint8_t crc = hitag_s_crc8(cmd, 37);
    pack_bits(cmd, &bit_pos, crc, 8);

    FURI_LOG_D(TAG, "TX: SELECT UID=%08lX CRC=%02X (45 bits)", (unsigned long)uid, crc);

    furi_delay_us(HITAG_S_T_WAIT_SC_US);

    {
        FURI_CRITICAL_ENTER();
        hitag_s_send_frame(cmd, 45);
        FURI_CRITICAL_EXIT();
    }

    /* Receive 32-bit config page */
    uint8_t rx[4] = {0};
    size_t rx_bits = hitag_s_receive_frame(rx, 32, HITAG_S_T_WAIT_RESP_US * 20);

    if(rx_bits < 32) {
        FURI_LOG_W(TAG, "SELECT: only %d bits received", (int)rx_bits);
        return HitagSResultTimeout;
    }

    *config = ((uint32_t)rx[0] << 24) | ((uint32_t)rx[1] << 16) |
              ((uint32_t)rx[2] << 8) | (uint32_t)rx[3];

    FURI_LOG_I(TAG, "Config page: %08lX", (unsigned long)*config);
    return HitagSResultOk;
}

HitagSResult hitag_s_8268_authenticate(uint32_t password) {
    /* WRITE_PAGE to page 64 (authentication trigger for 8268)
     * WRITE_PAGE = 1000 (4 bits) + addr (8 bits) + CRC8 (8 bits) = 20 bits
     */
    uint8_t cmd[3] = {0}; /* 24 bits capacity */
    size_t bit_pos = 0;

    /* 4-bit command: 1000 */
    pack_bits(cmd, &bit_pos, 0x08, 4);
    /* 8-bit page address: 64 = 0x40 */
    pack_bits(cmd, &bit_pos, HITAG_S_8268_AUTH_PAGE, 8);
    /* CRC8 over first 12 bits */
    uint8_t crc = hitag_s_crc8(cmd, 12);
    pack_bits(cmd, &bit_pos, crc, 8);

    FURI_LOG_D(TAG, "TX: WRITE_PAGE addr=64 CRC=%02X (20 bits) [AUTH step 1]", crc);

    furi_delay_us(HITAG_S_T_WAIT_SC_US);

    {
        FURI_CRITICAL_ENTER();
        hitag_s_send_frame(cmd, 20);
        FURI_CRITICAL_EXIT();
    }

    /* Receive 2-bit ACK */
    uint8_t ack[1] = {0};
    size_t ack_bits = hitag_s_receive_frame(ack, 8, HITAG_S_T_WAIT_RESP_US * 10);

    if(ack_bits < 2) {
        FURI_LOG_W(TAG, "AUTH step 1: no ACK (%d bits)", (int)ack_bits);
        return HitagSResultTimeout;
    }

    /* Check ACK (top 2 bits of ack[0] should be 01) */
    uint8_t ack_val = (ack[0] >> 6) & 0x03;
    if(ack_val != HITAG_S_ACK) {
        FURI_LOG_W(TAG, "AUTH step 1: NACK (got 0x%02X)", ack_val);
        return HitagSResultNack;
    }

    /* Now send 32-bit password + CRC8 = 40 bits */
    uint8_t pwd_frame[5] = {0};
    size_t pwd_pos = 0;
    pack_bits(pwd_frame, &pwd_pos, password, 32);
    uint8_t pwd_crc = hitag_s_crc8(pwd_frame, 32);
    pack_bits(pwd_frame, &pwd_pos, pwd_crc, 8);

    FURI_LOG_D(
        TAG,
        "TX: Password=%08lX CRC=%02X (40 bits) [AUTH step 2]",
        (unsigned long)password,
        pwd_crc);

    furi_delay_us(HITAG_S_T_WAIT_SC_US);

    {
        FURI_CRITICAL_ENTER();
        hitag_s_send_frame(pwd_frame, 40);
        FURI_CRITICAL_EXIT();
    }

    /* Receive 2-bit ACK */
    uint8_t ack2[1] = {0};
    size_t ack2_bits = hitag_s_receive_frame(ack2, 8, HITAG_S_T_WAIT_RESP_US * 10);

    if(ack2_bits < 2) {
        FURI_LOG_W(TAG, "AUTH step 2: no ACK (%d bits)", (int)ack2_bits);
        return HitagSResultTimeout;
    }

    uint8_t ack2_val = (ack2[0] >> 6) & 0x03;
    if(ack2_val != HITAG_S_ACK) {
        FURI_LOG_W(TAG, "AUTH step 2: NACK (got 0x%02X)", ack2_val);
        return HitagSResultNack;
    }

    FURI_LOG_I(TAG, "8268 authentication successful!");
    return HitagSResultOk;
}

HitagSResult hitag_s_write_page(uint8_t page, uint32_t data) {
    /* WRITE_PAGE = 1000 (4 bits) + addr (8 bits) + CRC8 (8 bits) = 20 bits */
    uint8_t cmd[3] = {0};
    size_t bit_pos = 0;

    pack_bits(cmd, &bit_pos, 0x08, 4); /* 1000 */
    pack_bits(cmd, &bit_pos, page, 8);
    uint8_t crc = hitag_s_crc8(cmd, 12);
    pack_bits(cmd, &bit_pos, crc, 8);

    FURI_LOG_D(TAG, "TX: WRITE_PAGE addr=%d CRC=%02X (20 bits)", page, crc);

    furi_delay_us(HITAG_S_T_WAIT_SC_US);

    {
        FURI_CRITICAL_ENTER();
        hitag_s_send_frame(cmd, 20);
        FURI_CRITICAL_EXIT();
    }

    /* Receive ACK */
    uint8_t ack[1] = {0};
    size_t ack_bits = hitag_s_receive_frame(ack, 8, HITAG_S_T_WAIT_RESP_US * 10);

    if(ack_bits < 2) {
        FURI_LOG_W(TAG, "WRITE_PAGE addr=%d: no ACK", page);
        return HitagSResultTimeout;
    }

    uint8_t ack_val = (ack[0] >> 6) & 0x03;
    if(ack_val != HITAG_S_ACK) {
        FURI_LOG_W(TAG, "WRITE_PAGE addr=%d: NACK (0x%02X)", page, ack_val);
        return HitagSResultNack;
    }

    /* Send 32-bit data + CRC8 = 40 bits */
    uint8_t data_frame[5] = {0};
    size_t data_pos = 0;
    pack_bits(data_frame, &data_pos, data, 32);
    uint8_t data_crc = hitag_s_crc8(data_frame, 32);
    pack_bits(data_frame, &data_pos, data_crc, 8);

    FURI_LOG_D(
        TAG,
        "TX: Data=%08lX CRC=%02X (40 bits)",
        (unsigned long)data,
        data_crc);

    furi_delay_us(HITAG_S_T_WAIT_SC_US);

    {
        FURI_CRITICAL_ENTER();
        hitag_s_send_frame(data_frame, 40);
        FURI_CRITICAL_EXIT();
    }

    /* Wait for programming and receive ACK */
    furi_delay_us(HITAG_S_T_PROG_US);

    uint8_t ack2[1] = {0};
    size_t ack2_bits = hitag_s_receive_frame(ack2, 8, HITAG_S_T_WAIT_RESP_US * 20);

    if(ack2_bits < 2) {
        /* Some tags don't ACK after programming — treat as OK */
        FURI_LOG_D(TAG, "WRITE_PAGE addr=%d: no final ACK (may be OK)", page);
        return HitagSResultOk;
    }

    uint8_t ack2_val = (ack2[0] >> 6) & 0x03;
    if(ack2_val != HITAG_S_ACK) {
        FURI_LOG_W(TAG, "WRITE_PAGE addr=%d: final NACK (0x%02X)", page, ack2_val);
        return HitagSResultNack;
    }

    FURI_LOG_I(TAG, "WRITE_PAGE addr=%d: OK", page);
    return HitagSResultOk;
}

HitagSResult hitag_s_read_page(uint8_t page, uint32_t* data) {
    /* READ_PAGE = 1100 (4 bits) + addr (8 bits) + CRC8 (8 bits) = 20 bits */
    uint8_t cmd[3] = {0};
    size_t bit_pos = 0;

    pack_bits(cmd, &bit_pos, 0x0C, 4); /* 1100 */
    pack_bits(cmd, &bit_pos, page, 8);
    uint8_t crc = hitag_s_crc8(cmd, 12);
    pack_bits(cmd, &bit_pos, crc, 8);

    FURI_LOG_D(TAG, "TX: READ_PAGE addr=%d CRC=%02X (20 bits)", page, crc);

    furi_delay_us(HITAG_S_T_WAIT_SC_US);

    {
        FURI_CRITICAL_ENTER();
        hitag_s_send_frame(cmd, 20);
        FURI_CRITICAL_EXIT();
    }

    /* Receive 32-bit page data */
    uint8_t rx[4] = {0};
    size_t rx_bits = hitag_s_receive_frame(rx, 32, HITAG_S_T_WAIT_RESP_US * 20);

    if(rx_bits < 32) {
        FURI_LOG_W(TAG, "READ_PAGE addr=%d: only %d bits received", page, (int)rx_bits);
        return HitagSResultTimeout;
    }

    *data = ((uint32_t)rx[0] << 24) | ((uint32_t)rx[1] << 16) |
            ((uint32_t)rx[2] << 8) | (uint32_t)rx[3];

    FURI_LOG_I(TAG, "READ_PAGE addr=%d: %08lX", page, (unsigned long)*data);
    return HitagSResultOk;
}

/* ============================================================
 * High-Level Sequences
 * ============================================================ */

HitagSResult hitag_s_read_uid_sequence(uint32_t* uid) {
    hitag_s_field_on();

    HitagSResult result = hitag_s_uid_request(uid);

    hitag_s_field_off();
    return result;
}

HitagSResult hitag_s_8268_write_sequence(
    uint32_t password,
    const uint32_t* pages,
    const uint8_t* page_addrs,
    size_t page_count) {
    uint32_t uid = 0;
    uint32_t config = 0;

    hitag_s_field_on();

    /* Step 1: UID request */
    HitagSResult result = hitag_s_uid_request(&uid);
    if(result != HitagSResultOk) {
        FURI_LOG_E(TAG, "Write sequence: UID request failed");
        hitag_s_field_off();
        return result;
    }

    /* Step 2: SELECT */
    result = hitag_s_select(uid, &config);
    if(result != HitagSResultOk) {
        FURI_LOG_E(TAG, "Write sequence: SELECT failed");
        hitag_s_field_off();
        return result;
    }

    /* Step 3: Authenticate */
    result = hitag_s_8268_authenticate(password);
    if(result != HitagSResultOk) {
        FURI_LOG_E(TAG, "Write sequence: Authentication failed");
        hitag_s_field_off();
        return result;
    }

    /* Step 4: Write pages */
    for(size_t i = 0; i < page_count; i++) {
        result = hitag_s_write_page(page_addrs[i], pages[i]);
        if(result != HitagSResultOk) {
            FURI_LOG_E(TAG, "Write sequence: Write page %d failed", page_addrs[i]);
            hitag_s_field_off();
            return result;
        }
    }

    FURI_LOG_I(TAG, "Write sequence: All %d pages written successfully!", (int)page_count);
    hitag_s_field_off();
    return HitagSResultOk;
}
