#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "em4100_encode.h"

static void test_decode_rejects_bad_row_parity(void) {
    const uint8_t id[5] = {0x00, 0x00, 0x00, 0x20, 0x4C};
    uint64_t frame = em4100_encode(id);
    uint64_t corrupted = frame ^ (1ULL << 50); /* first row parity bit */
    uint8_t decoded[5] = {0};

    assert(!em4100_decode_hitag_data((uint32_t)(corrupted >> 32), (uint32_t)corrupted, decoded));
}

static void test_decode_rejects_bad_stop_bit(void) {
    const uint8_t id[5] = {0x00, 0x00, 0x00, 0x20, 0x4C};
    uint64_t frame = em4100_encode(id);
    uint8_t decoded[5] = {0};

    assert(!em4100_decode_hitag_data((uint32_t)(frame >> 32), (uint32_t)(frame | 1), decoded));
}

static void test_decode_round_trips_valid_frame(void) {
    const uint8_t id[5] = {0x01, 0x02, 0x03, 0x04, 0x05};
    uint64_t frame = em4100_encode(id);
    uint8_t decoded[5] = {0};

    assert(em4100_decode_hitag_data((uint32_t)(frame >> 32), (uint32_t)frame, decoded));
    assert(memcmp(id, decoded, sizeof(id)) == 0);
}

static void test_config_set_ttf_preserves_locks_and_sets_em4100_mode(void) {
    uint32_t current = 0xFF03AA55;
    uint32_t updated = em4100_config_set_ttf(current);

    assert(((updated >> 24) & 0xFF) == 0xFB); /* CON0 RES0 cleared, other bits preserved */
    assert(((updated >> 16) & 0xFF) == 0x27); /* auth=0 TTFC=0 TTFDR=10 TTFM=01 LCON/LKP kept */
    assert((updated & 0xFFFF) == 0xAA55); /* CON2/PWDH0 preserved */
}

static void test_config_make_8268_ttf_matches_proxmark_profile(void) {
    assert(em4100_config_make_8268_ttf(0xFFFFFFFF) == 0xDAA40000);
}

int main(void) {
    test_decode_rejects_bad_row_parity();
    test_decode_rejects_bad_stop_bit();
    test_decode_round_trips_valid_frame();
    test_config_set_ttf_preserves_locks_and_sets_em4100_mode();
    test_config_make_8268_ttf_matches_proxmark_profile();
    return 0;
}
