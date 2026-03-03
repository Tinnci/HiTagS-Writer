/**
 * @file em4100_encode.c
 * @brief EM4100 encoding for HiTag S 8268 chips
 */

#include "em4100_encode.h"
#include <furi.h>
#include <string.h>
#include <stdio.h>

#define TAG "EM4100"

/* EM4100 encoding constants */
#define EM41XX_LINES   10
#define EM41XX_COLUMNS 4
#define EM41XX_HEADER  0x1FFULL /* 9 bits of 1s */

/**
 * @brief HiTag S page 1 config for EM4100 TTF emulation:
 *   CON0 = 0x48: MEMT=01 (256 pages), LCON=0, LKP=0
 *   CON1 = 0x00: auth=0, no lock bits
 *   CON2 = 0x00
 *   PWDH0 = 0x00
 *
 * Note: For EM4100 emulation, the 8268 config page should set:
 *   - TTFM = Manchester
 *   - TTFDR = RF/64 (same as EM4100)
 *   - TTF enabled
 *   - auth bit can be left set (we already authenticated)
 *
 * Observed default config: 0xDAA40000
 * We may need to modify this based on actual chip behavior.
 * For now we write the config that enables EM4100-compatible output.
 */
#define HITAG_S_EM4100_CONFIG 0x48000000UL

static bool get_parity(uint16_t data) {
    bool result = false;
    for(int i = 0; i < 16; i++) {
        result ^= ((data >> i) & 1);
    }
    return result;
}

static bool get_line_parity_bit(uint8_t line_num, uint64_t data) {
    uint8_t line = (data >> (EM41XX_COLUMNS * line_num)) & 0x0F;
    return get_parity(line);
}

static bool get_column_parity_bit(uint8_t column_num, uint64_t data) {
    uint16_t column = 0;
    for(int i = 0; i < EM41XX_LINES; i++) {
        column <<= 1;
        column |= (data >> (EM41XX_COLUMNS * i + column_num)) & 1;
    }
    return get_parity(column);
}

uint64_t em4100_encode(const uint8_t* id_bytes) {
    /* Convert 5 bytes to 40-bit value */
    uint64_t data = 0;
    for(int i = 0; i < 5; i++) {
        data = (data << 8) | id_bytes[i];
    }

    /* Build the 64-bit Manchester-encoded EM4100 frame:
     * [9 header bits] [10 rows × (4 data + 1 parity)] [4 column parity] [1 stop]
     */
    uint64_t result = EM41XX_HEADER; /* 9 header bits: 111111111 */

    /* 10 data rows, 4 bits each + row parity, MSB row first */
    for(int i = EM41XX_LINES - 1; i >= 0; i--) {
        result <<= EM41XX_COLUMNS;
        uint8_t line = (data >> (i * EM41XX_COLUMNS)) & 0x0F;
        result |= line;

        result <<= 1;
        result |= get_line_parity_bit(i, data);
    }

    /* 4 column parity bits */
    for(int i = EM41XX_COLUMNS - 1; i >= 0; i--) {
        result <<= 1;
        result |= get_column_parity_bit(i, data);
    }

    /* 1 stop bit (0) */
    result <<= 1;

    FURI_LOG_D(TAG, "Encoded EM4100: %016llX", (unsigned long long)result);
    return result;
}

void em4100_prepare_hitag_data(const uint8_t* id_bytes, Em4100HitagData* out_data) {
    uint64_t encoded = em4100_encode(id_bytes);

    /* Split 64-bit encoded data into two 32-bit pages */
    out_data->data_hi = (uint32_t)(encoded >> 32);
    out_data->data_lo = (uint32_t)(encoded & 0xFFFFFFFF);
    out_data->config_page = HITAG_S_EM4100_CONFIG;

    FURI_LOG_I(
        TAG,
        "HiTag data prepared: config=%08lX hi=%08lX lo=%08lX",
        (unsigned long)out_data->config_page,
        (unsigned long)out_data->data_hi,
        (unsigned long)out_data->data_lo);
}

void em4100_id_to_string(const uint8_t* id_bytes, char* out_str) {
    snprintf(
        out_str,
        15,
        "%02X:%02X:%02X:%02X:%02X",
        id_bytes[0],
        id_bytes[1],
        id_bytes[2],
        id_bytes[3],
        id_bytes[4]);
}

bool em4100_string_to_id(const char* hex_str, uint8_t* id_bytes) {
    if(strlen(hex_str) != 10) return false;

    for(int i = 0; i < 5; i++) {
        char byte_str[3] = {hex_str[i * 2], hex_str[i * 2 + 1], '\0'};
        char* end;
        unsigned long val = strtoul(byte_str, &end, 16);
        if(*end != '\0') return false;
        id_bytes[i] = (uint8_t)val;
    }

    return true;
}
