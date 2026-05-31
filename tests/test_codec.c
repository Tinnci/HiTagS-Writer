#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "hitag_s_codec.h"

static void test_ac2k_uid_quality_rejects_long_gap_noise(void) {
    const HitagSEdge edges[] = {
        {false, 256},
        {false, 384},
        {false, 1200},
        {false, 256},
    };

    HitagSAc2kQuality quality = hitag_s_codec_ac2k_quality(edges, 4);
    assert(quality.long_ac_periods == 1);
    assert(!quality.too_noisy);

    const HitagSEdge noisy_edges[] = {
        {false, 256},
        {false, 1200},
        {false, 900},
        {false, 256},
    };
    quality = hitag_s_codec_ac2k_quality(noisy_edges, 4);
    assert(quality.long_ac_periods == 2);
    assert(quality.too_noisy);
}

static void test_valid_uid_capture_requires_32_bits_and_clean_quality(void) {
    const HitagSEdge clean_edges[] = {
        {false, 256},
        {false, 384},
        {false, 512},
        {false, 256},
    };
    const HitagSEdge noisy_edges[] = {
        {false, 256},
        {false, 40},
        {false, 50},
        {false, 60},
        {false, 256},
    };

    assert(hitag_s_codec_is_valid_ac2k_uid_capture(32, clean_edges, 4));
    assert(!hitag_s_codec_is_valid_ac2k_uid_capture(31, clean_edges, 4));
    assert(!hitag_s_codec_is_valid_ac2k_uid_capture(32, noisy_edges, 5));
}

static void test_ac2k_quality_ignores_startup_glitch_before_response_body(void) {
    const HitagSEdge edges[] = {
        {false, 5},
        {false, 1200},
        {false, 7},
        {false, 256},
        {false, 512},
        {false, 256},
        {false, 512},
    };

    HitagSAc2kQuality quality = hitag_s_codec_ac2k_quality(edges, 7);
    assert(quality.glitches == 1);
    assert(quality.long_ac_periods == 1);
    assert(!quality.too_noisy);
    assert(hitag_s_codec_is_valid_ac2k_uid_capture(32, edges, 7));
    assert(!hitag_s_codec_is_marginal_ac2k_uid_quality(31, &quality));
}

static void test_start01_fallback_rejects_low_entropy_uid_candidates(void) {
    assert(hitag_s_codec_is_low_entropy_uid(0x00000000));
    assert(hitag_s_codec_is_low_entropy_uid(0x40000000));
    assert(hitag_s_codec_is_low_entropy_uid(0x60000000));
    assert(hitag_s_codec_is_low_entropy_uid(0x80000000));
    assert(hitag_s_codec_is_low_entropy_uid(0xFFFFFFFF));
    assert(!hitag_s_codec_is_low_entropy_uid(0x52810231));

    assert(!hitag_s_codec_is_acceptable_start01_uid(0x40000000, 8, 8));
    assert(!hitag_s_codec_is_acceptable_start01_uid(0x52810231, 7, 8));
    assert(!hitag_s_codec_is_acceptable_start01_uid(0x52810231, 8, 3));
    assert(hitag_s_codec_is_acceptable_start01_uid(0x52810231, 8, 4));
}

int main(void) {
    test_ac2k_uid_quality_rejects_long_gap_noise();
    test_valid_uid_capture_requires_32_bits_and_clean_quality();
    test_ac2k_quality_ignores_startup_glitch_before_response_body();
    test_start01_fallback_rejects_low_entropy_uid_candidates();
    return 0;
}
