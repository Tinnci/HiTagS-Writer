/**
 * @file hitag_s_codec.h
 * @brief Pure HiTag S codec helpers for offline-testable decoding and quality checks.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool level;
    uint32_t duration;
} HitagSEdge;

typedef struct {
    size_t usable_periods;
    size_t glitches;
    size_t long_gaps;
    size_t long_ac_periods;
    bool startup_seen;
    bool too_noisy;
} HitagSAc2kQuality;

#define HITAG_S_UID_VARIANT_MAX 4

typedef struct {
    uint32_t uid;
    const char* label;
} HitagSUidVariant;

#define HITAG_HTU_UID_SIZE 6

uint8_t hitag_s_codec_crc8(const uint8_t* data, size_t bits);

void hitag_s_codec_pack_bits(uint8_t* buf, size_t* bit_pos, uint32_t value, size_t n_bits);

void hitag_htu_codec_pack_bits_lsb(uint8_t* buf, size_t* bit_pos, uint32_t value, size_t n_bits);

uint16_t hitag_htu_codec_crc16(const uint8_t* data, size_t bits, bool refout);

uint16_t hitag_htu_codec_build_read_uid_frame(uint8_t* buf, size_t* bits);

bool hitag_htu_codec_decode_uid_response(
    const uint8_t* rx,
    size_t rx_bits,
    uint8_t uid[HITAG_HTU_UID_SIZE]);

uint8_t hitag_s_codec_build_select_frame(uint8_t* buf, size_t* bits, uint32_t uid);

size_t hitag_s_codec_uid_variants(uint32_t uid, HitagSUidVariant* variants, size_t max_variants);

uint32_t hitag_s_codec_bplm_frame_duration_us(const uint8_t* data, size_t bits);

void hitag_s_codec_ac2k_quality_add(HitagSAc2kQuality* quality, bool level, uint32_t duration);

HitagSAc2kQuality hitag_s_codec_ac2k_quality(const HitagSEdge* edges, size_t edge_count);

bool hitag_s_codec_is_valid_ac2k_uid_capture(
    size_t bits,
    const HitagSEdge* edges,
    size_t edge_count);

bool hitag_s_codec_is_marginal_ac2k_uid_quality(size_t bits, const HitagSAc2kQuality* quality);

bool hitag_s_codec_is_low_entropy_uid(uint32_t uid);

bool hitag_s_codec_is_acceptable_start01_uid(uint32_t uid, size_t votes, size_t partial_support);

#ifdef __cplusplus
}
#endif
