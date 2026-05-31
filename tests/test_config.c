#include <assert.h>
#include <string.h>

#include "hitag_s_proto.h"

static void test_parse_proxmark_em4100_ttf_profile(void) {
    HitagSConfig cfg = hitag_s_parse_config(0xDAA40000);

    assert(cfg.MEMT == 2);
    assert(cfg.RES0 == 0);
    assert(cfg.RES1 == 1);
    assert(cfg.RES2 == 1);
    assert(cfg.RES3 == 0);
    assert(cfg.RES4 == 1);
    assert(cfg.RES5 == 1);
    assert(cfg.auth == 1);
    assert(cfg.TTFC == 0);
    assert(cfg.TTFDR == 2);
    assert(cfg.TTFM == 1);
    assert(cfg.LCON == 0);
    assert(cfg.LKP == 0);
    assert(hitag_s_pack_config(&cfg) == 0xDAA40000);
}

static void test_lock_bits_map_to_expected_page_ranges(void) {
    HitagSConfig cfg = hitag_s_parse_config(0x00008000);

    assert(hitag_s_page_locked(&cfg, 4));
    assert(hitag_s_page_locked(&cfg, 5));
    assert(!hitag_s_page_locked(&cfg, 6));
}

static void test_wipe_config_pack_is_plain_64_page_default_with_password_high_byte(void) {
    HitagSConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.MEMT = 3;
    cfg.pwdh0 = 0xBB;

    assert(hitag_s_pack_config(&cfg) == 0x030000BB);
}

int main(void) {
    test_parse_proxmark_em4100_ttf_profile();
    test_lock_bits_map_to_expected_page_ranges();
    test_wipe_config_pack_is_plain_64_page_default_with_password_high_byte();
    return 0;
}
