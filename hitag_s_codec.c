/**
 * @file hitag_s_codec.c
 * @brief Pure HiTag S codec helpers.
 */

#include "hitag_s_codec.h"

#define HITAG_S_AC2K_GLITCH_US      80U
#define HITAG_S_CODEC_T0_US         8U
#define HITAG_S_CODEC_T_0_CYCLES    20U
#define HITAG_S_CODEC_T_1_CYCLES    28U
#define HITAG_S_CODEC_T_LOW_CYCLES  8U
#define HITAG_S_CODEC_T_STOP_CYCLES 36U
#define HITAG_S_START01_MIN_VOTES   8U
#define HITAG_S_START01_MIN_PARTIAL 4U
#define HITAG_HTU_FLAG_CRCT         0x04U
#define HITAG_HTU_CMD_READ_UID      0x02U
#define HITAG_HTU_CRC16_POLY_CCITT  0x1021U

static uint16_t hitag_htu_codec_reflect16(uint16_t v) {
    uint16_t out = 0;
    for(size_t i = 0; i < 16; i++) {
        if(v & (1U << i)) {
            out |= (uint16_t)(1U << (15 - i));
        }
    }
    return out;
}

uint8_t hitag_s_codec_crc8(const uint8_t* data, size_t bits) {
    uint8_t crc = 0xFF;
    for(size_t i = 0; i < bits; i++) {
        uint8_t byte_idx = i / 8;
        uint8_t bit_idx = 7 - (i % 8);
        bool bit = (data[byte_idx] >> bit_idx) & 1U;
        bool c7 = (crc >> 7) & 1U;
        crc <<= 1;
        if(c7 ^ bit) {
            crc ^= 0x1D;
        }
    }
    return crc;
}

void hitag_s_codec_pack_bits(uint8_t* buf, size_t* bit_pos, uint32_t value, size_t n_bits) {
    for(size_t i = 0; i < n_bits; i++) {
        size_t pos = *bit_pos + i;
        uint8_t byte_idx = pos / 8;
        uint8_t bit_idx = 7 - (pos % 8);
        bool bit_val = (value >> (n_bits - 1 - i)) & 1U;
        if(bit_val) {
            buf[byte_idx] |= (1U << bit_idx);
        } else {
            buf[byte_idx] &= ~(1U << bit_idx);
        }
    }
    *bit_pos += n_bits;
}

void hitag_htu_codec_pack_bits_lsb(uint8_t* buf, size_t* bit_pos, uint32_t value, size_t n_bits) {
    for(size_t i = 0; i < n_bits; i++) {
        size_t pos = *bit_pos + i;
        uint8_t byte_idx = pos / 8;
        uint8_t bit_idx = 7 - (pos % 8);
        bool bit_val = (value >> i) & 1U;
        if(bit_val) {
            buf[byte_idx] |= (uint8_t)(1U << bit_idx);
        } else {
            buf[byte_idx] &= (uint8_t) ~(1U << bit_idx);
        }
    }
    *bit_pos += n_bits;
}

uint16_t hitag_htu_codec_crc16(const uint8_t* data, size_t bits, bool refout) {
    if(bits == 0) return 0xFFFFU;

    uint16_t remainder = 0;
    uint8_t offset = 8 - (bits % 8);
    uint8_t prebits = 0;

    for(size_t i = 0; i < (bits + 7) / 8; i++) {
        uint8_t c = (uint8_t)(prebits | (data[i] >> offset));
        prebits = (uint8_t)(data[i] << (8 - offset));

        remainder ^= (uint16_t)c << 8;
        for(size_t j = 0; j < 8; j++) {
            if(remainder & 0x8000U) {
                remainder = (uint16_t)((remainder << 1) ^ HITAG_HTU_CRC16_POLY_CCITT);
            } else {
                remainder = (uint16_t)(remainder << 1);
            }
        }
    }

    return refout ? hitag_htu_codec_reflect16(remainder) : remainder;
}

uint16_t hitag_htu_codec_build_read_uid_frame(uint8_t* buf, size_t* bits) {
    *bits = 0;
    hitag_htu_codec_pack_bits_lsb(buf, bits, HITAG_HTU_FLAG_CRCT, 5);
    hitag_htu_codec_pack_bits_lsb(buf, bits, HITAG_HTU_CMD_READ_UID, 6);
    uint16_t crc = hitag_htu_codec_crc16(buf, *bits, true);
    hitag_htu_codec_pack_bits_lsb(buf, bits, crc, 16);
    return crc;
}

static bool hitag_s_codec_get_bit(const uint8_t* data, size_t bit_pos) {
    return (data[bit_pos / 8] >> (7 - (bit_pos % 8))) & 1U;
}

bool hitag_htu_codec_decode_uid_response(
    const uint8_t* rx,
    size_t rx_bits,
    uint8_t uid[HITAG_HTU_UID_SIZE]) {
    const size_t expected_bits = 1 + (HITAG_HTU_UID_SIZE * 8) + 16;
    if(rx_bits < expected_bits) return false;
    if(hitag_htu_codec_crc16(rx, expected_bits, false) != 0) return false;

    for(size_t i = 0; i < HITAG_HTU_UID_SIZE; i++) {
        uint8_t b = 0;
        for(size_t j = 0; j < 8; j++) {
            if(hitag_s_codec_get_bit(rx, 1 + (i * 8) + j)) {
                b |= (uint8_t)(1U << (7 - j));
            }
        }
        uid[i] = b;
    }
    return true;
}

uint8_t hitag_s_codec_build_select_frame(uint8_t* buf, size_t* bits, uint32_t uid) {
    *bits = 0;
    hitag_s_codec_pack_bits(buf, bits, 0x00, 5);
    hitag_s_codec_pack_bits(buf, bits, uid, 32);
    uint8_t crc = hitag_s_codec_crc8(buf, *bits);
    hitag_s_codec_pack_bits(buf, bits, crc, 8);
    return crc;
}

static uint32_t hitag_s_codec_reverse_uid_bytes(uint32_t uid) {
    return ((uid & 0x000000FFUL) << 24) | ((uid & 0x0000FF00UL) << 8) |
           ((uid & 0x00FF0000UL) >> 8) | ((uid & 0xFF000000UL) >> 24);
}

static uint8_t hitag_s_codec_reverse_bits8(uint8_t v) {
    v = ((v & 0xF0U) >> 4) | ((v & 0x0FU) << 4);
    v = ((v & 0xCCU) >> 2) | ((v & 0x33U) << 2);
    return ((v & 0xAAU) >> 1) | ((v & 0x55U) << 1);
}

static uint32_t hitag_s_codec_reverse_uid_byte_bits(uint32_t uid) {
    return ((uint32_t)hitag_s_codec_reverse_bits8((uid >> 24) & 0xFFU) << 24) |
           ((uint32_t)hitag_s_codec_reverse_bits8((uid >> 16) & 0xFFU) << 16) |
           ((uint32_t)hitag_s_codec_reverse_bits8((uid >> 8) & 0xFFU) << 8) |
           (uint32_t)hitag_s_codec_reverse_bits8(uid & 0xFFU);
}

size_t hitag_s_codec_uid_variants(uint32_t uid, HitagSUidVariant* variants, size_t max_variants) {
    const HitagSUidVariant candidates[HITAG_S_UID_VARIANT_MAX] = {
        {uid, "UID0..UID3"},
        {hitag_s_codec_reverse_uid_bytes(uid), "UID3..UID0"},
        {hitag_s_codec_reverse_uid_byte_bits(uid), "bit-reversed bytes"},
        {
            hitag_s_codec_reverse_uid_bytes(hitag_s_codec_reverse_uid_byte_bits(uid)),
            "bit+byte reversed",
        },
    };
    size_t count = 0;

    for(size_t i = 0; i < HITAG_S_UID_VARIANT_MAX && count < max_variants; i++) {
        bool duplicate = false;
        for(size_t j = 0; j < count; j++) {
            if(variants[j].uid == candidates[i].uid) {
                duplicate = true;
                break;
            }
        }
        if(duplicate) continue;
        variants[count++] = candidates[i];
    }

    return count;
}

uint32_t hitag_s_codec_bplm_frame_duration_us(const uint8_t* data, size_t bits) {
    uint32_t duration = 0;

    for(size_t i = 0; i < bits; i++) {
        uint8_t byte_idx = i / 8;
        uint8_t bit_idx = 7 - (i % 8);
        bool bit = (data[byte_idx] >> bit_idx) & 1U;
        duration +=
            (bit ? HITAG_S_CODEC_T_1_CYCLES : HITAG_S_CODEC_T_0_CYCLES) * HITAG_S_CODEC_T0_US;
    }

    duration += (HITAG_S_CODEC_T_LOW_CYCLES + HITAG_S_CODEC_T_STOP_CYCLES) * HITAG_S_CODEC_T0_US;
    return duration;
}

void hitag_s_codec_ac2k_quality_add(HitagSAc2kQuality* quality, bool level, uint32_t duration) {
    if(level) return;

    if(!quality->startup_seen) {
        quality->startup_seen = true;
        return;
    }

    if(duration < HITAG_S_AC2K_GLITCH_US) {
        quality->glitches++;
    } else if(duration > 1100U) {
        quality->long_gaps++;
        quality->long_ac_periods++;
    } else if(duration > 600U) {
        quality->long_ac_periods++;
    } else {
        quality->usable_periods++;
    }

    quality->too_noisy = quality->glitches > 1 || quality->long_ac_periods > 1;
}

HitagSAc2kQuality hitag_s_codec_ac2k_quality(const HitagSEdge* edges, size_t edge_count) {
    HitagSAc2kQuality quality = {0};

    for(size_t i = 0; i < edge_count; i++) {
        hitag_s_codec_ac2k_quality_add(&quality, edges[i].level, edges[i].duration);
    }
    return quality;
}

bool hitag_s_codec_is_valid_ac2k_uid_capture(
    size_t bits,
    const HitagSEdge* edges,
    size_t edge_count) {
    return bits == 32 && !hitag_s_codec_ac2k_quality(edges, edge_count).too_noisy;
}

bool hitag_s_codec_is_marginal_ac2k_uid_quality(size_t bits, const HitagSAc2kQuality* quality) {
    return bits == 32 && quality->glitches <= 2 && quality->long_ac_periods <= 1;
}

static size_t hitag_s_codec_uid_popcount(uint32_t uid) {
    size_t count = 0;
    while(uid) {
        count += uid & 1U;
        uid >>= 1;
    }
    return count;
}

bool hitag_s_codec_is_low_entropy_uid(uint32_t uid) {
    size_t popcount = hitag_s_codec_uid_popcount(uid);
    return popcount <= 2 || popcount >= 30 || uid == 0x00000000UL || uid == 0x40000000UL ||
           uid == 0x60000000UL || uid == 0x80000000UL || uid == 0xFFFFFFFFUL;
}

bool hitag_s_codec_is_acceptable_start01_uid(uint32_t uid, size_t votes, size_t partial_support) {
    return votes >= HITAG_S_START01_MIN_VOTES && partial_support >= HITAG_S_START01_MIN_PARTIAL &&
           !hitag_s_codec_is_low_entropy_uid(uid);
}
