/**
 * @file em4100_encode.h
 * @brief EM4100 encoding for HiTag S 8268 chips
 *
 * Encodes a 5-byte (40-bit) EM4100 card ID into the format needed
 * to program a HiTag S 8268 chip to emulate an EM4100 card.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief EM4100 data ready to be written to HiTag S pages
 */
typedef struct {
    uint32_t config_page; /**< HiTag S page 1 config for EM4100 emulation */
    uint32_t data_hi;     /**< Upper 32 bits of Manchester-encoded EM4100 data */
    uint32_t data_lo;     /**< Lower 32 bits of Manchester-encoded EM4100 data */
} Em4100HitagData;

/**
 * @brief Encode a 40-bit EM4100 ID into the 64-bit Manchester format
 * @param id_bytes  5 bytes of EM4100 ID (MSB first: version + card number)
 * @return 64-bit Manchester-encoded EM4100 data
 */
uint64_t em4100_encode(const uint8_t* id_bytes);

/**
 * @brief Prepare EM4100 data for writing to HiTag S 8268 chip
 *
 * This prepares the config page and two data pages needed to make
 * the 8268 chip broadcast as an EM4100 card.
 *
 * @param id_bytes   5 bytes of EM4100 ID
 * @param out_data   Pointer to output structure
 */
void em4100_prepare_hitag_data(const uint8_t* id_bytes, Em4100HitagData* out_data);

/**
 * @brief Convert 5 EM4100 ID bytes to display string "XX:XX:XX:XX:XX"
 * @param id_bytes  5 bytes of EM4100 ID
 * @param out_str   Output buffer (at least 15 bytes)
 */
void em4100_id_to_string(const uint8_t* id_bytes, char* out_str);

/**
 * @brief Convert hex string "XXXXXXXXXX" to 5 EM4100 ID bytes
 * @param hex_str   10-character hex string
 * @param id_bytes  Output 5 bytes
 * @return true on success
 */
bool em4100_string_to_id(const char* hex_str, uint8_t* id_bytes);

#ifdef __cplusplus
}
#endif
