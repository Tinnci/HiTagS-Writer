/**
 * @file hitag_s_proto.h
 * @brief Hitag S protocol implementation for 8268 magic chips
 *
 * Implements BPLM (Binary Pulse Length Modulation) TX and Manchester RX
 * for communicating with Hitag S compatible 8268/F8268/F8278 chips.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Hitag S command opcodes --- */
#define HITAG_S_UID_REQ_STD  0x30  /* 00110 (5 bits) */
#define HITAG_S_UID_REQ_ADV1 0xC8  /* 11001 (5 bits) */
#define HITAG_S_UID_REQ_ADV2 0xC0  /* 11000 (5 bits) */
#define HITAG_S_UID_REQ_FADV 0xD0  /* 11010 (5 bits) */
#define HITAG_S_SELECT       0x00  /* 00000 (5 bits) + 32bit UID + 8bit CRC */
#define HITAG_S_READ_PAGE    0xC0  /* 1100 (4 bits) + 8bit page + 8bit CRC */
#define HITAG_S_WRITE_PAGE   0x80  /* 1000 (4 bits) + 8bit page + 8bit CRC */
#define HITAG_S_READ_BLOCK   0xD0  /* 1101 (4 bits) + 8bit block + 8bit CRC */
#define HITAG_S_WRITE_BLOCK  0x90  /* 1001 (4 bits) + 8bit block + 8bit CRC */
#define HITAG_S_QUIET        0x70

/* --- 8268 magic chip constants --- */
#define HITAG_S_8268_PASSWORD     0xBBDD3399UL
#define HITAG_S_8268_AUTH_PAGE    64  /* Page 0x40 — write password here to authenticate */
#define HITAG_S_8268_PASSWORD_ALT 0xAAAAAAAAUL

/* --- BPLM timing constants (in microseconds) ---
 * Based on Proxmark3 Hitag S implementation.
 * T0 = 8 µs (one carrier cycle at 125 kHz).
 */
#define HITAG_S_T0_US             8   /* Base time unit */
#define HITAG_S_T_LOW_CYCLES      6   /* Gap low duration in T0 cycles */
#define HITAG_S_T_0_CYCLES        18  /* Bit '0' total duration in T0 cycles */
#define HITAG_S_T_1_CYCLES        28  /* Bit '1' total duration in T0 cycles */
#define HITAG_S_T_STOP_CYCLES     40  /* Stop/EOF duration in T0 cycles */
#define HITAG_S_T_WAIT_POWERUP_US 2500 /* Power-up wait time (312.5 × T0) */
#define HITAG_S_T_WAIT_SC_US      700  /* Standard command wait (~87.5 × T0) */
#define HITAG_S_T_WAIT_RESP_US    200  /* Wait for tag response */
#define HITAG_S_T_PROG_US         5600 /* Program time after write (700 × T0) */

/* --- Manchester decoding thresholds (in timer ticks at capture resolution) --- */
#define HITAG_S_MC_SHORT_MIN_US   100  /* Min short pulse */
#define HITAG_S_MC_SHORT_MAX_US   280  /* Max short pulse */
#define HITAG_S_MC_LONG_MIN_US    300  /* Min long pulse */
#define HITAG_S_MC_LONG_MAX_US    500  /* Max long pulse */

/* --- Tag memory pages --- */
#define HITAG_S_PAGE_UID    0
#define HITAG_S_PAGE_CONFIG 1
#define HITAG_S_PAGE_PWD    2
#define HITAG_S_PAGE_KEY    3
#define HITAG_S_PAGE_DATA   4

/* --- ACK value --- */
#define HITAG_S_ACK 0x01  /* 2-bit ACK = 01 */

/* --- Maximum pages for data --- */
#define HITAG_S_MAX_PAGES 64

/* --- Result codes --- */
typedef enum {
    HitagSResultOk,
    HitagSResultTimeout,
    HitagSResultNack,
    HitagSResultError,
    HitagSResultCrcError,
} HitagSResult;

/**
 * @brief Calculate CRC-8 for Hitag S (polynomial 0x1D, init 0xFF)
 * @param data  Pointer to data bits (MSB first, packed in bytes)
 * @param bits  Number of bits to CRC
 * @return CRC-8 value
 */
uint8_t hitag_s_crc8(const uint8_t* data, size_t bits);

/**
 * @brief Send a BPLM-encoded bit frame to the tag
 * @param data  Packed bits, MSB first
 * @param bits  Number of bits to send
 */
void hitag_s_send_frame(const uint8_t* data, size_t bits);

/**
 * @brief Receive Manchester-encoded response from tag
 * @param data      Buffer to store received bits (packed, MSB first)
 * @param max_bits  Maximum bits to receive
 * @param timeout_us Timeout in microseconds
 * @return Number of bits actually received, 0 on timeout/error
 */
size_t hitag_s_receive_frame(uint8_t* data, size_t max_bits, uint32_t timeout_us);

/**
 * @brief Start 125 kHz carrier for Hitag S communication
 */
void hitag_s_field_on(void);

/**
 * @brief Stop 125 kHz carrier
 */
void hitag_s_field_off(void);

/**
 * @brief Build and send UID request (UID_REQ_ADV1, 5 bits)
 * @param uid  Pointer to store 32-bit UID on success
 * @return HitagSResult
 */
HitagSResult hitag_s_uid_request(uint32_t* uid);

/**
 * @brief Build and send SELECT command
 * @param uid     32-bit UID to select
 * @param config  Pointer to store 32-bit config page (page 1) on success
 * @return HitagSResult
 */
HitagSResult hitag_s_select(uint32_t uid, uint32_t* config);

/**
 * @brief Authenticate to 8268 chip by writing password to page 64
 * @param password  32-bit password (default 0xBBDD3399)
 * @return HitagSResult
 */
HitagSResult hitag_s_8268_authenticate(uint32_t password);

/**
 * @brief Write a single page to the tag (after authentication)
 * @param page  Page address (0-63)
 * @param data  32-bit data to write
 * @return HitagSResult
 */
HitagSResult hitag_s_write_page(uint8_t page, uint32_t data);

/**
 * @brief Read a single page from the tag (after authentication)
 * @param page  Page address
 * @param data  Pointer to store 32-bit page data
 * @return HitagSResult
 */
HitagSResult hitag_s_read_page(uint8_t page, uint32_t* data);

/**
 * @brief Full sequence: read UID from 8268 tag
 * @param uid  Pointer to store 32-bit UID
 * @return HitagSResult
 */
HitagSResult hitag_s_read_uid_sequence(uint32_t* uid);

/**
 * @brief Full write sequence for 8268: UID request → SELECT → Auth → Write pages
 * @param password    Authentication password
 * @param pages       Array of page data to write
 * @param page_addrs  Array of page addresses
 * @param page_count  Number of pages to write
 * @return HitagSResult
 */
HitagSResult hitag_s_8268_write_sequence(
    uint32_t password,
    const uint32_t* pages,
    const uint8_t* page_addrs,
    size_t page_count);

/**
 * @brief Full read sequence for 8268: UID request → SELECT → Auth → Read pages
 * @param password    Authentication password
 * @param pages       Array to store page data
 * @param page_addrs  Array of page addresses to read
 * @param page_count  Number of pages to read
 * @param uid_out     Optional: pointer to store tag UID (may be NULL)
 * @return HitagSResult
 */
HitagSResult hitag_s_8268_read_sequence(
    uint32_t password,
    uint32_t* pages,
    const uint8_t* page_addrs,
    size_t page_count,
    uint32_t* uid_out);

#ifdef __cplusplus
}
#endif
