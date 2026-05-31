/**
 * @file hitag_s_session.c
 * @brief Hitag S session command implementation for 8268 magic chips
 *
 * Uses furi_hal_rfid HAL APIs to generate BPLM encoded commands and
 * receive Manchester encoded responses from Hitag S tags.
 */

#include "hitag_s_proto.h"
#include "hitag_s_codec.h"
#include "hitag_s_trace.h"
#include <furi.h>
#include <furi_hal.h>

#define TAG "HitagS"

/** Append formatted text to trace buffer (if tracing is active) */
static void trace_append(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    hitag_s_trace_vappend(fmt, args);
    va_end(args);
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
 * AC2K encodes bits as tag load modulation patterns. The TIM2 rising-to-rising
 * periods (level=false captures) classify as:
 *   TWO_HALF   (~256µs) : within a '1' bit
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

static HitagSAc2kQuality hitag_s_capture_ac2k_quality(const HitagSCapture* cap) {
    HitagSAc2kQuality quality = {0};
    size_t edge_count = cap->edge_count < HITAG_S_MAX_EDGES ? cap->edge_count : HITAG_S_MAX_EDGES;

    for(size_t i = 0; i < edge_count; i++) {
        hitag_s_codec_ac2k_quality_add(&quality, cap->levels[i], cap->durations[i]);
    }

    return quality;
}

static bool hitag_s_capture_has_excessive_glitches(const HitagSCapture* cap) {
    HitagSAc2kQuality quality = hitag_s_capture_ac2k_quality(cap);
    return quality.too_noisy;
}

static bool hitag_s_capture_is_marginal_uid_candidate(const HitagSCapture* cap) {
    HitagSAc2kQuality quality = hitag_s_capture_ac2k_quality(cap);
    return hitag_s_codec_is_marginal_ac2k_uid_quality(32, &quality);
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

    /* Wait for tag response edges, then return as soon as the response goes idle.
     * HiTag S expects the next reader command in a short inter-frame window; waiting
     * the whole RX timeout after a complete UID response makes SELECT arrive late. */
    uint32_t elapsed_us = 0;
    uint32_t idle_us = 0;
    size_t last_edge_count = 0;
    while(elapsed_us < rx_timeout_us) {
        const uint32_t step_us = 100;
        furi_delay_us(step_us);
        elapsed_us += step_us;

        size_t edge_count = hs_capture.edge_count;
        if(edge_count != last_edge_count) {
            last_edge_count = edge_count;
            idle_us = 0;
        } else if(edge_count > 1) {
            idle_us += step_us;
            if(idle_us >= HITAG_S_T_RX_IDLE_US) break;
        }
    }

    /* Stop capture */
    furi_hal_rfid_tim_read_capture_stop();

    trace_append(
        "  RX_META: elapsed_us=%lu idle_us=%lu timeout_us=%lu final_edges=%d\n",
        (unsigned long)elapsed_us,
        (unsigned long)idle_us,
        (unsigned long)rx_timeout_us,
        (int)hs_capture.edge_count);

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
    if(hitag_s_trace_is_active()) {
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
    if(hitag_s_trace_is_active()) {
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
 * Hitag S Command Builders
 * ============================================================ */

/**
 * Pack bits into byte array (MSB first).
 * Helper to build command frames.
 */
static void pack_bits(uint8_t* buf, size_t* bit_pos, uint32_t value, size_t n_bits) {
    hitag_s_codec_pack_bits(buf, bit_pos, value, n_bits);
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
    bool response_crc; /* Advanced modes append CRC to data responses */
} HitagSProtoMode;

static const HitagSProtoMode proto_modes[] = {
    {0x06, "STD", 0, 1, false}, /* UID_REQ_STD (00110): UID response has no extra SOF bit */
    {0x19, "ADV1", 0, 6, true}, /* UID response uses raw 32-bit AC2K UID */
    {0x18, "ADV2", 0, 6, true}, /* UID response uses raw 32-bit AC2K UID */
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
    /* Try Proxmark's 8268 write mode first, then fall back to other observed modes. */
    for(size_t c = 0; c < COUNT_OF(proto_modes); c++) {
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
        trace_append(
            "  TX_FRAME: frame=%02X bits=%d tx_us=%lu\n",
            cmd[0],
            (int)bit_pos,
            (unsigned long)hitag_s_codec_bplm_frame_duration_us(cmd, bit_pos));

        bool had_decode = false;
        bool marginal_uid_valid = false;
        uint32_t marginal_uid = 0;

        for(size_t attempt = 0; attempt < 6; attempt++) {
            uint8_t rx[4] = {0};
            trace_append("  attempt %d/%s:\n", (int)(attempt + 1), proto_modes[c].name);

            /* UID response is AC2K per Hitag S anti-collision. MC2K-looking captures on
             * Flipper can be stable noise, so do not use them as UID truth. */
            size_t rx_bits = hitag_s_send_receive(
                cmd, 5, rx, 32, HITAG_S_RX_TIMEOUT_UID, HitagSRxAC2K, proto_modes[c].uid_sof);

            if(rx_bits == 32) {
                uint32_t current_uid = ((uint32_t)rx[0] << 24) | ((uint32_t)rx[1] << 16) |
                                       ((uint32_t)rx[2] << 8) | (uint32_t)rx[3];
                had_decode = true;

                if(hitag_s_capture_has_excessive_glitches(&hs_capture)) {
                    if(hitag_s_capture_is_marginal_uid_candidate(&hs_capture)) {
                        marginal_uid = current_uid;
                        marginal_uid_valid = true;
                        FURI_LOG_W(
                            TAG,
                            "%s UID try %d: keeping marginal noisy AC2K UID %08lX",
                            proto_modes[c].name,
                            (int)(attempt + 1),
                            (unsigned long)marginal_uid);
                        trace_append(
                            "  %s: keeping marginal noisy AC2K UID=%08lX\n",
                            proto_modes[c].name,
                            (unsigned long)marginal_uid);
                    } else {
                        FURI_LOG_W(
                            TAG,
                            "%s UID try %d: rejected noisy AC2K capture",
                            proto_modes[c].name,
                            (int)(attempt + 1));
                        trace_append("  %s: rejected noisy AC2K capture\n", proto_modes[c].name);
                    }
                    furi_delay_us(HITAG_S_T_WAIT_SC_US);
                    continue;
                }

                *uid = current_uid;
                active_mode_idx = c;
                FURI_LOG_I(
                    TAG,
                    "UID: %08lX (via %s mode, AC2K)",
                    (unsigned long)*uid,
                    proto_modes[c].name);
                trace_append(
                    "  RESULT: OK, UID=%08lX (mode=%s, AC2K)\n",
                    (unsigned long)*uid,
                    proto_modes[c].name);
                return HitagSResultOk;
            } else if(rx_bits > 0) {
                had_decode = true;
                if(hitag_s_capture_has_excessive_glitches(&hs_capture)) {
                    FURI_LOG_W(
                        TAG,
                        "%s UID try %d: rejected noisy partial AC2K capture (%d bits)",
                        proto_modes[c].name,
                        (int)(attempt + 1),
                        (int)rx_bits);
                    trace_append("  %s: rejected noisy AC2K capture\n", proto_modes[c].name);
                }
            }

            furi_delay_us(HITAG_S_T_WAIT_SC_US);
        }

        if(marginal_uid_valid) {
            *uid = marginal_uid;
            active_mode_idx = c;
            FURI_LOG_W(
                TAG,
                "UID: %08lX (via %s mode, marginal noisy AC2K)",
                (unsigned long)*uid,
                proto_modes[c].name);
            trace_append(
                "  RESULT: OK, UID=%08lX (mode=%s, AC2K, marginal)\n",
                (unsigned long)*uid,
                proto_modes[c].name);
            trace_append("  %s: using marginal noisy AC2K UID\n", proto_modes[c].name);
            return HitagSResultOk;
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

static HitagSResult hitag_s_select_frame(uint32_t uid, uint32_t* config, const char* uid_order) {
    /* SELECT = 00000 (5 bits) + UID (32 bits) + CRC8 (8 bits) = 45 bits */
    uint8_t cmd[6] = {0}; /* 48 bits capacity */
    size_t bit_pos = 0;
    uint8_t crc = hitag_s_codec_build_select_frame(cmd, &bit_pos, uid);

    FURI_LOG_D(
        TAG, "TX: SELECT UID=%08lX CRC=%02X (45 bits, %s)", (unsigned long)uid, crc, uid_order);
    trace_append(
        "  TX: SELECT UID=%08lX CRC=%02X (45 bits, %s)\n", (unsigned long)uid, crc, uid_order);
    trace_append(
        "  TX_FRAME: frame=%02X %02X %02X %02X %02X %02X bits=%d tx_us=%lu\n",
        cmd[0],
        cmd[1],
        cmd[2],
        cmd[3],
        cmd[4],
        cmd[5],
        (int)bit_pos,
        (unsigned long)hitag_s_codec_bplm_frame_duration_us(cmd, bit_pos));

    furi_delay_us(HITAG_S_T_WAIT_INTER_US);

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
    if(proto_modes[active_mode_idx].response_crc && rx_bits >= 40) {
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

HitagSResult hitag_s_select(uint32_t uid, uint32_t* config) {
    trace_append("\n--- SELECT ---\n");

    HitagSUidVariant variants[HITAG_S_UID_VARIANT_MAX] = {0};
    size_t variant_count = hitag_s_codec_uid_variants(uid, variants, COUNT_OF(variants));

    HitagSResult last_result = HitagSResultTimeout;
    for(size_t i = 0; i < variant_count; i++) {
        if(i > 0) {
            trace_append("  SELECT retry with %s\n", variants[i].label);
        }
        last_result = hitag_s_select_frame(variants[i].uid, config, variants[i].label);
        if(last_result == HitagSResultOk) {
            return last_result;
        }
    }

    return last_result;
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
    if(proto_modes[active_mode_idx].response_crc && rx_bits >= 40) {
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
