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

/* TTF configuration bits in CON1 byte (byte 1 of config page):
 *   auth[7] TTFC[6] TTFDR[5:4] TTFM[3:2] LCON[1] LKP[0]
 *
 * For EM4100 emulation:
 *   TTFC  = 0  (Manchester coding)
 *   TTFDR = 10 (2 kBit/s = fc/64, same as EM4100)
 *   TTFM  = 01 (broadcast pages 4-5, total 64 bits)
 *
 * CON0 byte: RES5[7] RES4[6] RES3[5] RES2[4] RES1[3] RES0[2] MEMT[1:0]
 *   RES0 must be 0 for standard TTF mode (combined with TTFM).
 */
#define EM4100_TTF_CON1_MASK     0xFC /* Bits to modify: auth, TTFC, TTFDR, TTFM */
#define EM4100_TTF_CON1_VALUE    0x24 /* 0b00100100: auth=0, TTFC=0, TTFDR=10, TTFM=01 */
#define EM4100_TTF_CON0_RES0_BIT 0x04 /* RES0 = bit 2 of CON0, must be 0 */

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

    FURI_LOG_I(
        TAG,
        "HiTag data prepared: hi=%08lX lo=%08lX",
        (unsigned long)out_data->data_hi,
        (unsigned long)out_data->data_lo);
}

uint32_t em4100_config_set_ttf(uint32_t current_config) {
    /* Config page is 4 bytes, MSB first on wire:
     * Byte 0 = CON0, Byte 1 = CON1, Byte 2 = CON2, Byte 3 = PWDH0
     * As uint32_t (big-endian on wire): CON0 is bits [31:24], CON1 is bits [23:16] */

    uint8_t con0 = (current_config >> 24) & 0xFF;
    uint8_t con1 = (current_config >> 16) & 0xFF;

    /* Clear RES0 bit in CON0 (required for standard TTFM interpretation) */
    con0 &= ~EM4100_TTF_CON0_RES0_BIT;

    /* Modify CON1: set TTF fields, preserve LCON and LKP */
    con1 = (con1 & ~EM4100_TTF_CON1_MASK) | (EM4100_TTF_CON1_VALUE & EM4100_TTF_CON1_MASK);

    uint32_t new_config = (current_config & 0x0000FFFF) | ((uint32_t)con0 << 24) |
                          ((uint32_t)con1 << 16);

    FURI_LOG_I(
        TAG,
        "Config TTF set: %08lX -> %08lX (CON0=%02X CON1=%02X)",
        (unsigned long)current_config,
        (unsigned long)new_config,
        con0,
        con1);

    return new_config;
}

bool em4100_decode_hitag_data(uint32_t data_hi, uint32_t data_lo, uint8_t* id_bytes) {
    /* Reconstruct the 64-bit EM4100 frame from pages 4 and 5 */
    uint64_t frame = ((uint64_t)data_hi << 32) | (uint64_t)data_lo;

    /* EM4100 frame format (64 bits):
     * [9 header 1s] [row9: 4data+1parity] ... [row0: 4data+1parity] [4 col parity] [1 stop]
     * Total: 9 + 10*5 + 4 + 1 = 64 bits
     *
     * Verify header (top 9 bits should all be 1) */
    if((frame >> 55) != 0x1FF) {
        FURI_LOG_W(TAG, "EM4100 decode: invalid header %03llX", (unsigned long long)(frame >> 55));
        memset(id_bytes, 0, 5);
        return false;
    }

    /* Extract 40 data bits: skip header, extract 4 data bits per row, skip parity */
    uint64_t data_40 = 0;
    for(int row = EM41XX_LINES - 1; row >= 0; row--) {
        /* Each row is 5 bits (4 data + 1 parity), rows stored MSB-row first after header
         * Bit position: 55 - (EM41XX_LINES - 1 - row) * 5 - 1 ... for 4 data bits */
        int row_start = 55 - (EM41XX_LINES - 1 - row) * 5 - 1; /* first data bit of row */
        uint8_t nibble = 0;
        for(int b = 0; b < EM41XX_COLUMNS; b++) {
            nibble <<= 1;
            nibble |= (frame >> (row_start - b)) & 1;
        }
        data_40 |= ((uint64_t)nibble << (row * 4));
    }

    /* Convert 40-bit value to 5 bytes */
    for(int i = 0; i < 5; i++) {
        id_bytes[i] = (uint8_t)(data_40 >> (32 - i * 8));
    }

    FURI_LOG_I(
        TAG,
        "Decoded EM4100: %02X:%02X:%02X:%02X:%02X",
        id_bytes[0],
        id_bytes[1],
        id_bytes[2],
        id_bytes[3],
        id_bytes[4]);

    return true;
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
