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
#include <string.h>
#include "em4100_encode.h"

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
#define HITAG_S_8268_PASSWORD_ALT 0x4D494B52UL  /* "MIKR" — alternate 82xx password */

/* --- Config page bitfield structure (matches PM3 hitags_config_t) ---
 * Config page (page 1) is 4 bytes on wire, MSB first:
 *   Byte 0 = CON0, Byte 1 = CON1, Byte 2 = CON2, Byte 3 = PWDH0
 *
 * As uint32_t (big-endian on wire):
 *   bits [31:24] = CON0, [23:16] = CON1, [15:8] = CON2, [7:0] = PWDH0
 */
typedef struct {
    /* CON0 — byte 0 (bits 31:24) */
    uint8_t MEMT : 2;   /* Memory type: 00=32pg, 01=8pg, 10=unused, 11=64pg */
    uint8_t RES0 : 1;   /* For 82xx: extended TTF mode (combined with TTFM) */
    uint8_t RES1 : 1;
    uint8_t RES2 : 1;
    uint8_t RES3 : 1;   /* For 82xx: TTF FSK mode (0=RF/10, 1=RF/8) */
    uint8_t RES4 : 1;
    uint8_t RES5 : 1;

    /* CON1 — byte 1 (bits 23:16) */
    uint8_t LKP  : 1;   /* Lock page 2/3: 0=RW, 1=RO(plain)/no-access(auth) */
    uint8_t LCON : 1;   /* Lock config: 0=RW, 1=CON1 RO + CON2 OTP */
    uint8_t TTFM : 2;   /* TTF page count: 00=1pg, 01=2pg, 10=4pg, 11=8pg */
    uint8_t TTFDR: 2;   /* TTF data rate: 00=4kBps, 01=8kBps, 10=2kBps, 11=2kBps */
    uint8_t TTFC : 1;   /* TTF coding: 0=Manchester, 1=Biphase */
    uint8_t auth : 1;   /* Auth mode: 0=Plain, 1=Authentication required */

    /* CON2 — byte 2 (bits 15:8), page lock bits (0=RW, 1=RO) */
    uint8_t LCK0 : 1;   /* Pages 48-63 */
    uint8_t LCK1 : 1;   /* Pages 32-47 */
    uint8_t LCK2 : 1;   /* Pages 24-31 */
    uint8_t LCK3 : 1;   /* Pages 16-23 */
    uint8_t LCK4 : 1;   /* Pages 12-15 */
    uint8_t LCK5 : 1;   /* Pages 8-11 */
    uint8_t LCK6 : 1;   /* Pages 6-7 */
    uint8_t LCK7 : 1;   /* Pages 4-5 */

    /* PWDH0 — byte 3 (bits 7:0) */
    uint8_t pwdh0;
} __attribute__((packed)) HitagSConfig;

/**
 * @brief Parse config page uint32_t into bitfield struct
 * @param config_val  32-bit config page value (big-endian wire order)
 * @return Parsed config struct
 */
static inline HitagSConfig hitag_s_parse_config(uint32_t config_val) {
    HitagSConfig cfg;
    uint8_t bytes[4] = {
        (uint8_t)(config_val >> 24),
        (uint8_t)(config_val >> 16),
        (uint8_t)(config_val >> 8),
        (uint8_t)(config_val)
    };
    memcpy(&cfg, bytes, 4);
    return cfg;
}

/**
 * @brief Pack config bitfield struct back to uint32_t
 * @param cfg  Config struct
 * @return 32-bit config value
 */
static inline uint32_t hitag_s_pack_config(const HitagSConfig* cfg) {
    uint8_t bytes[4];
    memcpy(bytes, cfg, 4);
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

/**
 * @brief Get max page number from MEMT field
 * @param cfg  Config struct
 * @return Maximum valid page number
 */
static inline int hitag_s_max_page(const HitagSConfig* cfg) {
    switch(cfg->MEMT) {
    case 0: return 32 - 1;  /* 32 pages (256 bit) */
    case 1: return 8 - 1;   /* 8 pages (64 bit) */
    case 3: return 64 - 1;  /* 64 pages (512 bit) — 8268 uses this */
    default: return 64 - 1; /* Unknown, assume max */
    }
}

/**
 * @brief Check if a page is locked (read-only) based on CON2 lock bits
 * @param cfg   Config struct
 * @param page  Page address to check
 * @return true if the page is locked (write-protected)
 */
static inline bool hitag_s_page_locked(const HitagSConfig* cfg, uint8_t page) {
    if(page <= 3) return false;  /* Pages 0-3 are system — 82xx can write page 0 */
    if(page <= 5) return cfg->LCK7;
    if(page <= 7) return cfg->LCK6;
    if(page <= 11) return cfg->LCK5;
    if(page <= 15) return cfg->LCK4;
    if(page <= 23) return cfg->LCK3;
    if(page <= 31) return cfg->LCK2;
    if(page <= 47) return cfg->LCK1;
    if(page <= 63) return cfg->LCK0;
    return true;  /* Beyond max — 82xx page 64 is auth, not data */
}

/* --- BPLM timing constants (in microseconds) ---
 * Based on Proxmark3 Hitag S implementation.
 * T0 = 8 µs (one carrier cycle at 125 kHz).
 */
#define HITAG_S_T0_US             8   /* Base time unit */
#define HITAG_S_T_LOW_CYCLES      8   /* Gap low duration in T0 cycles (spec: 4..10, PM3: 8) */
#define HITAG_S_T_0_CYCLES        20  /* Bit '0' total duration in T0 cycles (spec: 18..22, PM3: 20) */
#define HITAG_S_T_1_CYCLES        28  /* Bit '1' total duration in T0 cycles (spec: 26..32, PM3: 28) */
#define HITAG_S_T_STOP_CYCLES     40  /* Stop/EOF duration in T0 cycles (spec: >36) */
#define HITAG_S_T_WAIT_POWERUP_US 2500 /* Power-up wait time (312.5 × T0) */
#define HITAG_S_T_WAIT_SC_US      1600 /* Standard command wait (200 × T0, spec: 90..5000) */
#define HITAG_S_T_WAIT_RESP_US    200  /* Wait for tag response */
#define HITAG_S_T_PROG_US         6000 /* Program time after write (750 × T0, spec: 716..726) */

/* --- MC4K Manchester decoding (post-SELECT data exchange) ---
 * Half-bit = 128µs = 16 × T0, Full-bit = 256µs = 32 × T0 */
#define HITAG_S_MC4K_THRESHOLD_US  192  /* Midpoint: SHORT ~128µs / LONG ~256µs */
#define HITAG_S_MC4K_GLITCH_US     50   /* Min valid pulse duration */

/* --- AC2K Anti-collision decoding (UID response) ---
 * Half = 256µs = 32 × T0, based on 16 × T0 quarter periods */
#define HITAG_S_AC2K_THRESH_23_US  320  /* Between 2-half (256µs) and 3-half (384µs) */
#define HITAG_S_AC2K_THRESH_34_US  448  /* Between 3-half (384µs) and 4-half (512µs) */
#define HITAG_S_AC2K_GLITCH_US     80   /* Min valid period */

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
 * @brief Full EM4100 write sequence: UID → SELECT → Auth → Read config →
 *        Modify TTF → Write config → Write EM4100 data pages 4,5
 * @param password    Authentication password
 * @param em_data     Prepared EM4100 data (pages 4 and 5)
 * @param config_out  Optional: pointer to store final config value (may be NULL)
 * @return HitagSResult
 */
HitagSResult hitag_s_8268_write_em4100_sequence(
    uint32_t password,
    const Em4100HitagData* em_data,
    uint32_t* config_out);

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

/**
 * @brief Write a page with readback verification
 * @param page  Page address
 * @param data  32-bit data to write
 * @return HitagSResult (HitagSResultError if verify mismatch)
 */
HitagSResult hitag_s_write_page_verify(uint8_t page, uint32_t data);

/**
 * @brief Authenticate with multiple passwords (tries default, then alternate)
 * @param passwords   NULL-terminated array of password values to try, or NULL for defaults
 * @param count       Number of passwords (0 = use built-in defaults)
 * @return HitagSResult
 */
HitagSResult hitag_s_8268_authenticate_multi(const uint32_t* passwords, size_t count);

/**
 * @brief Read all accessible pages from 8268 tag (full dump)
 * @param password    Authentication password (0 = try defaults)
 * @param pages       Output array of at least 64 uint32_t
 * @param page_valid  Output array of at least 64 bool (which pages were read)
 * @param max_page    Output: detected max page number (may be NULL)
 * @param uid_out     Output: tag UID (may be NULL)
 * @return HitagSResult
 */
HitagSResult hitag_s_8268_read_all(
    uint32_t password,
    uint32_t* pages,
    bool* page_valid,
    int* max_page,
    uint32_t* uid_out);

/**
 * @brief Full clone sequence: write UID (page 0) + config + data pages
 *
 * 8268 magic chips allow writing page 0 (UID), which normal Hitag S tags
 * don't allow. This enables full tag cloning.
 *
 * @param password    Authentication password
 * @param new_uid     New 32-bit UID to write to page 0
 * @param config      New config page value for page 1
 * @param data_pages  Array of data page values (pages 4+)
 * @param data_addrs  Array of data page addresses
 * @param data_count  Number of data pages
 * @return HitagSResult
 */
HitagSResult hitag_s_8268_clone_sequence(
    uint32_t password,
    uint32_t new_uid,
    uint32_t config,
    const uint32_t* data_pages,
    const uint8_t* data_addrs,
    size_t data_count);

/**
 * @brief Write UID to 8268 magic chip (page 0)
 *
 * Normal Hitag S tags have read-only page 0 (UID). 82xx magic chips
 * allow writing page 0, enabling UID cloning.
 *
 * @param new_uid  New 32-bit UID value
 * @return HitagSResult
 */
HitagSResult hitag_s_write_uid(uint32_t new_uid);

/**
 * @brief Check if a page is write-accessible based on config lock bits
 * @param config_val  Config page value
 * @param page        Page number to check
 * @return true if writable
 */
bool hitag_s_page_writable(uint32_t config_val, uint8_t page);

#ifdef __cplusplus
}
#endif
