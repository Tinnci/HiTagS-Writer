#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "hitag_s_codec.h"

static void test_select_frame_matches_trace_crc_and_bit_packing(void) {
    uint8_t frame[6] = {0};
    size_t bits = 0;
    uint8_t crc = hitag_s_codec_build_select_frame(frame, &bits, 0x52810231);
    const uint8_t expected[6] = {0x02, 0x94, 0x08, 0x11, 0x8B, 0x78};

    assert(bits == 45);
    assert(crc == 0x6F);
    assert(memcmp(frame, expected, sizeof(expected)) == 0);
}

static void test_select_uid_variants_are_stable_and_deduplicated(void) {
    HitagSUidVariant variants[HITAG_S_UID_VARIANT_MAX] = {0};
    size_t count = hitag_s_codec_uid_variants(0x52810231, variants, HITAG_S_UID_VARIANT_MAX);

    assert(count == 4);
    assert(variants[0].uid == 0x52810231);
    assert(variants[1].uid == 0x31028152);
    assert(variants[2].uid == 0x4A81408C);
    assert(variants[3].uid == 0x8C40814A);
}

static void test_select_frame_bplm_duration_matches_transport_timing(void) {
    uint8_t frame[6] = {0};
    size_t bits = 0;
    hitag_s_codec_build_select_frame(frame, &bits, 0x52810231);

    assert(hitag_s_codec_bplm_frame_duration_us(frame, bits) == 8512);
}

int main(void) {
    test_select_frame_matches_trace_crc_and_bit_packing();
    test_select_uid_variants_are_stable_and_deduplicated();
    test_select_frame_bplm_duration_matches_transport_timing();
    return 0;
}
