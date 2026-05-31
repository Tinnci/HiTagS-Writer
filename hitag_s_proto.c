/**
 * @file hitag_s_proto.c
 * @brief Compatibility facade for shared HiTag S protocol helpers.
 */

#include "hitag_s_proto.h"
#include "hitag_s_codec.h"

uint8_t hitag_s_crc8(const uint8_t* data, size_t bits) {
    return hitag_s_codec_crc8(data, bits);
}
