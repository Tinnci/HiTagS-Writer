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
#include <string.h>

#define TAG                               "HitagS"
#define HITAG_S_START01_CONSENSUS_MIN     8
#define HITAG_S_START01_CONSENSUS_MAX     4
#define HITAG_S_UID_MODE_CONFIRMATION_MIN 2
#define HITAG_HTU_RX_TIMEOUT_UID          25000 /* Hitag µ READ UID response */

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
#define HITAG_S_MAX_EDGES               512
#define HITAG_S_TRACE_MAX_EDGES_PER_RX  24
#define HITAG_HTU_RESPONSE_BITS         65
#define HITAG_HTU_MAX_CANDIDATE_BITS    96
#define HITAG_HTU_TRACE_CANDIDATE_LIMIT 24

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

static void hitag_s_capture_start(void* context) {
    UNUSED(context);
    hs_capture.edge_count = 0;
    hs_capture.overflow = false;
    furi_hal_rfid_tim_read_capture_start(hitag_s_capture_callback, (void*)&hs_capture);
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
    size_t sof_bits,
    uint32_t thresh_23_us,
    uint32_t thresh_34_us,
    uint32_t glitch_us,
    const char* mode_name) {
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
            continue;
        }

        if(rb < glitch_us) continue;

        period_count++;

        if(rb >= thresh_34_us) {
            /* FOUR_HALF: one '0' bit */
            lastbit = 0;
            total_bits++;
            if(sof_remaining > 0) {
                sof_remaining--;
            } else {
                data_bits++;
            }
        } else if(rb >= thresh_23_us) {
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

    UNUSED(mode_name);
    UNUSED(period_count);
    UNUSED(total_bits);

    return data_bits;
}

static void hitag_s_ac2k_put_bit(uint8_t* out_data, size_t* data_bits, size_t max_bits, bool bit) {
    if(*data_bits >= max_bits) return;
    if(bit) {
        out_data[*data_bits / 8] |= (1U << (7 - (*data_bits % 8)));
    }
    (*data_bits)++;
}

static size_t
    hitag_s_decode_ac2k_start01(const HitagSCapture* cap, uint8_t* out_data, size_t max_bits) {
    memset(out_data, 0, (max_bits + 7) / 8);

    int lastbit = 0;
    bool bSkip = false;
    bool started = false;
    size_t data_bits = 0;

    for(size_t i = 0; i < cap->edge_count && data_bits < max_bits; i++) {
        if(cap->levels[i]) continue;

        uint32_t rb = cap->durations[i];
        if(!started) {
            if(rb < HITAG_S_AC2K_GLITCH_US) continue;
            started = true;
            hitag_s_ac2k_put_bit(out_data, &data_bits, max_bits, false);
            hitag_s_ac2k_put_bit(out_data, &data_bits, max_bits, true);
            continue;
        }

        if(rb < HITAG_S_AC2K_GLITCH_US) continue;

        if(rb >= HITAG_S_AC2K_THRESH_34_US) {
            lastbit = 0;
            hitag_s_ac2k_put_bit(out_data, &data_bits, max_bits, false);
        } else if(rb >= HITAG_S_AC2K_THRESH_23_US) {
            lastbit = !lastbit;
            hitag_s_ac2k_put_bit(out_data, &data_bits, max_bits, lastbit);
            bSkip = (lastbit != 0);
        } else {
            if(!bSkip) {
                lastbit = 1;
                hitag_s_ac2k_put_bit(out_data, &data_bits, max_bits, true);
            }
            bSkip = !bSkip;
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

static bool hitag_s_capture_is_partial_uid_response(const HitagSCapture* cap, size_t rx_bits) {
    HitagSAc2kQuality quality = hitag_s_capture_ac2k_quality(cap);
    return rx_bits >= 27 && rx_bits < 32 && quality.usable_periods >= 27 && quality.long_gaps >= 1;
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

    uint32_t glitch_min = (threshold <= 128) ? 25 :
                                               ((threshold > 200) ? 80 : HITAG_S_MC4K_GLITCH_US);

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

    return data_bits;
}

static bool hitag_htu_bit_get(const uint8_t* data, size_t bit) {
    return (data[bit / 8] >> (7 - (bit % 8))) & 1U;
}

static void hitag_htu_bit_put(uint8_t* data, size_t bit, bool value) {
    if(value) {
        data[bit / 8] |= 1U << (7 - (bit % 8));
    }
}

static size_t hitag_htu_copy_window(
    const uint8_t* src,
    size_t src_bits,
    size_t start_bit,
    uint8_t* dst,
    size_t max_bits,
    bool invert) {
    memset(dst, 0, (max_bits + 7) / 8);
    if(start_bit >= src_bits) return 0;

    size_t bits = src_bits - start_bit;
    if(bits > max_bits) bits = max_bits;

    for(size_t i = 0; i < bits; i++) {
        bool bit = hitag_htu_bit_get(src, start_bit + i);
        hitag_htu_bit_put(dst, i, invert ? !bit : bit);
    }

    return bits;
}

static uint16_t hitag_htu_candidate_residue(const uint8_t* data, size_t bits) {
    uint8_t uid[HITAG_HTU_UID_SIZE] = {0};
    if(hitag_htu_codec_decode_uid_response(data, bits, uid)) return 0;

    if(bits == HITAG_HTU_RESPONSE_BITS - 1) {
        uint8_t normalized[(HITAG_HTU_RESPONSE_BITS + 7) / 8] = {0};
        for(size_t i = 0; i < bits; i++) {
            hitag_htu_bit_put(normalized, i + 1, hitag_htu_bit_get(data, i));
        }
        return hitag_htu_codec_crc16(normalized, HITAG_HTU_RESPONSE_BITS, false);
    }
    if(bits < HITAG_HTU_RESPONSE_BITS) return 0xFFFFU;
    return hitag_htu_codec_crc16(data, HITAG_HTU_RESPONSE_BITS, false);
}

static uint32_t hitag_htu_candidate_score(size_t bits, uint16_t residue) {
    uint32_t distance = (bits > HITAG_HTU_RESPONSE_BITS) ?
                            (uint32_t)(bits - HITAG_HTU_RESPONSE_BITS) :
                            (uint32_t)(HITAG_HTU_RESPONSE_BITS - bits);
    if(bits >= HITAG_HTU_RESPONSE_BITS && residue != 0) distance += 100;
    return distance;
}

static void hitag_htu_probe_note_candidate(
    HitagHtuProbeInfo* info,
    const char* method,
    size_t bits,
    uint16_t residue) {
    if(!info) return;

    info->had_activity = true;
    if(info->method == NULL ||
       hitag_htu_candidate_score(bits, residue) <
           hitag_htu_candidate_score(info->response_bits, info->crc_ok ? 0 : 0xFFFFU)) {
        info->method = method;
        info->response_bits = bits;
        info->crc_ok = (residue == 0);
        info->best_residue = residue;
    }
}

static bool hitag_htu_try_raw_candidate(
    HitagHtuProbeInfo* info,
    const char* method,
    const uint8_t* raw,
    size_t raw_bits,
    uint8_t uid[HITAG_HTU_UID_SIZE]) {
    uint8_t candidate[(HITAG_HTU_MAX_CANDIDATE_BITS + 7) / 8];

    for(size_t sof = 0; sof <= 8; sof++) {
        for(size_t inv = 0; inv < 2; inv++) {
            bool invert = inv != 0;
            size_t bits = hitag_htu_copy_window(
                raw, raw_bits, sof, candidate, HITAG_HTU_MAX_CANDIDATE_BITS, invert);
            if(bits == 0) continue;

            uint16_t residue = hitag_htu_candidate_residue(candidate, bits);
            if(info) info->candidates_tried++;

            if(hitag_s_trace_is_active() &&
               (!info || info->candidates_tried <= HITAG_HTU_TRACE_CANDIDATE_LIMIT ||
                residue == 0)) {
                trace_append(
                    "  HTU candidate method=%s sof=%d invert=%d bits=%d crc16=%04X first=%02X %02X %02X\n",
                    method,
                    (int)sof,
                    invert ? 1 : 0,
                    (int)bits,
                    residue,
                    candidate[0],
                    candidate[1],
                    candidate[2]);
            }

            hitag_htu_probe_note_candidate(info, method, bits, residue);
            if(info && info->method == method && info->response_bits == bits) {
                info->best_prefix[0] = candidate[0];
                info->best_prefix[1] = candidate[1];
                info->best_prefix[2] = candidate[2];
                info->ttf_broadcast = hitag_htu_codec_is_ttf_broadcast_candidate(candidate, bits);
            }
            if(hitag_htu_codec_decode_uid_response(candidate, bits, uid)) {
                if(info) {
                    info->detected = true;
                    info->crc_ok = true;
                    info->method = method;
                    info->response_bits = bits;
                }
                return true;
            }
        }
    }

    return false;
}

typedef struct {
    uint32_t high;
    uint32_t period;
} HitagHtuPulsePair;

static size_t
    hitag_htu_capture_pairs(const HitagSCapture* cap, HitagHtuPulsePair* pairs, size_t max_pairs) {
    size_t count = 0;
    uint32_t high = 0;

    for(size_t i = 0; i < cap->edge_count; i++) {
        if(cap->levels[i]) {
            high = cap->durations[i];
            continue;
        }

        uint32_t period = cap->durations[i];
        if(high > 0 && period > high && count < max_pairs) {
            pairs[count].high = high;
            pairs[count].period = period;
            count++;
        }
        high = 0;
    }

    return count;
}

static size_t hitag_htu_falling_intervals(
    const HitagHtuPulsePair* pairs,
    size_t pair_count,
    uint32_t* intervals,
    size_t max_intervals) {
    size_t count = 0;

    for(size_t i = 0; i + 1 < pair_count && count < max_intervals; i++) {
        if(pairs[i].period <= pairs[i].high) continue;
        intervals[count++] = (pairs[i].period - pairs[i].high) + pairs[i + 1].high;
    }

    return count;
}

static size_t hitag_htu_decode_pm3_mc(
    const uint32_t* intervals,
    size_t interval_count,
    uint8_t* out_data,
    size_t max_bits,
    uint32_t two_half_us,
    uint32_t three_half_us,
    uint32_t four_half_us,
    size_t start_skip,
    bool seed_one) {
    memset(out_data, 0, (max_bits + 7) / 8);
    size_t bits = 0;
    bool lastbit = true;
    bool bSkip = false;

    if(seed_one) {
        hitag_htu_bit_put(out_data, bits++, true);
        lastbit = true;
        bSkip = true;
    }

    for(size_t i = start_skip; i < interval_count && bits < max_bits; i++) {
        uint32_t rb = intervals[i];
        if(rb < (two_half_us / 2)) continue;

        if(rb >= four_half_us) {
            hitag_htu_bit_put(out_data, bits++, false);
            if(bits < max_bits) hitag_htu_bit_put(out_data, bits++, true);
            lastbit = true;
            bSkip = true;
        } else if(rb >= three_half_us) {
            lastbit = !lastbit;
            hitag_htu_bit_put(out_data, bits++, lastbit);
            bSkip = lastbit;
        } else if(rb >= two_half_us) {
            if(!bSkip) {
                hitag_htu_bit_put(out_data, bits++, lastbit);
            }
            bSkip = !bSkip;
        }
    }

    return bits;
}

static bool hitag_htu_decode_candidates(HitagHtuProbeInfo* info, uint8_t uid[HITAG_HTU_UID_SIZE]) {
    static uint8_t raw[(HITAG_HTU_MAX_CANDIDATE_BITS + 7) / 8];
    static HitagHtuPulsePair pairs[HITAG_S_MAX_EDGES / 2];
    static uint32_t intervals[HITAG_S_MAX_EDGES / 2];

    if(info) {
        info->had_activity = hs_capture.edge_count > 0;
        info->method = NULL;
        info->response_bits = 0;
        info->candidates_tried = 0;
    }

    const struct {
        const char* method;
        uint32_t threshold;
    } half_methods[] = {
        {"half-mc4k", HITAG_S_MC4K_THRESHOLD_US},
        {"half-mc8k", 96},
        {"half-mc2k", 384},
        {"half-mc4k-low", 160},
        {"half-mc4k-high", 224},
    };

    for(size_t i = 0; i < COUNT_OF(half_methods); i++) {
        size_t bits = hitag_s_decode_mc4k(
            &hs_capture, raw, HITAG_HTU_MAX_CANDIDATE_BITS, 0, half_methods[i].threshold);
        if(hitag_htu_try_raw_candidate(info, half_methods[i].method, raw, bits, uid)) return true;
    }

    size_t pair_count = hitag_htu_capture_pairs(&hs_capture, pairs, COUNT_OF(pairs));
    size_t interval_count =
        hitag_htu_falling_intervals(pairs, pair_count, intervals, COUNT_OF(intervals));
    const struct {
        const char* method;
        uint32_t two;
        uint32_t three;
        uint32_t four;
    } pm3_methods[] = {
        {"pm3-mc4k", 25U * HITAG_S_T0_US, 41U * HITAG_S_T0_US, 57U * HITAG_S_T0_US},
        {"pm3-mc8k",
         (25U * HITAG_S_T0_US) / 2,
         (41U * HITAG_S_T0_US) / 2,
         (57U * HITAG_S_T0_US) / 2},
        {"pm3-mc2k", 25U * HITAG_S_T0_US * 2, 41U * HITAG_S_T0_US * 2, 57U * HITAG_S_T0_US * 2},
    };

    for(size_t i = 0; i < COUNT_OF(pm3_methods); i++) {
        for(size_t start_skip = 0; start_skip <= 4; start_skip++) {
            for(size_t seed = 0; seed < 2; seed++) {
                size_t bits = hitag_htu_decode_pm3_mc(
                    intervals,
                    interval_count,
                    raw,
                    HITAG_HTU_MAX_CANDIDATE_BITS,
                    pm3_methods[i].two,
                    pm3_methods[i].three,
                    pm3_methods[i].four,
                    start_skip,
                    seed != 0);
                if(hitag_htu_try_raw_candidate(info, pm3_methods[i].method, raw, bits, uid)) {
                    return true;
                }
            }
        }
    }

    return false;
}

static const char* hitag_s_rx_mode_name(HitagSRxMode rx_mode) {
    switch(rx_mode) {
    case HitagSRxAC2K:
        return "AC2K";
    case HitagSRxMC4K:
        return "MC4K";
    case HitagSRxMC2K:
        return "MC2K";
    case HitagSRxAC4K:
        return "AC4K";
    case HitagSRxMC8K:
        return "MC8K";
    default:
        return "?";
    }
}

static uint32_t hitag_s_mc_threshold_us(HitagSRxMode rx_mode) {
    if(rx_mode == HitagSRxMC2K) return 384;
    if(rx_mode == HitagSRxMC8K) return 96;
    return HITAG_S_MC4K_THRESHOLD_US;
}

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
    hitag_s_send_frame_with_early_rx(tx_data, tx_bits, hitag_s_capture_start, NULL);

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
        "  RX_META: elapsed_us=%lu idle_us=%lu timeout_us=%lu final_edges=%d early_rx=stop_tail\n",
        (unsigned long)elapsed_us,
        (unsigned long)idle_us,
        (unsigned long)rx_timeout_us,
        (int)hs_capture.edge_count);

    if(hs_capture.edge_count == 0) {
        trace_append("  RX: no edges (timeout %lu us)\n", (unsigned long)rx_timeout_us);
        return 0;
    }

    const char* mode_str = hitag_s_rx_mode_name(rx_mode);

    /* Trace: log a bounded prefix of raw edges to avoid exhausting heap on noisy captures. */
    if(hitag_s_trace_is_active()) {
        trace_append(
            "  RX: %d edges%s mode=%s",
            (int)hs_capture.edge_count,
            hs_capture.overflow ? " [OVERFLOW]" : "",
            mode_str);
        if(rx_mode == HitagSRxAC2K) {
            trace_append(" threshold=%d/%d", HITAG_S_AC2K_THRESH_23_US, HITAG_S_AC2K_THRESH_34_US);
        } else if(rx_mode == HitagSRxAC4K) {
            trace_append(" threshold=%d/%d", 160, 224);
        } else {
            trace_append(" threshold=%lu", (unsigned long)hitag_s_mc_threshold_us(rx_mode));
        }
        trace_append(" sof=%d expected_bits=%d\n", (int)sof_bits, (int)rx_max_bits);
        trace_append("  EDGES:");
        size_t trace_edge_count = hs_capture.edge_count < HITAG_S_TRACE_MAX_EDGES_PER_RX ?
                                      hs_capture.edge_count :
                                      HITAG_S_TRACE_MAX_EDGES_PER_RX;
        for(size_t i = 0; i < trace_edge_count; i++) {
            trace_append(
                " %s:%lu",
                hs_capture.levels[i] ? "H" : "L",
                (unsigned long)hs_capture.durations[i]);
        }
        if(trace_edge_count < hs_capture.edge_count) {
            trace_append(
                " ... truncated_edges=%d", (int)(hs_capture.edge_count - trace_edge_count));
        }
        trace_append("\n");
    }

    /* Decode using appropriate decoder */
    size_t bits;
    if(rx_mode == HitagSRxAC2K || rx_mode == HitagSRxAC4K) {
        uint32_t thresh_23 = (rx_mode == HitagSRxAC4K) ? 160 : HITAG_S_AC2K_THRESH_23_US;
        uint32_t thresh_34 = (rx_mode == HitagSRxAC4K) ? 224 : HITAG_S_AC2K_THRESH_34_US;
        uint32_t glitch = (rx_mode == HitagSRxAC4K) ? 40 : HITAG_S_AC2K_GLITCH_US;
        bits = hitag_s_decode_ac2k(
            &hs_capture, rx_data, rx_max_bits, sof_bits, thresh_23, thresh_34, glitch, mode_str);
    } else {
        uint32_t threshold = hitag_s_mc_threshold_us(rx_mode);
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

static size_t hitag_htu_send_receive(
    const uint8_t* tx_data,
    size_t tx_bits,
    uint8_t* rx_data,
    size_t rx_max_bits,
    uint32_t rx_timeout_us,
    size_t sof_bits) {
    hitag_s_send_htu_frame_with_early_rx(tx_data, tx_bits, hitag_s_capture_start, NULL);

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

    furi_hal_rfid_tim_read_capture_stop();

    trace_append(
        "  RX_META: elapsed_us=%lu idle_us=%lu timeout_us=%lu final_edges=%d early_rx=htu_tail\n",
        (unsigned long)elapsed_us,
        (unsigned long)idle_us,
        (unsigned long)rx_timeout_us,
        (int)hs_capture.edge_count);

    if(hs_capture.edge_count == 0) {
        trace_append("  RX: no edges (timeout %lu us)\n", (unsigned long)rx_timeout_us);
        return 0;
    }

    if(hitag_s_trace_is_active()) {
        trace_append(
            "  RX: %d edges%s mode=MC4K threshold=%d sof=%d expected_bits=%d\n",
            (int)hs_capture.edge_count,
            hs_capture.overflow ? " [OVERFLOW]" : "",
            HITAG_S_MC4K_THRESHOLD_US,
            (int)sof_bits,
            (int)rx_max_bits);
        trace_append("  EDGES:");
        size_t trace_edge_count = hs_capture.edge_count < HITAG_S_TRACE_MAX_EDGES_PER_RX ?
                                      hs_capture.edge_count :
                                      HITAG_S_TRACE_MAX_EDGES_PER_RX;
        for(size_t i = 0; i < trace_edge_count; i++) {
            trace_append(
                " %s:%lu",
                hs_capture.levels[i] ? "H" : "L",
                (unsigned long)hs_capture.durations[i]);
        }
        if(trace_edge_count < hs_capture.edge_count) {
            trace_append(
                " ... truncated_edges=%d", (int)(hs_capture.edge_count - trace_edge_count));
        }
        trace_append("\n");
    }

    size_t bits = hitag_s_decode_mc4k(
        &hs_capture, rx_data, rx_max_bits, sof_bits, HITAG_S_MC4K_THRESHOLD_US);

    if(hitag_s_trace_is_active()) {
        trace_append("  DECODE: %d bits", (int)bits);
        if(bits > 0) {
            size_t bytes = (bits + 7) / 8;
            trace_append(" =");
            for(size_t i = 0; i < bytes && i < 9; i++) {
                trace_append(" %02X", rx_data[i]);
            }
        }
        trace_append("\n");
    }

    return bits;
}

static uint32_t hitag_htu_frame_duration_us(const uint8_t* data, size_t bits) {
    uint32_t duration = HITAG_S_T_0_CYCLES * HITAG_S_T0_US;
    duration += (HITAG_S_T_LOW_CYCLES + HITAG_S_T_CODE_VIOLATION_CYCLES) * HITAG_S_T0_US;
    for(size_t i = 0; i < bits; i++) {
        uint8_t byte_idx = i / 8;
        uint8_t bit_idx = 7 - (i % 8);
        bool bit = (data[byte_idx] >> bit_idx) & 1U;
        duration += (bit ? HITAG_S_T_1_CYCLES : HITAG_S_T_0_CYCLES) * HITAG_S_T0_US;
    }
    duration += HITAG_S_T_LOW_CYCLES * HITAG_S_T0_US;
    return duration;
}

HitagSResult hitag_htu_probe_uid(HitagHtuProbeInfo* info) {
    if(info) memset(info, 0, sizeof(*info));

    uint8_t tx[4] = {0};
    size_t tx_bits = 0;
    uint16_t tx_crc = hitag_htu_codec_build_read_uid_frame(tx, &tx_bits);

    FURI_LOG_I(TAG, "HTU/8265 probe: READ UID (bits=%d crc=%04X)", (int)tx_bits, tx_crc);
    trace_append("\n--- HTU_8265_PROBE ---\n");
    trace_append("  TX: HTU_READ_UID flags=CRCT cmd=0x02 crc=%04X\n", tx_crc);
    trace_append(
        "  TX_FRAME: frame=%02X %02X %02X %02X bits=%d tx_us=%lu sof=HTU\n",
        tx[0],
        tx[1],
        tx[2],
        tx[3],
        (int)tx_bits,
        (unsigned long)hitag_htu_frame_duration_us(tx, tx_bits));

    uint8_t uid[HITAG_HTU_UID_SIZE] = {0};
    uint8_t rx[(HITAG_HTU_MAX_CANDIDATE_BITS + 7) / 8] = {0};
    UNUSED(hitag_htu_send_receive(
        tx, tx_bits, rx, HITAG_HTU_MAX_CANDIDATE_BITS, HITAG_HTU_RX_TIMEOUT_UID, 0));

    bool ok = hitag_htu_decode_candidates(info, uid);

    if(!ok) {
        size_t best_bits = info ? info->response_bits : 0;
        const char* best_method = (info && info->method) ? info->method : "none";
        size_t candidates_tried = info ? info->candidates_tried : 0;
        if(!info || !info->had_activity) {
            FURI_LOG_I(TAG, "HTU/8265 probe: no response");
            trace_append("  RESULT: no HTU READ UID response\n");
            return HitagSResultTimeout;
        }
        FURI_LOG_W(
            TAG,
            "HTU/8265 probe: rejected response best=%s bits=%d candidates=%d crc16=%04X first=%02X %02X %02X%s",
            best_method,
            (int)best_bits,
            (int)candidates_tried,
            info ? info->best_residue : 0xFFFFU,
            info ? info->best_prefix[0] : 0,
            info ? info->best_prefix[1] : 0,
            info ? info->best_prefix[2] : 0,
            (info && info->ttf_broadcast) ? " ttf_broadcast" : "");
        trace_append(
            "  RESULT: HTU READ UID response rejected best=%s bits=%d candidates=%d crc16=%04X first=%02X %02X %02X%s\n",
            best_method,
            (int)best_bits,
            (int)candidates_tried,
            info ? info->best_residue : 0xFFFFU,
            info ? info->best_prefix[0] : 0,
            info ? info->best_prefix[1] : 0,
            info ? info->best_prefix[2] : 0,
            (info && info->ttf_broadcast) ? " ttf_broadcast" : "");
        return HitagSResultCrcError;
    }

    if(info) {
        info->detected = true;
        info->crc_ok = true;
        memcpy(info->uid, uid, HITAG_HTU_UID_SIZE);
    }

    FURI_LOG_W(
        TAG,
        "HTU/8265 probe UID=%02X%02X%02X%02X%02X%02X (48-bit, CRC16 OK, method=%s, candidates=%d)",
        uid[0],
        uid[1],
        uid[2],
        uid[3],
        uid[4],
        uid[5],
        (info && info->method) ? info->method : "?",
        info ? (int)info->candidates_tried : 0);
    trace_append(
        "  RESULT: HTU UID=%02X%02X%02X%02X%02X%02X bits=%d crc16=ok method=%s candidates=%d\n",
        uid[0],
        uid[1],
        uid[2],
        uid[3],
        uid[4],
        uid[5],
        info ? (int)info->response_bits : HITAG_HTU_RESPONSE_BITS,
        (info && info->method) ? info->method : "?",
        info ? (int)info->candidates_tried : 0);
    return HitagSResultOk;
}

HitagSResult hitag_htu_probe_uid_sequence(HitagHtuProbeInfo* info) {
    hitag_s_field_on();
    HitagSResult result = hitag_htu_probe_uid(info);
    hitag_s_field_off();
    return result;
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
    HitagSMode mode;
    HitagSRxMode uid_rx_mode;
    HitagSRxMode data_rx_mode;
    size_t uid_sof; /* SOF bits in UID response */
    size_t data_sof; /* SOF bits in data exchange */
    size_t select_response_bits;
    bool response_crc; /* Advanced modes append CRC to data responses */
} HitagSProtoMode;

static const HitagSProtoMode proto_modes[] = {
    {
        .cmd_5bit = 0x1A,
        .name = "FADV",
        .mode = HitagSModeFadv,
        .uid_rx_mode = HitagSRxAC4K,
        .data_rx_mode = HitagSRxMC8K,
        .uid_sof = 3,
        .data_sof = 6,
        .select_response_bits = 40,
        .response_crc = true,
    },
    {
        .cmd_5bit = 0x19,
        .name = "ADV1",
        .mode = HitagSModeAdv1,
        .uid_rx_mode = HitagSRxAC2K,
        .data_rx_mode = HitagSRxMC4K,
        .uid_sof = 0,
        .data_sof = 6,
        .select_response_bits = 40,
        .response_crc = true,
    },
    {
        .cmd_5bit = 0x18,
        .name = "ADV2",
        .mode = HitagSModeAdv2,
        .uid_rx_mode = HitagSRxAC2K,
        .data_rx_mode = HitagSRxMC4K,
        .uid_sof = 0,
        .data_sof = 6,
        .select_response_bits = 40,
        .response_crc = true,
    },
    {
        .cmd_5bit = 0x06,
        .name = "STD",
        .mode = HitagSModeStd,
        .uid_rx_mode = HitagSRxAC2K,
        .data_rx_mode = HitagSRxMC4K,
        .uid_sof = 0,
        .data_sof = 1,
        .select_response_bits = 32,
        .response_crc = false,
    },
};
static size_t active_mode_idx = 0;
static bool active_uid_requires_select_verification = false;

const char* hitag_s_mode_name(HitagSMode mode) {
    switch(mode) {
    case HitagSModeStd:
        return "STD";
    case HitagSModeAdv1:
        return "ADV1";
    case HitagSModeAdv2:
        return "ADV2";
    case HitagSModeFadv:
        return "FADV";
    default:
        return "?";
    }
}

static inline size_t hitag_s_data_sof(void) {
    return proto_modes[active_mode_idx].data_sof;
}

static inline HitagSRxMode hitag_s_data_rx_mode(void) {
    return proto_modes[active_mode_idx].data_rx_mode;
}

static inline size_t hitag_s_select_expected_bits(void) {
    return proto_modes[active_mode_idx].select_response_bits;
}

/* Receive timeouts */
#define HITAG_S_RX_TIMEOUT_UID  25000 /* AC2K UID response (~18ms + margin) */
#define HITAG_S_RX_TIMEOUT_DATA 15000 /* MC4K 32-bit response (~10ms + margin) */
#define HITAG_S_RX_TIMEOUT_ACK  5000 /* MC4K ACK response (~2.5ms + margin) */

typedef struct {
    uint32_t uid;
    size_t votes;
} HitagSStart01Vote;

static void hitag_s_start01_votes_reset(HitagSStart01Vote* votes, size_t vote_count) {
    for(size_t i = 0; i < vote_count; i++) {
        votes[i].uid = 0;
        votes[i].votes = 0;
    }
}

HitagSResult hitag_s_uid_request(uint32_t* uid) {
    trace_append("\n--- UID_REQUEST ---\n");
    HitagSStart01Vote start01_consensus[HITAG_S_START01_CONSENSUS_MAX] = {0};
    size_t partial_uid_responses = 0;
    size_t empty_uid_responses = 0;
    bool cold_retry_done = false;

    /* Try Proxmark's 8268 write mode first, then fall back to other observed modes. */
    for(size_t c = 0; c < COUNT_OF(proto_modes); c++) {
        trace_append(
            "PROTO_MODE: %s cmd=%02X uid_rx=%s data_rx=%s uid_sof=%d data_sof=%d\n",
            proto_modes[c].name,
            proto_modes[c].cmd_5bit << 3,
            hitag_s_rx_mode_name(proto_modes[c].uid_rx_mode),
            hitag_s_rx_mode_name(proto_modes[c].data_rx_mode),
            (int)proto_modes[c].uid_sof,
            (int)proto_modes[c].data_sof);
        uint8_t cmd[1] = {0};
        size_t bit_pos = 0;
        pack_bits(cmd, &bit_pos, proto_modes[c].cmd_5bit, 5);

        if(!hitag_s_trace_is_active()) {
            FURI_LOG_I(
                TAG,
                "TX: UID_REQ_%s (5 bits, val=0x%02X)",
                proto_modes[c].name,
                proto_modes[c].cmd_5bit);
        }
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
        size_t low_entropy_rejects = 0;
        size_t noisy_rejects = 0;
        size_t partial_noisy_rejects = 0;
        size_t marginal_noisy_candidates = 0;
        size_t mode_partial_uid_responses = 0;
        size_t mode_empty_uid_responses = 0;

        for(size_t attempt = 0; attempt < 6; attempt++) {
            uint8_t rx[4] = {0};
            trace_append("  attempt %d/%s:\n", (int)(attempt + 1), proto_modes[c].name);

            /* UID response must match the active protocol mode; other modulation
             * captures can be stable noise, so do not use them as UID truth. */
            size_t rx_bits = hitag_s_send_receive(
                cmd,
                5,
                rx,
                32,
                HITAG_S_RX_TIMEOUT_UID,
                proto_modes[c].uid_rx_mode,
                proto_modes[c].uid_sof);

            uint8_t start01_rx[4] = {0};
            size_t start01_bits = hitag_s_decode_ac2k_start01(&hs_capture, start01_rx, 32);
            if(start01_bits == 32) {
                uint32_t start01_uid = ((uint32_t)start01_rx[0] << 24) |
                                       ((uint32_t)start01_rx[1] << 16) |
                                       ((uint32_t)start01_rx[2] << 8) | (uint32_t)start01_rx[3];
                bool stored = false;
                for(size_t v = 0; v < COUNT_OF(start01_consensus); v++) {
                    if(start01_consensus[v].votes > 0 && start01_consensus[v].uid == start01_uid) {
                        start01_consensus[v].votes++;
                        stored = true;
                        break;
                    }
                }
                if(!stored) {
                    for(size_t v = 0; v < COUNT_OF(start01_consensus); v++) {
                        if(start01_consensus[v].votes == 0) {
                            start01_consensus[v].uid = start01_uid;
                            start01_consensus[v].votes = 1;
                            break;
                        }
                    }
                }
                trace_append(
                    "  %s: start01 candidate UID=%08lX\n",
                    proto_modes[c].name,
                    (unsigned long)start01_uid);
            }

            if(rx_bits == 32) {
                uint32_t current_uid = ((uint32_t)rx[0] << 24) | ((uint32_t)rx[1] << 16) |
                                       ((uint32_t)rx[2] << 8) | (uint32_t)rx[3];
                had_decode = true;

                if(hitag_s_codec_is_low_entropy_uid(current_uid)) {
                    low_entropy_rejects++;
                    trace_append(
                        "  %s: rejected low-entropy UID=%08lX\n",
                        proto_modes[c].name,
                        (unsigned long)current_uid);
                    furi_delay_us(HITAG_S_T_WAIT_SC_US);
                    continue;
                }

                if(hitag_s_capture_has_excessive_glitches(&hs_capture)) {
                    if(hitag_s_capture_is_marginal_uid_candidate(&hs_capture)) {
                        marginal_uid = current_uid;
                        marginal_uid_valid = true;
                        marginal_noisy_candidates++;
                        trace_append(
                            "  %s: keeping marginal noisy AC2K UID=%08lX\n",
                            proto_modes[c].name,
                            (unsigned long)marginal_uid);
                    } else {
                        noisy_rejects++;
                        trace_append("  %s: rejected noisy AC2K capture\n", proto_modes[c].name);
                    }
                    furi_delay_us(HITAG_S_T_WAIT_SC_US);
                    continue;
                }

                *uid = current_uid;
                active_mode_idx = c;
                active_uid_requires_select_verification = false;
                if(!hitag_s_trace_is_active()) {
                    FURI_LOG_I(
                        TAG,
                        "UID: %08lX (via %s mode, AC2K)",
                        (unsigned long)*uid,
                        proto_modes[c].name);
                }
                trace_append(
                    "  RESULT: OK, UID=%08lX (mode=%s, %s)\n",
                    (unsigned long)*uid,
                    proto_modes[c].name,
                    hitag_s_rx_mode_name(proto_modes[c].uid_rx_mode));
                return HitagSResultOk;
            } else if(rx_bits > 0) {
                had_decode = true;
                if(hitag_s_capture_is_partial_uid_response(&hs_capture, rx_bits)) {
                    partial_uid_responses++;
                    mode_partial_uid_responses++;
                    trace_append(
                        "  %s: partial UID response (%d bits, count=%d)\n",
                        proto_modes[c].name,
                        (int)rx_bits,
                        (int)partial_uid_responses);
                    if(partial_uid_responses >= 6 && !cold_retry_done) {
                        trace_append("  cold retry after repeated partial UID responses\n");
                        hitag_s_start01_votes_reset(
                            start01_consensus, COUNT_OF(start01_consensus));
                        hitag_s_field_off();
                        furi_delay_ms(50);
                        hitag_s_field_on();
                        cold_retry_done = true;
                    }
                }
                if(hitag_s_capture_has_excessive_glitches(&hs_capture)) {
                    partial_noisy_rejects++;
                    trace_append("  %s: rejected noisy AC2K capture\n", proto_modes[c].name);
                }
            } else if(hs_capture.edge_count <= 2) {
                empty_uid_responses++;
                mode_empty_uid_responses++;
                trace_append(
                    "  %s: empty UID response (edges=%d, count=%d)\n",
                    proto_modes[c].name,
                    (int)hs_capture.edge_count,
                    (int)empty_uid_responses);
                if(empty_uid_responses >= 6 && !cold_retry_done) {
                    trace_append("  cold retry after repeated empty UID responses\n");
                    hitag_s_start01_votes_reset(start01_consensus, COUNT_OF(start01_consensus));
                    hitag_s_field_off();
                    furi_delay_ms(80);
                    hitag_s_field_on();
                    cold_retry_done = true;
                }
            }

            furi_delay_us(HITAG_S_T_WAIT_SC_US);
        }

        if(marginal_uid_valid) {
            if(hitag_s_codec_is_low_entropy_uid(marginal_uid)) {
                trace_append(
                    "  %s: rejected low-entropy marginal UID=%08lX\n",
                    proto_modes[c].name,
                    (unsigned long)marginal_uid);
                marginal_uid_valid = false;
            }
        }

        if(marginal_uid_valid) {
            *uid = marginal_uid;
            active_mode_idx = c;
            active_uid_requires_select_verification = true;
            if(!hitag_s_trace_is_active()) {
                FURI_LOG_W(
                    TAG,
                    "UID: %08lX (via %s mode, marginal noisy AC2K)",
                    (unsigned long)*uid,
                    proto_modes[c].name);
            }
            trace_append(
                "  RESULT: OK, UID=%08lX (mode=%s, %s, marginal)\n",
                (unsigned long)*uid,
                proto_modes[c].name,
                hitag_s_rx_mode_name(proto_modes[c].uid_rx_mode));
            trace_append("  %s: using marginal noisy UID\n", proto_modes[c].name);
            return HitagSResultOk;
        }

        if(had_decode) {
            if(!hitag_s_trace_is_active()) {
                FURI_LOG_W(
                    TAG,
                    "%s: UID unstable (low_entropy=%d noisy=%d partial_noisy=%d marginal=%d partial=%d empty=%d)",
                    proto_modes[c].name,
                    (int)low_entropy_rejects,
                    (int)noisy_rejects,
                    (int)partial_noisy_rejects,
                    (int)marginal_noisy_candidates,
                    (int)mode_partial_uid_responses,
                    (int)mode_empty_uid_responses);
            }
            trace_append("  %s: UID decoded but unstable\n", proto_modes[c].name);
        } else {
            if(!hitag_s_trace_is_active()) {
                FURI_LOG_W(TAG, "%s: no valid 32-bit UID response", proto_modes[c].name);
            }
            trace_append("  %s: no valid UID response\n", proto_modes[c].name);
        }
        furi_delay_us(HITAG_S_T_WAIT_SC_US);
    }

    for(size_t v = 0; v < COUNT_OF(start01_consensus); v++) {
        if(hitag_s_codec_is_acceptable_start01_uid(
               start01_consensus[v].uid, start01_consensus[v].votes, partial_uid_responses)) {
            *uid = start01_consensus[v].uid;
            active_mode_idx = COUNT_OF(proto_modes) - 1;
            active_uid_requires_select_verification = true;
            if(!hitag_s_trace_is_active()) {
                FURI_LOG_W(
                    TAG,
                    "UID: %08lX via start01 consensus (%d votes)",
                    (unsigned long)*uid,
                    (int)start01_consensus[v].votes);
            }
            trace_append(
                "  RESULT: OK, UID=%08lX (mode=start01-consensus, AC2K)\n", (unsigned long)*uid);
            trace_append(
                "  start01: using consensus UID with %d votes\n", (int)start01_consensus[v].votes);
            return HitagSResultOk;
        } else if(start01_consensus[v].votes >= HITAG_S_START01_CONSENSUS_MIN) {
            const char* reason = hitag_s_codec_is_low_entropy_uid(start01_consensus[v].uid) ?
                                     "low-entropy" :
                                     "insufficient-partial-support";
            if(!hitag_s_trace_is_active()) {
                FURI_LOG_W(
                    TAG,
                    "start01: rejected UID %08lX (%s, votes=%d partial=%d)",
                    (unsigned long)start01_consensus[v].uid,
                    reason,
                    (int)start01_consensus[v].votes,
                    (int)partial_uid_responses);
            }
            trace_append(
                "  start01: rejected UID=%08lX reason=%s votes=%d partial=%d\n",
                (unsigned long)start01_consensus[v].uid,
                reason,
                (int)start01_consensus[v].votes,
                (int)partial_uid_responses);
        }
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
        "SELECT_EXPECT: bits=%d crc=%s mode=%s data_rx=%s\n",
        (int)hitag_s_select_expected_bits(),
        proto_modes[active_mode_idx].response_crc ? "yes" : "no",
        proto_modes[active_mode_idx].name,
        hitag_s_rx_mode_name(hitag_s_data_rx_mode()));
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

    /* Combined send + receive with active protocol response mode */
    uint8_t rx[5] = {0}; /* 32 config + possibly 8 CRC in ADV mode */
    size_t rx_bits = hitag_s_send_receive(
        cmd,
        45,
        rx,
        hitag_s_select_expected_bits(),
        HITAG_S_RX_TIMEOUT_DATA,
        hitag_s_data_rx_mode(),
        hitag_s_data_sof());

    if(rx_bits < hitag_s_select_expected_bits()) {
        FURI_LOG_W(TAG, "SELECT: only %d bits received", (int)rx_bits);
        trace_append("  RESULT: TIMEOUT (%d bits)\n", (int)rx_bits);
        if(active_uid_requires_select_verification) {
            trace_append("  fallback UID unverified by SELECT\n");
        }
        return HitagSResultTimeout;
    }

    /* In ADV mode, response includes 8-bit CRC — verify it */
    if(proto_modes[active_mode_idx].response_crc) {
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
    active_uid_requires_select_verification = false;
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

    if(active_uid_requires_select_verification) {
        active_uid_requires_select_verification = false;
    }
    return last_result;
}

static HitagSResult hitag_s_select_current_mode(uint32_t uid, uint32_t* config) {
    HitagSUidVariant variants[HITAG_S_UID_VARIANT_MAX] = {0};
    size_t variant_count = hitag_s_codec_uid_variants(uid, variants, COUNT_OF(variants));

    HitagSResult last_result = HitagSResultTimeout;
    for(size_t i = 0; i < variant_count; i++) {
        if(i > 0) {
            trace_append("  SELECT retry with %s\n", variants[i].label);
        }
        last_result = hitag_s_select_frame(variants[i].uid, config, variants[i].label);
        if(last_result == HitagSResultOk) return last_result;
    }
    return last_result;
}

static HitagSResult hitag_s_uid_request_mode(size_t mode_idx, uint32_t* uid) {
    const HitagSProtoMode* mode = &proto_modes[mode_idx];
    uint8_t cmd[1] = {0};
    size_t bit_pos = 0;
    pack_bits(cmd, &bit_pos, mode->cmd_5bit, 5);
    uint32_t confirmed_uid = 0;
    size_t confirmed_votes = 0;

    trace_append(
        "PROTO_MODE: %s cmd=%02X uid_rx=%s data_rx=%s uid_sof=%d data_sof=%d\n",
        mode->name,
        mode->cmd_5bit << 3,
        hitag_s_rx_mode_name(mode->uid_rx_mode),
        hitag_s_rx_mode_name(mode->data_rx_mode),
        (int)mode->uid_sof,
        (int)mode->data_sof);
    trace_append("  TX: UID_REQ_%s (5 bits, val=0x%02X)\n", mode->name, mode->cmd_5bit);
    trace_append(
        "  TX_FRAME: frame=%02X bits=%d tx_us=%lu\n",
        cmd[0],
        (int)bit_pos,
        (unsigned long)hitag_s_codec_bplm_frame_duration_us(cmd, bit_pos));

    for(size_t attempt = 0; attempt < 3; attempt++) {
        uint8_t rx[4] = {0};
        trace_append("  attempt %d/%s:\n", (int)(attempt + 1), mode->name);
        size_t rx_bits = hitag_s_send_receive(
            cmd, 5, rx, 32, HITAG_S_RX_TIMEOUT_UID, mode->uid_rx_mode, mode->uid_sof);

        if(rx_bits != 32) {
            furi_delay_us(HITAG_S_T_WAIT_SC_US);
            continue;
        }

        uint32_t candidate = ((uint32_t)rx[0] << 24) | ((uint32_t)rx[1] << 16) |
                             ((uint32_t)rx[2] << 8) | (uint32_t)rx[3];
        if(hitag_s_codec_is_low_entropy_uid(candidate)) {
            trace_append(
                "  %s: rejected low-entropy UID=%08lX\n", mode->name, (unsigned long)candidate);
            furi_delay_us(HITAG_S_T_WAIT_SC_US);
            continue;
        }
        if(mode->uid_rx_mode == HitagSRxAC2K &&
           hitag_s_capture_has_excessive_glitches(&hs_capture)) {
            trace_append("  %s: rejected noisy AC2K capture\n", mode->name);
            furi_delay_us(HITAG_S_T_WAIT_SC_US);
            continue;
        }

        if(confirmed_votes == 0 || confirmed_uid != candidate) {
            confirmed_uid = candidate;
            confirmed_votes = 1;
            trace_append(
                "  %s: UID candidate %08lX needs repeat confirmation\n",
                mode->name,
                (unsigned long)candidate);
            furi_delay_us(HITAG_S_T_WAIT_SC_US);
            continue;
        }

        confirmed_votes++;
        if(confirmed_votes >= HITAG_S_UID_MODE_CONFIRMATION_MIN) {
            *uid = confirmed_uid;
            active_mode_idx = mode_idx;
            active_uid_requires_select_verification = false;
            trace_append(
                "  RESULT: OK, UID=%08lX (mode=%s, %s)\n",
                (unsigned long)*uid,
                mode->name,
                hitag_s_rx_mode_name(mode->uid_rx_mode));
            return HitagSResultOk;
        }

        furi_delay_us(HITAG_S_T_WAIT_SC_US);
        continue;
    }

    if(confirmed_votes > 0) {
        trace_append(
            "  %s: rejected unstable UID candidate %08lX (votes=%d)\n",
            mode->name,
            (unsigned long)confirmed_uid,
            (int)confirmed_votes);
    }
    trace_append("  %s: no valid UID response\n", mode->name);
    return HitagSResultTimeout;
}

HitagSResult hitag_s_open_session(HitagSSessionInfo* session) {
    if(session) {
        memset(session, 0, sizeof(*session));
        session->mode = HitagSModeStd;
    }

    trace_append("\n--- OPEN_SESSION ---\n");

    HitagSResult last_result = HitagSResultTimeout;
    for(size_t mode_idx = 0; mode_idx < COUNT_OF(proto_modes); mode_idx++) {
        uint32_t uid = 0;
        uint32_t config = 0;
        active_mode_idx = mode_idx;
        trace_append("  mode probe: %s\n", proto_modes[mode_idx].name);

        last_result = hitag_s_uid_request_mode(mode_idx, &uid);
        if(last_result != HitagSResultOk) {
            trace_append("  field reset before next protocol mode\n");
            hitag_s_field_off();
            furi_delay_ms(20);
            hitag_s_field_on();
            continue;
        }

        trace_append("\n--- SELECT ---\n");
        last_result = hitag_s_select_current_mode(uid, &config);
        if(last_result == HitagSResultOk) {
            if(session) {
                session->mode = proto_modes[mode_idx].mode;
                session->uid = uid;
                session->config = config;
                session->selected = true;
            }
            trace_append(
                "  SESSION: selected mode=%s UID=%08lX Config=%08lX\n",
                proto_modes[mode_idx].name,
                (unsigned long)uid,
                (unsigned long)config);
            return HitagSResultOk;
        }

        trace_append(
            "  %s: SELECT failed after UID candidate %08lX (result=%d)\n",
            proto_modes[mode_idx].name,
            (unsigned long)uid,
            (int)last_result);
        trace_append("  field reset before next protocol mode\n");
        hitag_s_field_off();
        furi_delay_ms(20);
        hitag_s_field_on();
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
        cmd, 20, ack, 8, HITAG_S_RX_TIMEOUT_ACK, hitag_s_data_rx_mode(), hitag_s_data_sof());

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
        pwd_frame, 40, ack2, 8, HITAG_S_RX_TIMEOUT_ACK, hitag_s_data_rx_mode(), hitag_s_data_sof());

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
    trace_append("\n--- WRITE_PAGE %d ---\n", page);
    /* WRITE_PAGE = 1000 (4 bits) + addr (8 bits) + CRC8 (8 bits) = 20 bits */
    uint8_t cmd[3] = {0};
    size_t bit_pos = 0;

    pack_bits(cmd, &bit_pos, 0x08, 4); /* 1000 */
    pack_bits(cmd, &bit_pos, page, 8);
    uint8_t crc = hitag_s_crc8(cmd, 12);
    pack_bits(cmd, &bit_pos, crc, 8);

    FURI_LOG_D(TAG, "TX: WRITE_PAGE addr=%d CRC=%02X (20 bits)", page, crc);
    trace_append("  TX: WRITE_PAGE addr=%d CRC=%02X (20 bits)\n", page, crc);
    trace_append(
        "  TX_FRAME: frame=%02X %02X %02X bits=%d tx_us=%lu\n",
        cmd[0],
        cmd[1],
        cmd[2],
        (int)bit_pos,
        (unsigned long)hitag_s_codec_bplm_frame_duration_us(cmd, bit_pos));

    furi_delay_us(HITAG_S_T_WAIT_SC_US);

    /* Combined send + receive for ACK — MC4K */
    uint8_t ack[1] = {0};
    size_t ack_bits = hitag_s_send_receive(
        cmd, 20, ack, 8, HITAG_S_RX_TIMEOUT_ACK, hitag_s_data_rx_mode(), hitag_s_data_sof());

    if(ack_bits < 2) {
        FURI_LOG_W(TAG, "WRITE_PAGE addr=%d: no ACK", page);
        trace_append("  step1: no ACK (%d bits)\n", (int)ack_bits);
        return HitagSResultTimeout;
    }

    uint8_t ack_val = (ack[0] >> 6) & 0x03;
    if(ack_val != HITAG_S_ACK) {
        FURI_LOG_W(TAG, "WRITE_PAGE addr=%d: NACK (0x%02X)", page, ack_val);
        trace_append("  step1: NACK (0x%02X)\n", ack_val);
        return HitagSResultNack;
    }
    trace_append("  step1: ACK OK\n");

    /* Send 32-bit data + CRC8 = 40 bits */
    uint8_t data_frame[5] = {0};
    size_t data_pos = 0;
    pack_bits(data_frame, &data_pos, data, 32);
    uint8_t data_crc = hitag_s_crc8(data_frame, 32);
    pack_bits(data_frame, &data_pos, data_crc, 8);

    FURI_LOG_D(TAG, "TX: Data=%08lX CRC=%02X (40 bits)", (unsigned long)data, data_crc);
    trace_append("  TX: Data=%08lX CRC=%02X (40 bits)\n", (unsigned long)data, data_crc);
    trace_append(
        "  TX_FRAME: frame=%02X %02X %02X %02X %02X bits=%d tx_us=%lu\n",
        data_frame[0],
        data_frame[1],
        data_frame[2],
        data_frame[3],
        data_frame[4],
        (int)data_pos,
        (unsigned long)hitag_s_codec_bplm_frame_duration_us(data_frame, data_pos));

    furi_delay_us(HITAG_S_T_WAIT_SC_US);

    /* Combined send + receive — timeout includes programming time */
    uint8_t ack2[1] = {0};
    size_t ack2_bits = hitag_s_send_receive(
        data_frame,
        40,
        ack2,
        8,
        HITAG_S_T_PROG_US + HITAG_S_RX_TIMEOUT_ACK,
        hitag_s_data_rx_mode(),
        hitag_s_data_sof());

    if(ack2_bits < 2) {
        /* Some tags don't ACK after programming — treat as OK */
        FURI_LOG_D(TAG, "WRITE_PAGE addr=%d: no final ACK (may be OK)", page);
        trace_append("  step2: no final ACK (%d bits, accepted)\n", (int)ack2_bits);
        return HitagSResultOk;
    }

    uint8_t ack2_val = (ack2[0] >> 6) & 0x03;
    if(ack2_val != HITAG_S_ACK) {
        FURI_LOG_W(TAG, "WRITE_PAGE addr=%d: final NACK (0x%02X)", page, ack2_val);
        trace_append("  step2: NACK (0x%02X)\n", ack2_val);
        return HitagSResultNack;
    }

    FURI_LOG_I(TAG, "WRITE_PAGE addr=%d: OK", page);
    trace_append("  step2: ACK OK\n");
    trace_append("  RESULT: OK\n");
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
        cmd,
        20,
        rx,
        hitag_s_select_expected_bits(),
        HITAG_S_RX_TIMEOUT_DATA,
        hitag_s_data_rx_mode(),
        hitag_s_data_sof());

    if(rx_bits < hitag_s_select_expected_bits()) {
        FURI_LOG_W(TAG, "READ_PAGE addr=%d: only %d bits received", page, (int)rx_bits);
        trace_append("  RESULT: TIMEOUT (%d bits)\n", (int)rx_bits);
        return HitagSResultTimeout;
    }

    /* In ADV mode, verify CRC */
    if(proto_modes[active_mode_idx].response_crc) {
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
