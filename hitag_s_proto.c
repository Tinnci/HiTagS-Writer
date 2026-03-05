/**
 * @file hitag_s_proto.c
 * @brief Hitag S protocol implementation for 8268 magic chips
 *
 * Uses furi_hal_rfid HAL APIs to generate BPLM encoded commands and
 * receive Manchester encoded responses from Hitag S tags.
 */

#include "hitag_s_proto.h"
#include "em4100_encode.h"
#include <furi.h>
#include <furi_hal.h>
#include <flipper_format/flipper_format.h>
#include <storage/storage.h>

#define TAG "HitagS"

/* ============================================================
 * Debug Trace — internal state
 * ============================================================ */
static FuriString* g_debug_trace = NULL;
static bool g_trace_active = false;

/** Append formatted text to trace buffer (if tracing is active) */
static void trace_append(const char* fmt, ...) {
    if(!g_trace_active || !g_debug_trace) return;
    va_list args;
    va_start(args, fmt);
    furi_string_cat_vprintf(g_debug_trace, fmt, args);
    va_end(args);
}

void hitag_s_debug_trace_start(void) {
    if(g_debug_trace) {
        furi_string_free(g_debug_trace);
    }
    g_debug_trace = furi_string_alloc();
    furi_string_cat_str(g_debug_trace, "=== HiTag S Debug Trace ===\n");
    g_trace_active = true;
    FURI_LOG_I(TAG, "Debug trace started");
}

void* hitag_s_debug_trace_stop(void) {
    g_trace_active = false;
    FuriString* result = g_debug_trace;
    g_debug_trace = NULL;
    FURI_LOG_I(TAG, "Debug trace stopped (%d bytes)", result ? (int)furi_string_size(result) : 0);
    return result;
}

#define HITAGS_TRACE_FILETYPE "HiTag S Debug Trace"
#define HITAGS_TRACE_VERSION  1

bool hitag_s_debug_trace_save(void* storage_ptr, const char* path, void* trace_string) {
    FuriString* trace = (FuriString*)trace_string;
    if(!trace || furi_string_size(trace) == 0) return false;

    Storage* storage = (Storage*)storage_ptr;
    File* file = storage_file_alloc(storage);
    bool ok = false;

    if(storage_file_open(file, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        size_t len = furi_string_size(trace);
        size_t written = storage_file_write(file, furi_string_get_cstr(trace), len);
        ok = (written == len);
        if(ok) {
            FURI_LOG_I(TAG, "Trace saved: %s (%d bytes)", path, (int)len);
        } else {
            FURI_LOG_E(TAG, "Trace write error: %d/%d", (int)written, (int)len);
        }
    } else {
        FURI_LOG_E(TAG, "Trace file open failed: %s", path);
    }

    storage_file_close(file);
    storage_file_free(file);
    return ok;
}

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
 * Tag Response Decoders
 *
 * Hitag S uses two response encoding modes:
 *
 * AC2K (Anti-Collision 2kbit/s) — for UID responses:
 *   Bit period = 512µs = 64 × T0
 *   '0' = LOAD(half) + UNLOAD(half) [one rising edge per bit]
 *   '1' = L/U/L/U quarter periods   [two rising edges per bit]
 *   Decoded via rising-edge interval analysis (Proxmark3 algorithm).
 *
 * MC4K (Manchester 4kbit/s) — for data exchange after SELECT:
 *   Bit period = 256µs = 32 × T0
 *   Half-bit = 128µs = 16 × T0
 *   Uses Flipper's manchester_advance() state machine.
 *
 * Capture callback stores edge level + duration from TIM2:
 *   level=true  (CH3 falling): HIGH pulse width
 *   level=false (CH4 rising+reset): total period (rising-to-rising)
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
 * @brief Decode AC2K anti-collision response (used for UID)
 *
 * AC2K encodes bits as tag load modulation patterns. The rising-to-rising
 * intervals (level=false periods from TIM2) classify as:
 *   TWO_HALF   (~256µs) : within a '1' bit (alternating L/U pattern)
 *   THREE_HALF (~384µs) : transition between '0' and '1'
 *   FOUR_HALF  (~512µs) : consecutive '0' bits
 *
 * Based on Proxmark3 hitag_common.c AC decoding algorithm.
 *
 * @param cap       Capture context with raw edges
 * @param out_data  Output buffer for decoded bits (packed, MSB first)
 * @param max_bits  Maximum data bits to decode (excluding SOF)
 * @param sof_bits  Number of SOF bits to strip (1 for STD, 3 for ADV)
 * @return Number of decoded data bits (after SOF stripping)
 */
static size_t hitag_s_decode_ac2k(
    const HitagSCapture* cap,
    uint8_t* out_data,
    size_t max_bits,
    size_t sof_bits) {
    memset(out_data, 0, (max_bits + 7) / 8);

    int lastbit = 0;
    bool bSkip = false;
    size_t total_bits = 0;
    size_t sof_remaining = sof_bits;
    size_t data_bits = 0;
    bool first_period = true;
    size_t period_count = 0;

    for(size_t i = 0; i < cap->edge_count && data_bits < max_bits; i++) {
        if(cap->levels[i]) continue; /* skip HIGH entries, only use periods */

        uint32_t rb = cap->durations[i];

        if(first_period) {
            first_period = false;
            FURI_LOG_I(TAG, "AC2K: skip startup period %lu", (unsigned long)rb);
            continue;
        }

        if(rb < HITAG_S_AC2K_GLITCH_US) continue;

        /* Log first 10 periods for debugging */
        if(period_count < 10) {
            const char* cls = (rb >= HITAG_S_AC2K_THRESH_34_US) ? "4H" :
                              (rb >= HITAG_S_AC2K_THRESH_23_US) ? "3H" :
                                                                  "2H";
            FURI_LOG_I(TAG, "AC2K p[%d]: %lu (%s)", (int)period_count, (unsigned long)rb, cls);
        }
        period_count++;

        if(rb >= HITAG_S_AC2K_THRESH_34_US) {
            /* FOUR_HALF: one '0' bit */
            lastbit = 0;
            total_bits++;
            if(sof_remaining > 0) {
                sof_remaining--;
            } else {
                data_bits++;
            }
        } else if(rb >= HITAG_S_AC2K_THRESH_23_US) {
            /* THREE_HALF: transition between 0 and 1 */
            lastbit = !lastbit;
            total_bits++;
            if(sof_remaining > 0) {
                sof_remaining--;
            } else {
                if(lastbit) {
                    out_data[data_bits / 8] |= (1 << (7 - (data_bits % 8)));
                }
                data_bits++;
            }
            bSkip = (lastbit != 0);
        } else {
            /* TWO_HALF: within a '1' bit */
            if(!bSkip) {
                lastbit = 1;
                total_bits++;
                if(sof_remaining > 0) {
                    sof_remaining--;
                } else {
                    out_data[data_bits / 8] |= (1 << (7 - (data_bits % 8)));
                    data_bits++;
                }
            }
            bSkip = !bSkip;
        }
    }

    FURI_LOG_I(
        TAG,
        "AC2K: %d edges, %d periods -> %d bits (%d SOF + %d data)",
        (int)cap->edge_count,
        (int)period_count,
        (int)total_bits,
        (int)sof_bits,
        (int)data_bits);

    if(data_bits > 0) {
        size_t bytes = (data_bits + 7) / 8;
        if(bytes >= 4) {
            FURI_LOG_D(
                TAG,
                "AC2K data: %02X %02X %02X %02X (%d bits)",
                out_data[0],
                out_data[1],
                out_data[2],
                out_data[3],
                (int)data_bits);
        }
    }

    return data_bits;
}

/**
 * @brief Decode MC4K Manchester response (used after SELECT)
 *
 * MC4K at 125kHz: half-bit = 128µs (16 × T0), full-bit = 256µs (32 × T0)
 *
 * Algorithm: Half-period tracking decoder.
 * Verified via Python simulation (sim_mc4k_final.py) against all bit patterns.
 *
 * MC4K encoding (Hitag S / IEEE 802.3 convention):
 *   bit 0: tag UNLOAD(h) LOAD(h) → COMP1: HIGH(h) LOW(h)  → 2nd half = LOW
 *   bit 1: tag LOAD(h) UNLOAD(h) → COMP1: LOW(h)  HIGH(h) → 2nd half = HIGH
 *
 * Steps:
 * 1. Extract HIGH/LOW pulse pairs from capture events
 * 2. First pair is initial carrier → skip HIGH, keep LOW as SOF start
 * 3. Classify each pulse as SHORT (1 half-period) or LONG (2 half-periods)
 * 4. Build half-period level stream, pair up
 * 5. Bit value = 1 if second half is HIGH, 0 if LOW
 *
 * @param cap       Capture context with raw edges
 * @param out_data  Output buffer for decoded bits (packed, MSB first)
 * @param max_bits  Maximum data bits to decode (excluding SOF)
 * @param sof_bits  Number of SOF bits to strip
 * @param threshold Midpoint between SHORT and LONG pulse (µs)
 * @return Number of decoded data bits (after SOF stripping)
 */
static size_t hitag_s_decode_mc4k(
    const HitagSCapture* cap,
    uint8_t* out_data,
    size_t max_bits,
    size_t sof_bits,
    uint32_t threshold) {
    if(cap->edge_count < 4) return 0;

    memset(out_data, 0, (max_bits + 7) / 8);

    uint32_t glitch_min = (threshold > 200) ? 80 : HITAG_S_MC4K_GLITCH_US;

    FURI_LOG_D(
        TAG, "MC: threshold=%lu, glitch=%lu", (unsigned long)threshold, (unsigned long)glitch_min);

    /* --- Step 1-2: Extract pulse sequence from capture events ---
     * CC3 (level=true):  HIGH pulse duration (COMP1 HIGH time)
     * CC4 (level=false): period (rising-to-rising)
     * Each CC3/CC4 pair gives: HIGH_dur and LOW_dur = period - HIGH_dur.
     *
     * First pair is carrier→SOF transition:
     *   HIGH_dur = large (carrier time, skip)
     *   LOW_dur = first half of SOF bit '1' (keep)
     */

    /* Half-period buffer: stores level (true=HIGH, false=LOW) for each half-period */
#define MC4K_MAX_HALF_PERIODS ((HITAG_S_MAX_EDGES / 2) * 3)
    bool hp_levels[MC4K_MAX_HALF_PERIODS];
    size_t hp_count = 0;
    bool started = false;
    uint32_t last_high_dur = 0;

    for(size_t i = 0; i < cap->edge_count; i++) {
        bool level = cap->levels[i];
        uint32_t dur = cap->durations[i];

        if(level) {
            /* CC3 event: HIGH pulse duration */
            if(dur >= glitch_min) {
                last_high_dur = dur;
            }
            continue;
        }

        /* CC4 event: period */
        if(last_high_dur == 0 || dur <= last_high_dur) {
            last_high_dur = 0;
            continue;
        }

        uint32_t high_dur = last_high_dur;
        uint32_t low_dur = dur - high_dur;
        last_high_dur = 0;

        if(!started) {
            /* First pair: carrier HIGH → skip, keep LOW as SOF start */
            started = true;
            FURI_LOG_D(
                TAG,
                "MC: initial carrier H=%lu, SOF start L=%lu",
                (unsigned long)high_dur,
                (unsigned long)low_dur);
            if(low_dur >= glitch_min && hp_count < MC4K_MAX_HALF_PERIODS) {
                size_t n = (low_dur < threshold) ? 1 : 2;
                for(size_t j = 0; j < n && hp_count < MC4K_MAX_HALF_PERIODS; j++) {
                    hp_levels[hp_count++] = false; /* LOW */
                }
            }
            continue;
        }

        /* Normal pair: HIGH pulse then LOW pulse */
        if(high_dur >= glitch_min) {
            size_t n = (high_dur < threshold) ? 1 : 2;
            for(size_t j = 0; j < n && hp_count < MC4K_MAX_HALF_PERIODS; j++) {
                hp_levels[hp_count++] = true; /* HIGH */
            }
        }
        if(low_dur >= glitch_min) {
            size_t n = (low_dur < threshold) ? 1 : 2;
            for(size_t j = 0; j < n && hp_count < MC4K_MAX_HALF_PERIODS; j++) {
                hp_levels[hp_count++] = false; /* LOW */
            }
        }
    }

    /* If odd number of half-periods, pad with HIGH (carrier idle) */
    if((hp_count % 2) == 1 && hp_count < MC4K_MAX_HALF_PERIODS) {
        hp_levels[hp_count++] = true;
    }

    FURI_LOG_D(TAG, "MC: %d half-periods from %d edges", (int)hp_count, (int)cap->edge_count);

    /* --- Step 3-4: Pair half-periods into bits ---
     * bit value = 1 if second half is HIGH, 0 if second half is LOW */
    size_t total_bits = hp_count / 2;
    size_t sof_remaining = sof_bits;
    size_t data_bits = 0;

    for(size_t i = 0; i < total_bits && data_bits < max_bits; i++) {
        bool second_half = hp_levels[i * 2 + 1];

        if(sof_remaining > 0) {
            sof_remaining--;
        } else {
            if(second_half) {
                out_data[data_bits / 8] |= (1 << (7 - (data_bits % 8)));
            }
            data_bits++;
        }
    }

    FURI_LOG_I(
        TAG,
        "MC4K: %d edges -> %d hp -> %d bits (%d SOF + %d data)",
        (int)cap->edge_count,
        (int)hp_count,
        (int)total_bits,
        (int)sof_bits,
        (int)data_bits);

    if(data_bits > 0) {
        size_t bytes = (data_bits + 7) / 8;
        if(bytes >= 4) {
            FURI_LOG_D(
                TAG,
                "MC4K data: %02X %02X %02X %02X (%d bits)",
                out_data[0],
                out_data[1],
                out_data[2],
                out_data[3],
                (int)data_bits);
        }
    }

    return data_bits;
}

/* Decode mode for send_receive */
typedef enum {
    HitagSRxAC2K = 0, /* AC2K anti-collision (UID response) - interval based */
    HitagSRxMC4K = 1, /* MC4K Manchester 4kbit/s (data exchange, threshold 192µs) */
    HitagSRxMC2K = 2, /* MC2K Manchester 2kbit/s (UID response, threshold 384µs) */
} HitagSRxMode;

/**
 * @brief Combined send + receive with proper sequencing
 *
 * @param tx_data     BPLM data to send (packed bits, MSB first)
 * @param tx_bits     Number of bits to send
 * @param rx_data     Buffer for received bits
 * @param rx_max_bits Maximum data bits to receive (excluding SOF)
 * @param rx_timeout_us How long to wait for response after TX
 * @param rx_mode     Decode mode (AC2K for UID, MC4K for data exchange)
 * @param sof_bits    Number of SOF bits to strip from response
 * @return Number of decoded data bits received
 */
static size_t hitag_s_send_receive(
    const uint8_t* tx_data,
    size_t tx_bits,
    uint8_t* rx_data,
    size_t rx_max_bits,
    uint32_t rx_timeout_us,
    HitagSRxMode rx_mode,
    size_t sof_bits) {
    /* Send command in critical section (interrupts disabled = precise timing) */
    FURI_CRITICAL_ENTER();
    hitag_s_send_frame(tx_data, tx_bits);
    FURI_CRITICAL_EXIT();

    /* Now start capture — tag responds ~200µs after our stop bit */
    hs_capture.edge_count = 0;
    hs_capture.overflow = false;
    furi_hal_rfid_tim_read_capture_start(hitag_s_capture_callback, (void*)&hs_capture);

    /* Wait for tag response edges */
    furi_delay_us(rx_timeout_us);

    /* Stop capture */
    furi_hal_rfid_tim_read_capture_stop();

    if(hs_capture.edge_count == 0) {
        FURI_LOG_D(TAG, "RX: no edges (timeout %lu us)", (unsigned long)rx_timeout_us);
        trace_append("  RX: no edges (timeout %lu us)\n", (unsigned long)rx_timeout_us);
        return 0;
    }

    const char* mode_str = rx_mode == HitagSRxAC2K ? "AC2K" :
                                                     (rx_mode == HitagSRxMC2K ? "MC2K" : "MC4K");

    FURI_LOG_D(
        TAG,
        "RX: %d edges%s (mode=%s)",
        (int)hs_capture.edge_count,
        hs_capture.overflow ? " [OVERFLOW]" : "",
        mode_str);

    /* Log raw edges at DEBUG level (first 20) */
    size_t log_count = (hs_capture.edge_count < 20) ? hs_capture.edge_count : 20;
    for(size_t i = 0; i < log_count; i++) {
        FURI_LOG_D(
            TAG,
            "  e[%d]: %s %lu",
            (int)i,
            hs_capture.levels[i] ? "H" : "L",
            (unsigned long)hs_capture.durations[i]);
    }

    /* Trace: log ALL edges */
    if(g_trace_active) {
        trace_append(
            "  RX: %d edges%s mode=%s\n",
            (int)hs_capture.edge_count,
            hs_capture.overflow ? " [OVERFLOW]" : "",
            mode_str);
        trace_append("  EDGES:");
        for(size_t i = 0; i < hs_capture.edge_count; i++) {
            trace_append(
                " %s:%lu",
                hs_capture.levels[i] ? "H" : "L",
                (unsigned long)hs_capture.durations[i]);
        }
        trace_append("\n");
    }

    /* Decode using appropriate decoder */
    size_t bits;
    if(rx_mode == HitagSRxAC2K) {
        bits = hitag_s_decode_ac2k(&hs_capture, rx_data, rx_max_bits, sof_bits);
    } else {
        uint32_t threshold = (rx_mode == HitagSRxMC2K) ? 384 : HITAG_S_MC4K_THRESHOLD_US;
        bits = hitag_s_decode_mc4k(&hs_capture, rx_data, rx_max_bits, sof_bits, threshold);
    }

    /* Trace: log decode result */
    if(g_trace_active) {
        trace_append("  DECODE: %d bits", (int)bits);
        if(bits > 0) {
            size_t bytes = (bits + 7) / 8;
            trace_append(" =");
            for(size_t i = 0; i < bytes && i < 6; i++) {
                trace_append(" %02X", rx_data[i]);
            }
        }
        trace_append("\n");
    }

    return bits;
}

/* ============================================================
 * Field Control
 * ============================================================ */

void hitag_s_field_on(void) {
    furi_hal_rfid_tim_read_start(125000, 0.5f);
    /* Pull-down biases the antenna circuit for tag modulation detection.
     * The official LFRFID reader uses pulldown() — NOT pull_release()! */
    furi_hal_rfid_pin_pull_pulldown();
    furi_delay_us(HITAG_S_T_WAIT_POWERUP_US);
    FURI_LOG_D(TAG, "Field ON, carrier 125kHz");
}

void hitag_s_field_off(void) {
    furi_hal_rfid_tim_read_stop();
    furi_hal_rfid_pins_reset();
    FURI_LOG_D(TAG, "Field OFF");
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

/* --- Protocol mode tracking ---
 * The UID_REQ command determines the protocol mode for the session.
 * STD mode has simpler framing (SOF=1), ADV has longer SOF and CRC on responses.
 */
typedef struct {
    uint8_t cmd_5bit; /* 5-bit command value for pack_bits */
    const char* name;
    size_t uid_sof; /* SOF bits in AC2K UID response */
    size_t data_sof; /* SOF bits in MC4K data exchange */
} HitagSProtoMode;

static const HitagSProtoMode proto_modes[] = {
    {0x06, "STD", 1, 1}, /* UID_REQ_STD (00110): SOF=1 everywhere */
    {0x18, "ADV2", 3, 6}, /* UID_REQ_ADV2 (11000): SOF=3 for UID, 6 for data */
};
static size_t active_mode_idx = 0;

static inline size_t hitag_s_data_sof(void) {
    return proto_modes[active_mode_idx].data_sof;
}

/* Receive timeouts */
#define HITAG_S_RX_TIMEOUT_UID  25000 /* AC2K UID response (~18ms + margin) */
#define HITAG_S_RX_TIMEOUT_DATA 15000 /* MC4K 32-bit response (~10ms + margin) */
#define HITAG_S_RX_TIMEOUT_ACK  5000 /* MC4K ACK response (~2.5ms + margin) */

HitagSResult hitag_s_uid_request(uint32_t* uid) {
    trace_append("\n--- UID_REQUEST ---\n");
    /* Try STD mode first (simplest framing), then ADV2 (which we know works) */
    for(size_t c = 0; c < 2; c++) {
        uint8_t cmd[1] = {0};
        size_t bit_pos = 0;
        pack_bits(cmd, &bit_pos, proto_modes[c].cmd_5bit, 5);

        FURI_LOG_I(
            TAG,
            "TX: UID_REQ_%s (5 bits, val=0x%02X)",
            proto_modes[c].name,
            proto_modes[c].cmd_5bit);
        trace_append(
            "  TX: UID_REQ_%s (5 bits, val=0x%02X)\n",
            proto_modes[c].name,
            proto_modes[c].cmd_5bit);

        uint32_t prev_uid = 0;
        size_t stable_count = 0;
        bool had_decode = false;

        for(size_t attempt = 0; attempt < 6; attempt++) {
            uint8_t rx[4] = {0};
            trace_append("  attempt %d/%s:\n", (int)(attempt + 1), proto_modes[c].name);

            /* UID response is AC2K per Hitag S anti-collision; use MC2K as fallback
             * for clone variants that may answer with Manchester-like timing. */
            size_t rx_bits = hitag_s_send_receive(
                cmd, 5, rx, 32, HITAG_S_RX_TIMEOUT_UID, HitagSRxAC2K, proto_modes[c].uid_sof);

            if(rx_bits < 32) {
                rx_bits = hitag_s_send_receive(
                    cmd, 5, rx, 32, HITAG_S_RX_TIMEOUT_UID, HitagSRxMC2K, proto_modes[c].uid_sof);
            }

            if(rx_bits >= 32) {
                uint32_t current_uid = ((uint32_t)rx[0] << 24) | ((uint32_t)rx[1] << 16) |
                                       ((uint32_t)rx[2] << 8) | (uint32_t)rx[3];
                had_decode = true;

                if(stable_count == 0 || current_uid == prev_uid) {
                    stable_count++;
                } else {
                    stable_count = 1;
                }
                prev_uid = current_uid;

                FURI_LOG_D(
                    TAG,
                    "%s UID try %d: %08lX (stable=%d)",
                    proto_modes[c].name,
                    (int)(attempt + 1),
                    (unsigned long)current_uid,
                    (int)stable_count);

                if(stable_count >= 2) {
                    *uid = current_uid;
                    active_mode_idx = c;
                    FURI_LOG_I(
                        TAG,
                        "UID: %08lX (via %s mode, stable)",
                        (unsigned long)*uid,
                        proto_modes[c].name);
                    trace_append(
                        "  RESULT: OK, UID=%08lX (mode=%s)\n",
                        (unsigned long)*uid,
                        proto_modes[c].name);
                    return HitagSResultOk;
                }
            }

            furi_delay_us(HITAG_S_T_WAIT_SC_US);
        }

        if(had_decode) {
            FURI_LOG_W(TAG, "%s: UID decoded but unstable", proto_modes[c].name);
            trace_append("  %s: UID decoded but unstable\n", proto_modes[c].name);
        } else {
            FURI_LOG_W(TAG, "%s: no valid 32-bit UID response", proto_modes[c].name);
            trace_append("  %s: no valid UID response\n", proto_modes[c].name);
        }
        furi_delay_us(HITAG_S_T_WAIT_SC_US);
    }

    trace_append("  RESULT: TIMEOUT (no UID)\n");
    return HitagSResultTimeout;
}

HitagSResult hitag_s_select(uint32_t uid, uint32_t* config) {
    trace_append("\n--- SELECT ---\n");
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
    trace_append("  TX: SELECT UID=%08lX CRC=%02X (45 bits)\n", (unsigned long)uid, crc);

    furi_delay_us(HITAG_S_T_WAIT_SC_US);

    /* Combined send + receive — MC4K response with config page */
    uint8_t rx[5] = {0}; /* 32 config + possibly 8 CRC in ADV mode */
    size_t rx_bits = hitag_s_send_receive(
        cmd, 45, rx, 40, HITAG_S_RX_TIMEOUT_DATA, HitagSRxMC4K, hitag_s_data_sof());

    if(rx_bits < 32) {
        FURI_LOG_W(TAG, "SELECT: only %d bits received", (int)rx_bits);
        trace_append("  RESULT: TIMEOUT (%d bits)\n", (int)rx_bits);
        return HitagSResultTimeout;
    }

    /* In ADV mode, response includes 8-bit CRC — verify it */
    if(active_mode_idx == 1 && rx_bits >= 40) {
        uint8_t rx_crc = rx[4];
        uint8_t calc_crc = hitag_s_crc8(rx, 32);
        if(rx_crc != calc_crc) {
            FURI_LOG_W(TAG, "SELECT: CRC mismatch (rx=%02X calc=%02X)", rx_crc, calc_crc);
            trace_append("  RESULT: CRC ERROR (rx=%02X calc=%02X)\n", rx_crc, calc_crc);
            return HitagSResultCrcError;
        }
        FURI_LOG_D(TAG, "SELECT: ADV CRC OK (%02X)", rx_crc);
    }

    *config = ((uint32_t)rx[0] << 24) | ((uint32_t)rx[1] << 16) | ((uint32_t)rx[2] << 8) |
              (uint32_t)rx[3];

    FURI_LOG_I(TAG, "Config page: %08lX", (unsigned long)*config);
    trace_append("  RESULT: OK, Config=%08lX\n", (unsigned long)*config);
    return HitagSResultOk;
}

HitagSResult hitag_s_8268_authenticate(uint32_t password) {
    trace_append("\n--- AUTH (pwd=0x%08lX) ---\n", (unsigned long)password);
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
    trace_append("  TX: WRITE_PAGE addr=64 CRC=%02X (20 bits) [step 1]\n", crc);

    furi_delay_us(HITAG_S_T_WAIT_SC_US);

    /* Combined send + receive for ACK — MC4K */
    uint8_t ack[1] = {0};
    size_t ack_bits = hitag_s_send_receive(
        cmd, 20, ack, 8, HITAG_S_RX_TIMEOUT_ACK, HitagSRxMC4K, hitag_s_data_sof());

    if(ack_bits < 2) {
        FURI_LOG_W(TAG, "AUTH step 1: no ACK (%d bits)", (int)ack_bits);
        trace_append("  step1: no ACK (%d bits)\n", (int)ack_bits);
        return HitagSResultTimeout;
    }

    /* Check ACK (top 2 bits of ack[0] should be 01) */
    uint8_t ack_val = (ack[0] >> 6) & 0x03;
    if(ack_val != HITAG_S_ACK) {
        FURI_LOG_W(TAG, "AUTH step 1: NACK (got 0x%02X)", ack_val);
        trace_append("  step1: NACK (0x%02X)\n", ack_val);
        return HitagSResultNack;
    }
    trace_append("  step1: ACK OK\n");

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
    trace_append(
        "  TX: Password=%08lX CRC=%02X (40 bits) [step 2]\n", (unsigned long)password, pwd_crc);

    furi_delay_us(HITAG_S_T_WAIT_SC_US);

    /* Combined send + receive for ACK — MC4K */
    uint8_t ack2[1] = {0};
    size_t ack2_bits = hitag_s_send_receive(
        pwd_frame, 40, ack2, 8, HITAG_S_RX_TIMEOUT_ACK, HitagSRxMC4K, hitag_s_data_sof());

    if(ack2_bits < 2) {
        FURI_LOG_W(TAG, "AUTH step 2: no ACK (%d bits)", (int)ack2_bits);
        trace_append("  step2: no ACK (%d bits)\n", (int)ack2_bits);
        return HitagSResultTimeout;
    }

    uint8_t ack2_val = (ack2[0] >> 6) & 0x03;
    if(ack2_val != HITAG_S_ACK) {
        FURI_LOG_W(TAG, "AUTH step 2: NACK (got 0x%02X)", ack2_val);
        trace_append("  step2: NACK (0x%02X)\n", ack2_val);
        return HitagSResultNack;
    }

    FURI_LOG_I(TAG, "8268 authentication successful!");
    trace_append("  RESULT: AUTH OK\n");
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

    /* Combined send + receive for ACK — MC4K */
    uint8_t ack[1] = {0};
    size_t ack_bits = hitag_s_send_receive(
        cmd, 20, ack, 8, HITAG_S_RX_TIMEOUT_ACK, HitagSRxMC4K, hitag_s_data_sof());

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

    FURI_LOG_D(TAG, "TX: Data=%08lX CRC=%02X (40 bits)", (unsigned long)data, data_crc);

    furi_delay_us(HITAG_S_T_WAIT_SC_US);

    /* Combined send + receive — timeout includes programming time */
    uint8_t ack2[1] = {0};
    size_t ack2_bits = hitag_s_send_receive(
        data_frame,
        40,
        ack2,
        8,
        HITAG_S_T_PROG_US + HITAG_S_RX_TIMEOUT_ACK,
        HitagSRxMC4K,
        hitag_s_data_sof());

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
    trace_append("\n--- READ_PAGE %d ---\n", page);
    /* READ_PAGE = 1100 (4 bits) + addr (8 bits) + CRC8 (8 bits) = 20 bits */
    uint8_t cmd[3] = {0};
    size_t bit_pos = 0;

    pack_bits(cmd, &bit_pos, 0x0C, 4); /* 1100 */
    pack_bits(cmd, &bit_pos, page, 8);
    uint8_t crc = hitag_s_crc8(cmd, 12);
    pack_bits(cmd, &bit_pos, crc, 8);

    FURI_LOG_D(TAG, "TX: READ_PAGE addr=%d CRC=%02X (20 bits)", page, crc);

    furi_delay_us(HITAG_S_T_WAIT_SC_US);

    /* Combined send + receive — MC4K response with page data */
    uint8_t rx[5] = {0}; /* 32 data + possibly 8 CRC in ADV mode */
    size_t rx_bits = hitag_s_send_receive(
        cmd, 20, rx, 40, HITAG_S_RX_TIMEOUT_DATA, HitagSRxMC4K, hitag_s_data_sof());

    if(rx_bits < 32) {
        FURI_LOG_W(TAG, "READ_PAGE addr=%d: only %d bits received", page, (int)rx_bits);
        trace_append("  RESULT: TIMEOUT (%d bits)\n", (int)rx_bits);
        return HitagSResultTimeout;
    }

    /* In ADV mode, verify CRC */
    if(active_mode_idx == 1 && rx_bits >= 40) {
        uint8_t rx_crc = rx[4];
        uint8_t calc_crc = hitag_s_crc8(rx, 32);
        if(rx_crc != calc_crc) {
            FURI_LOG_W(
                TAG, "READ_PAGE addr=%d: CRC mismatch (rx=%02X calc=%02X)", page, rx_crc, calc_crc);
            trace_append("  RESULT: CRC ERROR (rx=%02X calc=%02X)\n", rx_crc, calc_crc);
            return HitagSResultCrcError;
        }
    }

    *data = ((uint32_t)rx[0] << 24) | ((uint32_t)rx[1] << 16) | ((uint32_t)rx[2] << 8) |
            (uint32_t)rx[3];

    FURI_LOG_I(TAG, "READ_PAGE addr=%d: %08lX", page, (unsigned long)*data);
    trace_append("  RESULT: OK, data=%08lX\n", (unsigned long)*data);
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
    if(password != 0) {
        result = hitag_s_8268_authenticate(password);
    } else {
        result = hitag_s_8268_authenticate_multi(NULL, 0);
    }
    if(result != HitagSResultOk) {
        FURI_LOG_E(TAG, "Write sequence: Authentication failed");
        hitag_s_field_off();
        return result;
    }

    /* Step 4: Write pages with verification */
    for(size_t i = 0; i < page_count; i++) {
        result = hitag_s_write_page_verify(page_addrs[i], pages[i]);
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

HitagSResult hitag_s_8268_write_em4100_sequence(
    uint32_t password,
    const Em4100HitagData* em_data,
    uint32_t* config_out) {
    uint32_t uid = 0;
    uint32_t config = 0;

    hitag_s_field_on();

    /* Step 1: UID request */
    HitagSResult result = hitag_s_uid_request(&uid);
    if(result != HitagSResultOk) {
        FURI_LOG_E(TAG, "EM4100 write: UID request failed");
        hitag_s_field_off();
        return result;
    }

    /* Step 2: SELECT — also gives us the current config page */
    result = hitag_s_select(uid, &config);
    if(result != HitagSResultOk) {
        FURI_LOG_E(TAG, "EM4100 write: SELECT failed");
        hitag_s_field_off();
        return result;
    }

    FURI_LOG_I(TAG, "EM4100 write: current config = %08lX", (unsigned long)config);

    /* Step 3: Authenticate with 82xx password (try multiple if needed) */
    if(password != 0) {
        result = hitag_s_8268_authenticate(password);
    } else {
        result = hitag_s_8268_authenticate_multi(NULL, 0);
    }
    if(result != HitagSResultOk) {
        FURI_LOG_E(TAG, "EM4100 write: Authentication failed");
        hitag_s_field_off();
        return result;
    }

    /* Step 4: Read config page after auth to get full current state */
    uint32_t current_config = 0;
    result = hitag_s_read_page(1, &current_config);
    if(result != HitagSResultOk) {
        FURI_LOG_W(TAG, "EM4100 write: Can't read config, using SELECT value");
        current_config = config;
    } else {
        FURI_LOG_I(TAG, "EM4100 write: read config = %08lX", (unsigned long)current_config);
    }

    /* Check lock bits before writing */
    HitagSConfig cfg = hitag_s_parse_config(current_config);
    if(hitag_s_page_locked(&cfg, 4) || hitag_s_page_locked(&cfg, 5)) {
        FURI_LOG_E(TAG, "EM4100 write: data pages 4/5 locked!");
        hitag_s_field_off();
        return HitagSResultError;
    }

    /* Step 5: Modify config for EM4100 TTF (read-modify-write) */
    uint32_t new_config = em4100_config_set_ttf(current_config);
    if(config_out) *config_out = new_config;

    /* Step 6: Write EM4100 data to pages 4 and 5 FIRST (before config change) */
    result = hitag_s_write_page_verify(4, em_data->data_hi);
    if(result != HitagSResultOk) {
        FURI_LOG_E(TAG, "EM4100 write: Write page 4 failed");
        hitag_s_field_off();
        return result;
    }

    result = hitag_s_write_page_verify(5, em_data->data_lo);
    if(result != HitagSResultOk) {
        FURI_LOG_E(TAG, "EM4100 write: Write page 5 failed");
        hitag_s_field_off();
        return result;
    }

    /* Step 7: Write modified config to page 1 (last, since it enables TTF) */
    result = hitag_s_write_page(1, new_config);
    if(result != HitagSResultOk) {
        FURI_LOG_E(TAG, "EM4100 write: Write config page failed");
        hitag_s_field_off();
        return result;
    }

    FURI_LOG_I(
        TAG,
        "EM4100 write: SUCCESS! Config=%08lX Data=%08lX %08lX",
        (unsigned long)new_config,
        (unsigned long)em_data->data_hi,
        (unsigned long)em_data->data_lo);

    hitag_s_field_off();
    return HitagSResultOk;
}

HitagSResult hitag_s_8268_read_sequence(
    uint32_t password,
    uint32_t* pages,
    const uint8_t* page_addrs,
    size_t page_count,
    uint32_t* uid_out) {
    uint32_t uid = 0;
    uint32_t config = 0;

    hitag_s_field_on();

    /* Step 1: UID request */
    HitagSResult result = hitag_s_uid_request(&uid);
    if(result != HitagSResultOk) {
        FURI_LOG_E(TAG, "Read sequence: UID request failed");
        hitag_s_field_off();
        return result;
    }

    if(uid_out) *uid_out = uid;

    /* Step 2: SELECT */
    result = hitag_s_select(uid, &config);
    if(result != HitagSResultOk) {
        FURI_LOG_E(TAG, "Read sequence: SELECT failed");
        hitag_s_field_off();
        return result;
    }

    /* Step 3: Authenticate */
    if(password != 0) {
        result = hitag_s_8268_authenticate(password);
    } else {
        result = hitag_s_8268_authenticate_multi(NULL, 0);
    }
    if(result != HitagSResultOk) {
        FURI_LOG_E(TAG, "Read sequence: Authentication failed");
        hitag_s_field_off();
        return result;
    }

    /* Step 4: Read pages */
    for(size_t i = 0; i < page_count; i++) {
        result = hitag_s_read_page(page_addrs[i], &pages[i]);
        if(result != HitagSResultOk) {
            FURI_LOG_E(TAG, "Read sequence: Read page %d failed", page_addrs[i]);
            hitag_s_field_off();
            return result;
        }
    }

    FURI_LOG_I(TAG, "Read sequence: All %d pages read successfully!", (int)page_count);
    hitag_s_field_off();
    return HitagSResultOk;
}

/* ============================================================
 * Write with Readback Verification (PM3-inspired)
 * ============================================================ */

HitagSResult hitag_s_write_page_verify(uint8_t page, uint32_t data) {
    /* Write the page */
    HitagSResult result = hitag_s_write_page(page, data);
    if(result != HitagSResultOk) {
        return result;
    }

    /* Read back for verification */
    uint32_t readback = 0;
    result = hitag_s_read_page(page, &readback);
    if(result != HitagSResultOk) {
        FURI_LOG_W(TAG, "VERIFY page %d: readback failed", page);
        return result;
    }

    /* For config page (page 1), PWDH0 byte is masked (returns 0xFF in plain mode) */
    uint32_t mask = 0xFFFFFFFF;
    if(page == 1) {
        mask = 0xFFFFFF00; /* Ignore PWDH0 byte */
    }

    if((readback & mask) != (data & mask)) {
        FURI_LOG_E(
            TAG,
            "VERIFY page %d: MISMATCH wrote=%08lX read=%08lX",
            page,
            (unsigned long)data,
            (unsigned long)readback);
        return HitagSResultError;
    }

    FURI_LOG_I(TAG, "VERIFY page %d: OK (%08lX)", page, (unsigned long)readback);
    return HitagSResultOk;
}

/* ============================================================
 * Multi-Password Authentication (PM3-inspired)
 * ============================================================ */

HitagSResult hitag_s_8268_authenticate_multi(const uint32_t* passwords, size_t count) {
    /* Default passwords to try if none provided */
    static const uint32_t default_passwords[] = {
        HITAG_S_8268_PASSWORD, /* 0xBBDD3399 — standard 8268 factory default */
        HITAG_S_8268_PASSWORD_ALT1, /* 0x4D494B52 — "MIKR" HiTag 2 default */
        HITAG_S_8268_PASSWORD_ALT2, /* 0xAAAAAAAA — common alternate */
        HITAG_S_8268_PASSWORD_ALT3, /* 0x00000000 — all zeros */
        HITAG_S_8268_PASSWORD_ALT4, /* 0xFFFFFFFF — all ones */
    };

    const uint32_t* pwd_list = passwords;
    size_t pwd_count = count;

    if(pwd_list == NULL || pwd_count == 0) {
        pwd_list = default_passwords;
        pwd_count = sizeof(default_passwords) / sizeof(default_passwords[0]);
    }

    for(size_t i = 0; i < pwd_count; i++) {
        FURI_LOG_I(
            TAG,
            "Auth attempt %d/%d with password %08lX",
            (int)(i + 1),
            (int)pwd_count,
            (unsigned long)pwd_list[i]);

        HitagSResult result = hitag_s_8268_authenticate(pwd_list[i]);
        if(result == HitagSResultOk) {
            FURI_LOG_I(TAG, "Auth OK with password %08lX", (unsigned long)pwd_list[i]);
            return HitagSResultOk;
        }

        /* If NACK, try next password. If timeout, tag may be gone. */
        if(result == HitagSResultTimeout) {
            FURI_LOG_W(TAG, "Auth timeout — tag may be out of range");
            return HitagSResultTimeout;
        }

        FURI_LOG_D(TAG, "Password %08lX rejected, trying next...", (unsigned long)pwd_list[i]);
    }

    FURI_LOG_E(TAG, "All %d passwords rejected", (int)pwd_count);
    return HitagSResultNack;
}

/* ============================================================
 * Page Lock Check
 * ============================================================ */

bool hitag_s_page_writable(uint32_t config_val, uint8_t page) {
    HitagSConfig cfg = hitag_s_parse_config(config_val);

    /* 82xx: page 0 (UID) is writable via magic command */
    if(page == 0) return true;

    /* Page 1 (config) — writable unless LCON is set */
    if(page == 1) return !cfg.LCON;

    /* Pages 2-3 (password/key) — writable unless LKP is set */
    if(page == 2 || page == 3) return !cfg.LKP;

    /* Pages 4-63 — check individual lock bits */
    return !hitag_s_page_locked(&cfg, page);
}

/* ============================================================
 * Write UID (page 0) — 8268 magic chip feature
 * ============================================================ */

HitagSResult hitag_s_write_uid(uint32_t new_uid) {
    FURI_LOG_I(TAG, "Writing UID page 0: %08lX", (unsigned long)new_uid);

    /* 8268 magic chips allow writing page 0 (UID)
     * This is the key difference from normal Hitag S tags */
    HitagSResult result = hitag_s_write_page(0, new_uid);
    if(result != HitagSResultOk) {
        FURI_LOG_E(TAG, "Write UID failed");
        return result;
    }

    /* Verify by reading back — but note we can't read page 0 directly
     * after write in the same session. The UID will change on next
     * power cycle, so we just trust the write ACK. */
    FURI_LOG_I(TAG, "UID write command sent successfully");
    return HitagSResultOk;
}

/* ============================================================
 * Full Tag Dump (read all pages)
 * ============================================================ */

HitagSResult hitag_s_8268_read_all(
    uint32_t password,
    uint32_t* pages,
    bool* page_valid,
    int* max_page_out,
    uint32_t* uid_out) {
    uint32_t uid = 0;
    uint32_t config = 0;

    hitag_s_field_on();

    /* Step 1: UID request */
    HitagSResult result = hitag_s_uid_request(&uid);
    if(result != HitagSResultOk) {
        FURI_LOG_E(TAG, "Read all: UID request failed");
        hitag_s_field_off();
        return result;
    }

    if(uid_out) *uid_out = uid;
    pages[0] = uid;
    page_valid[0] = true;

    /* Step 2: SELECT */
    result = hitag_s_select(uid, &config);
    if(result != HitagSResultOk) {
        FURI_LOG_E(TAG, "Read all: SELECT failed");
        hitag_s_field_off();
        return result;
    }

    /* Parse config to determine max page */
    HitagSConfig cfg = hitag_s_parse_config(config);
    int max_pg = hitag_s_max_page(&cfg);
    if(max_page_out) *max_page_out = max_pg;

    FURI_LOG_I(TAG, "Read all: MEMT=%d, max_page=%d, auth=%d", cfg.MEMT, max_pg, cfg.auth);

    /* Step 3: Authenticate with 82xx password */
    if(password != 0) {
        result = hitag_s_8268_authenticate(password);
    } else {
        result = hitag_s_8268_authenticate_multi(NULL, 0);
    }
    if(result != HitagSResultOk) {
        FURI_LOG_E(TAG, "Read all: Authentication failed");
        hitag_s_field_off();
        return result;
    }

    /* Step 4: Read config page (page 1) properly after auth */
    result = hitag_s_read_page(1, &pages[1]);
    if(result == HitagSResultOk) {
        page_valid[1] = true;
        /* Re-parse config with full data (after auth, PWDH0 may be readable) */
        config = pages[1];
        cfg = hitag_s_parse_config(config);
    } else {
        pages[1] = config; /* Use value from SELECT */
        page_valid[1] = true;
    }

    /* Step 5: Read pages 2 and 3 (password/key) — may be protected */
    for(uint8_t p = 2; p <= 3; p++) {
        if(cfg.auth && cfg.LKP) {
            /* LKP=1 + auth mode: pages 2/3 not accessible */
            FURI_LOG_D(TAG, "Read all: page %d protected (LKP+auth), skipping", p);
            page_valid[p] = false;
            continue;
        }
        result = hitag_s_read_page(p, &pages[p]);
        page_valid[p] = (result == HitagSResultOk);
        if(!page_valid[p]) {
            FURI_LOG_D(TAG, "Read all: page %d read failed", p);
        }
    }

    /* Step 6: Read data pages 4..max_page */
    for(int p = 4; p <= max_pg; p++) {
        result = hitag_s_read_page((uint8_t)p, &pages[p]);
        page_valid[p] = (result == HitagSResultOk);
        if(!page_valid[p]) {
            FURI_LOG_D(TAG, "Read all: page %d read failed", p);
        }
    }

    /* Mark remaining pages as invalid */
    for(int p = max_pg + 1; p < HITAG_S_MAX_PAGES; p++) {
        page_valid[p] = false;
    }

    /* Count successfully read pages */
    int read_count = 0;
    for(int p = 0; p <= max_pg; p++) {
        if(page_valid[p]) read_count++;
    }

    FURI_LOG_I(TAG, "Read all: %d/%d pages read", read_count, max_pg + 1);
    hitag_s_field_off();
    return HitagSResultOk;
}

/* ============================================================
 * Full Clone Sequence (UID + config + data)
 * ============================================================ */

HitagSResult hitag_s_8268_clone_sequence(
    uint32_t password,
    uint32_t new_uid,
    uint32_t config,
    const uint32_t* data_pages,
    const uint8_t* data_addrs,
    size_t data_count) {
    uint32_t uid = 0;
    uint32_t current_config = 0;

    hitag_s_field_on();

    /* Step 1: UID request (get current UID of target tag) */
    HitagSResult result = hitag_s_uid_request(&uid);
    if(result != HitagSResultOk) {
        FURI_LOG_E(TAG, "Clone: UID request failed");
        hitag_s_field_off();
        return result;
    }

    FURI_LOG_I(
        TAG,
        "Clone: target current UID=%08lX, will write new UID=%08lX",
        (unsigned long)uid,
        (unsigned long)new_uid);

    /* Step 2: SELECT */
    result = hitag_s_select(uid, &current_config);
    if(result != HitagSResultOk) {
        FURI_LOG_E(TAG, "Clone: SELECT failed");
        hitag_s_field_off();
        return result;
    }

    /* Step 3: Authenticate */
    if(password != 0) {
        result = hitag_s_8268_authenticate(password);
    } else {
        result = hitag_s_8268_authenticate_multi(NULL, 0);
    }
    if(result != HitagSResultOk) {
        FURI_LOG_E(TAG, "Clone: Authentication failed");
        hitag_s_field_off();
        return result;
    }

    /* Step 4: Write data pages FIRST (before changing config which may lock them) */
    for(size_t i = 0; i < data_count; i++) {
        if(!hitag_s_page_writable(current_config, data_addrs[i])) {
            FURI_LOG_W(TAG, "Clone: page %d locked, skipping", data_addrs[i]);
            continue;
        }

        result = hitag_s_write_page(data_addrs[i], data_pages[i]);
        if(result != HitagSResultOk) {
            FURI_LOG_E(TAG, "Clone: Write page %d failed", data_addrs[i]);
            hitag_s_field_off();
            return result;
        }
        FURI_LOG_D(TAG, "Clone: page %d = %08lX OK", data_addrs[i], (unsigned long)data_pages[i]);
    }

    /* Step 5: Write config page (page 1) */
    result = hitag_s_write_page(1, config);
    if(result != HitagSResultOk) {
        FURI_LOG_E(TAG, "Clone: Write config failed");
        hitag_s_field_off();
        return result;
    }
    FURI_LOG_I(TAG, "Clone: config=%08lX written", (unsigned long)config);

    /* Step 6: Write UID (page 0) — 82xx magic! Do this LAST since
     * the tag will have a different UID after next power cycle */
    result = hitag_s_write_uid(new_uid);
    if(result != HitagSResultOk) {
        FURI_LOG_E(TAG, "Clone: Write UID failed");
        hitag_s_field_off();
        return result;
    }

    FURI_LOG_I(
        TAG,
        "Clone: SUCCESS! UID=%08lX config=%08lX + %d data pages",
        (unsigned long)new_uid,
        (unsigned long)config,
        (int)data_count);
    hitag_s_field_off();
    return HitagSResultOk;
}

/* ============================================================
 * Wipe Tag — reset 8268 to factory-like defaults
 * ============================================================ */

/** Default config for wiped 8268: MEMT=11(64pg), no auth, no locks */
#define HITAG_S_WIPE_CONFIG 0x06000000UL
/* CON0=0x06: MEMT=11 (bits 7:6 = 0b11 → 0x06 with other bits 0)
 * Actually: packed byte[0] has MEMT in bits[1:0], so MEMT=11 = 0b00000011
 * Let me compute correctly using the struct... */

HitagSResult hitag_s_8268_wipe_sequence(uint32_t password, int max_page, int* pages_wiped) {
    uint32_t uid = 0;
    uint32_t current_config = 0;
    int wiped = 0;

    hitag_s_field_on();

    /* Step 1: UID request */
    HitagSResult result = hitag_s_uid_request(&uid);
    if(result != HitagSResultOk) {
        FURI_LOG_E(TAG, "Wipe: UID request failed");
        hitag_s_field_off();
        return result;
    }

    FURI_LOG_I(TAG, "Wipe: tag UID=%08lX", (unsigned long)uid);

    /* Step 2: SELECT */
    result = hitag_s_select(uid, &current_config);
    if(result != HitagSResultOk) {
        FURI_LOG_E(TAG, "Wipe: SELECT failed");
        hitag_s_field_off();
        return result;
    }

    /* Step 3: Authenticate */
    if(password != 0) {
        result = hitag_s_8268_authenticate(password);
    } else {
        result = hitag_s_8268_authenticate_multi(NULL, 0);
    }
    if(result != HitagSResultOk) {
        FURI_LOG_E(TAG, "Wipe: Authentication failed");
        hitag_s_field_off();
        return result;
    }

    /* Determine max page from current config if not specified */
    if(max_page <= 0) {
        HitagSConfig cfg = hitag_s_parse_config(current_config);
        max_page = hitag_s_max_page(&cfg);
    }

    FURI_LOG_I(TAG, "Wipe: clearing pages 4..%d", max_page);

    /* Step 4: Clear all data pages (4+) to 0x00000000 */
    for(int p = 4; p <= max_page; p++) {
        result = hitag_s_write_page((uint8_t)p, 0x00000000UL);
        if(result != HitagSResultOk) {
            FURI_LOG_W(TAG, "Wipe: page %d write failed, continuing", p);
            /* Don't abort — try remaining pages */
        } else {
            wiped++;
        }
    }

    /* Step 5: Reset password pages to defaults */
    /* Page 2 = password low 24 bits + PWDH0: write default password 0xBBDD3399 */
    result = hitag_s_write_page(2, HITAG_S_8268_PASSWORD);
    if(result == HitagSResultOk) {
        wiped++;
        FURI_LOG_I(TAG, "Wipe: password page 2 reset to default");
    }

    /* Page 3 = key page, clear to 0 */
    result = hitag_s_write_page(3, 0x00000000UL);
    if(result == HitagSResultOk) {
        wiped++;
        FURI_LOG_I(TAG, "Wipe: key page 3 cleared");
    }

    /* Step 6: Reset config page (page 1) — LAST before UID
     * Build a clean default config:
     *   MEMT=11 (64 pages), no auth, no locks, no TTF, plain mode */
    HitagSConfig clean_cfg;
    memset(&clean_cfg, 0, sizeof(clean_cfg));
    clean_cfg.MEMT = 3; /* 11 = 64 pages */
    clean_cfg.pwdh0 = (HITAG_S_8268_PASSWORD >> 24) & 0xFF;
    uint32_t clean_config = hitag_s_pack_config(&clean_cfg);

    result = hitag_s_write_page(1, clean_config);
    if(result == HitagSResultOk) {
        wiped++;
        FURI_LOG_I(TAG, "Wipe: config reset to %08lX", (unsigned long)clean_config);
    } else {
        FURI_LOG_E(TAG, "Wipe: config write failed!");
    }

    if(pages_wiped) *pages_wiped = wiped;

    FURI_LOG_I(TAG, "Wipe: done, %d pages wiped", wiped);
    hitag_s_field_off();
    return HitagSResultOk; /* Return OK even if some pages failed — partial wipe is useful */
}

/* ============================================================
 * Debug Read Sequence — full read with RF trace capture
 * ============================================================ */

HitagSResult hitag_s_debug_read_sequence(
    uint32_t* uid_out,
    uint32_t* config_out,
    uint32_t* pages,
    bool* page_valid,
    int* max_page) {
    uint32_t uid = 0;
    uint32_t config = 0;

    /* Trace is already started by caller (hitag_s_debug_trace_start) */
    trace_append("\n=== DEBUG READ SEQUENCE ===\n");

    hitag_s_field_on();
    trace_append("Field ON (125kHz)\n");

    /* Step 1: UID request */
    HitagSResult result = hitag_s_uid_request(&uid);
    if(result != HitagSResultOk) {
        trace_append("ABORT: UID request failed (result=%d)\n", (int)result);
        hitag_s_field_off();
        return result;
    }

    if(uid_out) *uid_out = uid;
    pages[0] = uid;
    page_valid[0] = true;

    /* Step 2: SELECT */
    result = hitag_s_select(uid, &config);
    if(result != HitagSResultOk) {
        trace_append("ABORT: SELECT failed (result=%d)\n", (int)result);
        hitag_s_field_off();
        return result;
    }

    if(config_out) *config_out = config;

    /* Parse config */
    HitagSConfig cfg = hitag_s_parse_config(config);
    int max_pg = hitag_s_max_page(&cfg);
    if(max_page) *max_page = max_pg;

    trace_append(
        "Config: MEMT=%d max_page=%d auth=%d LKP=%d LCON=%d\n",
        cfg.MEMT,
        max_pg,
        cfg.auth,
        cfg.LKP,
        cfg.LCON);
    trace_append("Config: TTFC=%d TTFDR=%d TTFM=%d\n", cfg.TTFC, cfg.TTFDR, cfg.TTFM);

    /* Step 3: Authenticate with multi-password */
    trace_append("\n--- AUTH MULTI ---\n");
    result = hitag_s_8268_authenticate_multi(NULL, 0);
    if(result != HitagSResultOk) {
        trace_append("ABORT: AUTH failed (result=%d)\n", (int)result);
        hitag_s_field_off();
        return result;
    }

    /* Step 4: Read config page after auth */
    result = hitag_s_read_page(1, &pages[1]);
    if(result == HitagSResultOk) {
        page_valid[1] = true;
        config = pages[1];
        cfg = hitag_s_parse_config(config);
    } else {
        pages[1] = config;
        page_valid[1] = true;
        trace_append("  (using config from SELECT)\n");
    }

    /* Step 5: Read password pages */
    for(uint8_t p = 2; p <= 3; p++) {
        result = hitag_s_read_page(p, &pages[p]);
        page_valid[p] = (result == HitagSResultOk);
    }

    /* Step 6: Read all data pages */
    for(int p = 4; p <= max_pg; p++) {
        result = hitag_s_read_page((uint8_t)p, &pages[p]);
        page_valid[p] = (result == HitagSResultOk);
    }

    /* Mark remaining as invalid */
    for(int p = max_pg + 1; p < HITAG_S_MAX_PAGES; p++) {
        page_valid[p] = false;
    }

    /* Summary */
    int read_count = 0;
    for(int p = 0; p <= max_pg; p++) {
        if(page_valid[p]) read_count++;
    }
    trace_append(
        "\n=== SUMMARY: %d/%d pages read, UID=%08lX ===\n",
        read_count,
        max_pg + 1,
        (unsigned long)uid);

    /* Append page table */
    trace_append("\nPAGE TABLE:\n");
    for(int p = 0; p <= max_pg; p++) {
        if(page_valid[p]) {
            trace_append("  [%2d] %08lX\n", p, (unsigned long)pages[p]);
        } else {
            trace_append("  [%2d] --------\n", p);
        }
    }

    hitag_s_field_off();
    trace_append("Field OFF\n");
    return HitagSResultOk;
}

/* ============================================================
 * Dump File Save / Load
 * ============================================================ */

#define HITAGS_DUMP_FILETYPE "HiTag S 8268 Dump"
#define HITAGS_DUMP_VERSION  1

bool hitag_s_dump_save(
    void* storage_ptr,
    const char* path,
    uint32_t uid,
    const uint32_t* pages,
    const bool* page_valid,
    int max_page) {
    Storage* storage = (Storage*)storage_ptr;
    FlipperFormat* file = flipper_format_file_alloc(storage);
    bool result = false;

    do {
        if(!flipper_format_file_open_always(file, path)) {
            FURI_LOG_E(TAG, "Dump save: can't open %s", path);
            break;
        }

        /* Header */
        if(!flipper_format_write_header_cstr(file, HITAGS_DUMP_FILETYPE, HITAGS_DUMP_VERSION)) {
            break;
        }

        /* UID as 4 hex bytes */
        uint8_t uid_bytes[4] = {
            (uid >> 24) & 0xFF, (uid >> 16) & 0xFF, (uid >> 8) & 0xFF, uid & 0xFF};
        if(!flipper_format_write_hex(file, "UID", uid_bytes, 4)) break;

        /* Max page as uint32 */
        uint32_t mp = (uint32_t)max_page;
        if(!flipper_format_write_uint32(file, "Max Page", &mp, 1)) break;

        /* Config summary as comment */
        if(page_valid[1]) {
            HitagSConfig cfg = hitag_s_parse_config(pages[1]);
            char comment[64];
            snprintf(
                comment,
                sizeof(comment),
                "MEMT=%d auth=%d LKP=%d LCON=%d TTFC=%d TTFDR=%d TTFM=%d",
                cfg.MEMT,
                cfg.auth,
                cfg.LKP,
                cfg.LCON,
                cfg.TTFC,
                cfg.TTFDR,
                cfg.TTFM);
            flipper_format_write_comment_cstr(file, comment);
        }

        /* Each page as "Page N: XX XX XX XX" */
        bool write_ok = true;
        for(int p = 0; p <= max_page; p++) {
            char key[16];
            snprintf(key, sizeof(key), "Page %d", p);

            if(page_valid[p]) {
                uint8_t page_bytes[4] = {
                    (pages[p] >> 24) & 0xFF,
                    (pages[p] >> 16) & 0xFF,
                    (pages[p] >> 8) & 0xFF,
                    pages[p] & 0xFF};
                if(!flipper_format_write_hex(file, key, page_bytes, 4)) {
                    write_ok = false;
                    break;
                }
            } else {
                /* Write placeholder for unreadable pages */
                uint8_t empty[4] = {0, 0, 0, 0};
                if(!flipper_format_write_hex(file, key, empty, 4)) {
                    write_ok = false;
                    break;
                }
            }
        }
        if(!write_ok) break;

        result = true;
        FURI_LOG_I(TAG, "Dump saved to %s (%d pages)", path, max_page + 1);
    } while(false);

    flipper_format_free(file);
    return result;
}

bool hitag_s_dump_load(
    void* storage_ptr,
    const char* path,
    uint32_t* uid,
    uint32_t* pages,
    bool* page_valid,
    int* max_page) {
    Storage* storage = (Storage*)storage_ptr;
    FlipperFormat* file = flipper_format_file_alloc(storage);
    bool result = false;

    /* Clear output */
    memset(pages, 0, HITAG_S_MAX_PAGES * sizeof(uint32_t));
    memset(page_valid, 0, HITAG_S_MAX_PAGES * sizeof(bool));
    *uid = 0;
    *max_page = 0;

    do {
        if(!flipper_format_file_open_existing(file, path)) {
            FURI_LOG_E(TAG, "Dump load: can't open %s", path);
            break;
        }

        /* Verify header */
        uint32_t version = 0;
        FuriString* filetype = furi_string_alloc();
        if(!flipper_format_read_header(file, filetype, &version)) {
            furi_string_free(filetype);
            break;
        }
        if(furi_string_cmp_str(filetype, HITAGS_DUMP_FILETYPE) != 0 ||
           version != HITAGS_DUMP_VERSION) {
            FURI_LOG_E(TAG, "Dump load: wrong filetype or version");
            furi_string_free(filetype);
            break;
        }
        furi_string_free(filetype);

        /* Read UID */
        uint8_t uid_bytes[4] = {0};
        if(!flipper_format_read_hex(file, "UID", uid_bytes, 4)) break;
        *uid = ((uint32_t)uid_bytes[0] << 24) | ((uint32_t)uid_bytes[1] << 16) |
               ((uint32_t)uid_bytes[2] << 8) | (uint32_t)uid_bytes[3];
        pages[0] = *uid;
        page_valid[0] = true;

        /* Read max page */
        uint32_t mp = 0;
        if(!flipper_format_read_uint32(file, "Max Page", &mp, 1)) break;
        *max_page = (int)mp;

        /* Read pages */
        for(int p = 0; p <= *max_page; p++) {
            char key[16];
            snprintf(key, sizeof(key), "Page %d", p);

            uint8_t page_bytes[4] = {0};
            if(flipper_format_read_hex(file, key, page_bytes, 4)) {
                pages[p] = ((uint32_t)page_bytes[0] << 24) | ((uint32_t)page_bytes[1] << 16) |
                           ((uint32_t)page_bytes[2] << 8) | (uint32_t)page_bytes[3];
                page_valid[p] = true;
            }
        }

        result = true;
        FURI_LOG_I(
            TAG,
            "Dump loaded from %s: UID=%08lX max_page=%d",
            path,
            (unsigned long)*uid,
            *max_page);
    } while(false);

    flipper_format_free(file);
    return result;
}
