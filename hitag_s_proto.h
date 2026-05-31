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
#include "hitag_s_codec.h"
#include "em4100_encode.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- Hitag S command opcodes --- */
#define HITAG_S_UID_REQ_STD  0x30 /* 00110 (5 bits) */
#define HITAG_S_UID_REQ_ADV1 0xC8 /* 11001 (5 bits) */
#define HITAG_S_UID_REQ_ADV2 0xC0 /* 11000 (5 bits) */
#define HITAG_S_UID_REQ_FADV 0xD0 /* 11010 (5 bits) */
#define HITAG_S_SELECT       0x00 /* 00000 (5 bits) + 32bit UID + 8bit CRC */
#define HITAG_S_READ_PAGE    0xC0 /* 1100 (4 bits) + 8bit page + 8bit CRC */
#define HITAG_S_WRITE_PAGE   0x80 /* 1000 (4 bits) + 8bit page + 8bit CRC */
#define HITAG_S_READ_BLOCK   0xD0 /* 1101 (4 bits) + 8bit block + 8bit CRC */
#define HITAG_S_WRITE_BLOCK  0x90 /* 1001 (4 bits) + 8bit block + 8bit CRC */
#define HITAG_S_QUIET        0x70

/* --- 8268 magic chip constants --- */
#define HITAG_S_8268_PASSWORD      0xBBDD3399UL /* Standard 8268 factory default */
#define HITAG_S_8268_AUTH_PAGE     64 /* Page 0x40 — write password here to authenticate */
#define HITAG_S_8268_PASSWORD_ALT1 0x4D494B52UL /* "MIKR" — HiTag 2 default key */
#define HITAG_S_8268_PASSWORD_ALT2 0xAAAAAAAAUL /* Common alternate password */
#define HITAG_S_8268_PASSWORD_ALT3 0x00000000UL /* All zeros */
#define HITAG_S_8268_PASSWORD_ALT4 0xFFFFFFFFUL /* All ones */

/* --- Config page bitfield structure (matches PM3 hitags_config_t) ---
 * Config page (page 1) is 4 bytes on wire, MSB first:
 *   Byte 0 = CON0, Byte 1 = CON1, Byte 2 = CON2, Byte 3 = PWDH0
 *
 * As uint32_t (big-endian on wire):
 *   bits [31:24] = CON0, [23:16] = CON1, [15:8] = CON2, [7:0] = PWDH0
 */
typedef struct {
    /* CON0 — byte 0 (bits 31:24) */
    uint8_t MEMT  : 2; /* Memory type: 00=32pg, 01=8pg, 10=unused, 11=64pg */
    uint8_t RES0  : 1; /* For 82xx: extended TTF mode (combined with TTFM) */
    uint8_t RES1  : 1;
    uint8_t RES2  : 1;
    uint8_t RES3  : 1; /* For 82xx: TTF FSK mode (0=RF/10, 1=RF/8) */
    uint8_t RES4  : 1;
    uint8_t RES5  : 1;

    /* CON1 — byte 1 (bits 23:16) */
    uint8_t LKP   : 1; /* Lock page 2/3: 0=RW, 1=RO(plain)/no-access(auth) */
    uint8_t LCON  : 1; /* Lock config: 0=RW, 1=CON1 RO + CON2 OTP */
    uint8_t TTFM  : 2; /* TTF page count: 00=1pg, 01=2pg, 10=4pg, 11=8pg */
    uint8_t TTFDR : 2; /* TTF data rate: 00=4kBps, 01=8kBps, 10=2kBps, 11=2kBps */
    uint8_t TTFC  : 1; /* TTF coding: 0=Manchester, 1=Biphase */
    uint8_t auth  : 1; /* Auth mode: 0=Plain, 1=Authentication required */

    /* CON2 — byte 2 (bits 15:8), page lock bits (0=RW, 1=RO) */
    uint8_t LCK0  : 1; /* Pages 48-63 */
    uint8_t LCK1  : 1; /* Pages 32-47 */
    uint8_t LCK2  : 1; /* Pages 24-31 */
    uint8_t LCK3  : 1; /* Pages 16-23 */
    uint8_t LCK4  : 1; /* Pages 12-15 */
    uint8_t LCK5  : 1; /* Pages 8-11 */
    uint8_t LCK6  : 1; /* Pages 6-7 */
    uint8_t LCK7  : 1; /* Pages 4-5 */

    /* PWDH0 — byte 3 (bits 7:0) */
    uint8_t pwdh0;
} __attribute__((packed)) HitagSConfig;

/**
 * @brief Parse config page uint32_t into bitfield struct
 * @param config_val  32-bit config page value (big-endian wire order)
 * @return Parsed config struct
 */
static inline HitagSConfig hitag_s_parse_config(uint32_t config_val) {
    uint8_t con0 = (uint8_t)(config_val >> 24);
    uint8_t con1 = (uint8_t)(config_val >> 16);
    uint8_t con2 = (uint8_t)(config_val >> 8);
    HitagSConfig cfg = {
        .MEMT = con0 & 0x03,
        .RES0 = (con0 >> 2) & 0x01,
        .RES1 = (con0 >> 3) & 0x01,
        .RES2 = (con0 >> 4) & 0x01,
        .RES3 = (con0 >> 5) & 0x01,
        .RES4 = (con0 >> 6) & 0x01,
        .RES5 = (con0 >> 7) & 0x01,
        .LKP = con1 & 0x01,
        .LCON = (con1 >> 1) & 0x01,
        .TTFM = (con1 >> 2) & 0x03,
        .TTFDR = (con1 >> 4) & 0x03,
        .TTFC = (con1 >> 6) & 0x01,
        .auth = (con1 >> 7) & 0x01,
        .LCK0 = con2 & 0x01,
        .LCK1 = (con2 >> 1) & 0x01,
        .LCK2 = (con2 >> 2) & 0x01,
        .LCK3 = (con2 >> 3) & 0x01,
        .LCK4 = (con2 >> 4) & 0x01,
        .LCK5 = (con2 >> 5) & 0x01,
        .LCK6 = (con2 >> 6) & 0x01,
        .LCK7 = (con2 >> 7) & 0x01,
        .pwdh0 = (uint8_t)config_val,
    };
    return cfg;
}

/**
 * @brief Pack config bitfield struct back to uint32_t
 * @param cfg  Config struct
 * @return 32-bit config value
 */
static inline uint32_t hitag_s_pack_config(const HitagSConfig* cfg) {
    uint8_t con0 = (cfg->MEMT & 0x03) | ((cfg->RES0 & 0x01) << 2) | ((cfg->RES1 & 0x01) << 3) |
                   ((cfg->RES2 & 0x01) << 4) | ((cfg->RES3 & 0x01) << 5) |
                   ((cfg->RES4 & 0x01) << 6) | ((cfg->RES5 & 0x01) << 7);
    uint8_t con1 = (cfg->LKP & 0x01) | ((cfg->LCON & 0x01) << 1) | ((cfg->TTFM & 0x03) << 2) |
                   ((cfg->TTFDR & 0x03) << 4) | ((cfg->TTFC & 0x01) << 6) |
                   ((cfg->auth & 0x01) << 7);
    uint8_t con2 = (cfg->LCK0 & 0x01) | ((cfg->LCK1 & 0x01) << 1) | ((cfg->LCK2 & 0x01) << 2) |
                   ((cfg->LCK3 & 0x01) << 3) | ((cfg->LCK4 & 0x01) << 4) |
                   ((cfg->LCK5 & 0x01) << 5) | ((cfg->LCK6 & 0x01) << 6) |
                   ((cfg->LCK7 & 0x01) << 7);
    return ((uint32_t)con0 << 24) | ((uint32_t)con1 << 16) | ((uint32_t)con2 << 8) |
           (uint32_t)cfg->pwdh0;
}

/**
 * @brief Get max page number from MEMT field
 * @param cfg  Config struct
 * @return Maximum valid page number
 */
static inline int hitag_s_max_page(const HitagSConfig* cfg) {
    switch(cfg->MEMT) {
    case 0:
        return 32 - 1; /* 32 pages (256 bit) */
    case 1:
        return 8 - 1; /* 8 pages (64 bit) */
    case 3:
        return 64 - 1; /* 64 pages (512 bit) — 8268 uses this */
    default:
        return 64 - 1; /* Unknown, assume max */
    }
}

/**
 * @brief Check if a page is locked (read-only) based on CON2 lock bits
 * @param cfg   Config struct
 * @param page  Page address to check
 * @return true if the page is locked (write-protected)
 */
static inline bool hitag_s_page_locked(const HitagSConfig* cfg, uint8_t page) {
    if(page <= 3) return false; /* Pages 0-3 are system — 82xx can write page 0 */
    if(page <= 5) return cfg->LCK7;
    if(page <= 7) return cfg->LCK6;
    if(page <= 11) return cfg->LCK5;
    if(page <= 15) return cfg->LCK4;
    if(page <= 23) return cfg->LCK3;
    if(page <= 31) return cfg->LCK2;
    if(page <= 47) return cfg->LCK1;
    if(page <= 63) return cfg->LCK0;
    return true; /* Beyond max — 82xx page 64 is auth, not data */
}

/* --- BPLM timing constants (in microseconds) ---
 * Based on Proxmark3 Hitag S implementation.
 * T0 = 8 µs (one carrier cycle at 125 kHz).
 */
#define HITAG_S_T0_US                   8 /* Base time unit */
#define HITAG_S_T_LOW_CYCLES            8 /* Gap low duration in T0 cycles (spec: 4..10, PM3: 8) */
#define HITAG_S_T_0_CYCLES              20 /* Bit '0' total duration in T0 cycles (spec: 18..22, PM3: 20) */
#define HITAG_S_T_1_CYCLES              28 /* Bit '1' total duration in T0 cycles (spec: 26..32, PM3: 28) */
#define HITAG_S_T_STOP_CYCLES           36 /* Stop/EOF duration in T0 cycles (spec: >=36, PM3: 36) */
#define HITAG_S_T_CODE_VIOLATION_CYCLES 36 /* Hitag µ SOF code-violation carrier tail */
#define HITAG_S_T_WAIT_POWERUP_US       3000 /* Power-up + START_AUTH window margin */
#define HITAG_S_T_WAIT_SC_US            1600 /* Standard command wait (200 × T0, spec: 90..5000) */
#define HITAG_S_T_WAIT_FIRST_US         2400 /* Hitag S/µ first command wait (300 × T0, PM3 T_wfc) */
#define HITAG_S_T_WAIT_INTER_US         400 /* After RX idle, top up to about T_WAIT_SC before next TX */
#define HITAG_S_T_WAIT_RESP_US          200 /* Wait for tag response */
#define HITAG_S_T_RX_IDLE_US            1200 /* Stop receive after response edges go idle */
#define HITAG_S_T_PROG_US               6000 /* Program time after write (750 × T0, spec: 716..726) */

/* --- MC4K Manchester decoding (post-SELECT data exchange) ---
 * Half-bit = 128µs = 16 × T0, Full-bit = 256µs = 32 × T0 */
#define HITAG_S_MC4K_THRESHOLD_US 192 /* Midpoint: SHORT ~128µs / LONG ~256µs */
#define HITAG_S_MC4K_GLITCH_US    50 /* Min valid pulse duration */

/* --- AC2K Anti-collision decoding (UID response) ---
 * Flipper TIM2 rising-to-rising periods are observed around 256/384/512µs. */
#define HITAG_S_AC2K_THRESH_23_US 320 /* Between 2-half (256µs) and 3-half (384µs) */
#define HITAG_S_AC2K_THRESH_34_US 448 /* Between 3-half (384µs) and 4-half (512µs) */
#define HITAG_S_AC2K_GLITCH_US    80 /* Min valid period */

/* --- Tag memory pages --- */
#define HITAG_S_PAGE_UID    0
#define HITAG_S_PAGE_CONFIG 1
#define HITAG_S_PAGE_PWD    2
#define HITAG_S_PAGE_KEY    3
#define HITAG_S_PAGE_DATA   4

/* --- ACK value --- */
#define HITAG_S_ACK 0x01 /* 2-bit ACK = 01 */

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

typedef enum {
    HitagSModeStd,
    HitagSModeAdv1,
    HitagSModeAdv2,
    HitagSModeFadv,
} HitagSMode;

typedef enum {
    HitagSRxAC2K = 0,
    HitagSRxMC4K = 1,
    HitagSRxMC2K = 2,
    HitagSRxAC4K = 3,
    HitagSRxMC8K = 4,
} HitagSRxMode;

typedef struct {
    HitagSMode mode;
    uint32_t uid;
    uint32_t config;
    bool selected;
} HitagSSessionInfo;

typedef struct {
    size_t attempts;
    size_t last_rx_bits;
    size_t last_edge_count;
    size_t max_edge_count;
    uint8_t last_rx[4];
    bool overflow;
    bool low_entropy_reject;
    bool noisy_reject;
} HitagSUidRequestReport;

typedef enum {
    HitagSPageStatusMissing,
    HitagSPageStatusRead,
    HitagSPageStatusSkippedProtected,
    HitagSPageStatusReadError,
} HitagSPageStatus;

typedef struct {
    bool detected;
    bool had_activity;
    bool crc_ok;
    bool ttf_broadcast;
    uint8_t uid[HITAG_HTU_UID_SIZE];
    uint8_t best_prefix[3];
    size_t response_bits;
    size_t candidates_tried;
    uint16_t best_residue;
    const char* method;
} HitagHtuProbeInfo;

typedef struct {
    HitagSSessionInfo session;
    HitagHtuProbeInfo htu_probe;
    const char* failure_stage;
    HitagSPageStatus page_status[HITAG_S_MAX_PAGES];
} HitagSDebugReadReport;

typedef struct {
    bool had_activity;
    uint32_t first_edge_us;
    uint32_t elapsed_us;
    size_t edge_count;
    bool overflow;
} HitagSPassiveTtfReport;

typedef void (*HitagSRxStartCallback)(void* context);

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
 * @brief Send a BPLM frame and open the RX capture window during the stop tail
 *
 * This removes the blind interval between reader EOF and tag response by
 * starting capture immediately after the stop low pulse returns to carrier.
 */
void hitag_s_send_frame_with_early_rx(
    const uint8_t* data,
    size_t bits,
    HitagSRxStartCallback start_rx,
    void* context);

void hitag_s_send_htu_frame_with_early_rx(
    const uint8_t* data,
    size_t bits,
    HitagSRxStartCallback start_rx,
    void* context);

/**
 * @brief Emit a raw carrier pause for black-box early-window diagnostics
 */
void hitag_s_send_pause_us(uint32_t pause_us);

/**
 * @brief Start 125 kHz carrier for Hitag S communication
 */
void hitag_s_field_on(void);

/**
 * @brief Start 125 kHz carrier without waiting for the Hitag S power-up margin
 *
 * Hitag µ / PCF7931 style probes must send the first command in the early
 * T_wfc window after field reset. The generic Hitag S wait can be too late
 * and let a TTF/EM4100 broadcast start first.
 */
void hitag_s_field_on_no_wait(void);

/**
 * @brief Stop 125 kHz carrier
 */
void hitag_s_field_off(void);

/**
 * @brief Force LF field off and keep it off for a bounded reset window
 *
 * This bypasses the local field-active guard and mirrors the platform LF worker
 * stop path: stop the read timer, reset RFID pins, then wait. It is used for
 * tags that must really lose field power before the early mode-switch command.
 */
void hitag_s_field_reset_hard(uint32_t off_ms);

/**
 * @brief Build and send UID request (UID_REQ_ADV1, 5 bits)
 * @param uid  Pointer to store 32-bit UID on success
 * @return HitagSResult
 */
HitagSResult hitag_s_uid_request(uint32_t* uid);

/**
 * @brief Build and send only the ADV1 UID request used by 82xx/F8268 mode switch
 * @param uid  Pointer to store 32-bit UID on success
 * @return HitagSResult
 */
HitagSResult hitag_s_uid_request_adv1(uint32_t* uid);

/**
 * @brief Send one ADV1 UID request for the 82xx/F8268 power-up mode-switch window
 *
 * A successful UID is accepted after one clean frame because the following SELECT
 * verifies it. This avoids wasting the one-shot wake window on repeat UID probes.
 *
 * @param uid     Pointer to store 32-bit UID on success
 * @param report  Optional RX summary for diagnostics
 * @return HitagSResult
 */
HitagSResult hitag_s_uid_request_adv1_once(uint32_t* uid, HitagSUidRequestReport* report);

/**
 * @brief Send one UID request in a selected protocol mode
 *
 * Used by diagnostics to test which UID request is accepted as the first
 * command inside an early mode-switch window.
 *
 * @param mode    Protocol mode to request
 * @param uid     Pointer to store 32-bit UID on success
 * @param report  Optional RX summary for diagnostics
 * @return HitagSResult
 */
HitagSResult
    hitag_s_uid_request_once(HitagSMode mode, uint32_t* uid, HitagSUidRequestReport* report);

HitagSResult hitag_s_uid_request_once_timed(
    HitagSMode mode,
    uint32_t* uid,
    HitagSUidRequestReport* report,
    uint32_t rx_timeout_us);

/**
 * @brief Build and send SELECT command
 * @param uid     32-bit UID to select
 * @param config  Pointer to store 32-bit config page (page 1) on success
 * @return HitagSResult
 */
HitagSResult hitag_s_select(uint32_t uid, uint32_t* config);

const char* hitag_s_mode_name(HitagSMode mode);

HitagSResult hitag_s_open_session(HitagSSessionInfo* session);

HitagSResult hitag_htu_probe_uid(HitagHtuProbeInfo* info);

HitagSResult hitag_htu_probe_uid_sequence(HitagHtuProbeInfo* info);

/**
 * @brief Capture passive TTF activity for a bounded listen window
 */
HitagSResult hitag_s_capture_passive_ttf(uint32_t listen_us, HitagSPassiveTtfReport* report);

/**
 * @brief Black-box reset sweep that records TTF first-edge timing
 */
HitagSResult hitag_s_8268_ttf_timing_diagnostic(void);

/**
 * @brief Black-box early pause matrix that records whether TTF is disturbed
 */
HitagSResult hitag_s_8268_disturb_diagnostic(void);

/**
 * @brief Black-box late pause matrix near the observed TTF first-frame window
 */
HitagSResult hitag_s_8268_late_disturb_diagnostic(void);

/**
 * @brief UID command matrix near the observed TTF first-frame window
 */
HitagSResult hitag_s_8268_late_command_diagnostic(void);

/**
 * @brief T5577 direct-access/read-block diagnostic with raw response trace
 */
HitagSResult hitag_s_t5577_detect_diagnostic(void);

/**
 * @brief EM4305/EM4x05 command-response diagnostic with raw response trace
 */
HitagSResult hitag_s_em4x05_detect_diagnostic(void);

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

/**
 * @brief Wipe 8268 tag to factory-like state
 *
 * Clears all data pages (4+) to 0x00000000, resets config to defaults:
 *   CON0: MEMT=11 (64 pages), RES0=0, RES3=0 (no TTF)
 *   CON1: auth=0 (plain mode), LKP=0, LCON=0, all unlocked
 *   CON2: 0x00 (no page locks)
 *   PWDH0: kept as 0x00 (will be written via password auth)
 * Password page 2 reset to default 0xBBDD3399, page 3 to 0x00000000.
 *
 * @param password  Auth password (0 = try default passwords)
 * @param max_page  Max page to clear (0 = auto-detect from config MEMT)
 * @param pages_wiped  Output: number of pages successfully wiped (can be NULL)
 * @return HitagSResult
 */
HitagSResult hitag_s_8268_wipe_sequence(uint32_t password, int max_page, int* pages_wiped);

/**
 * @brief Save tag dump to file in Flipper Format
 *
 * File format (.hts):
 *   Filetype: HiTag S 8268 Dump
 *   Version: 1
 *   UID: AA BB CC DD
 *   Max Page: 63
 *   Page 0: AA BB CC DD
 *   Page 1: 06 24 00 40
 *   ...
 *
 * @param storage     Storage service pointer
 * @param path        File path to save to
 * @param uid         Tag UID
 * @param pages       Array of page data
 * @param page_valid  Array of validity flags
 * @param max_page    Maximum page number
 * @return true on success
 */
bool hitag_s_dump_save(
    void* storage,
    const char* path,
    uint32_t uid,
    const uint32_t* pages,
    const bool* page_valid,
    int max_page);

/**
 * @brief Load tag dump from file
 */
bool hitag_s_dump_load(
    void* storage,
    const char* path,
    uint32_t* uid,
    uint32_t* pages,
    bool* page_valid,
    int* max_page);

/** Perform a full debug read: UID + SELECT + Auth + Read all pages, with tracing */
HitagSResult hitag_s_debug_read_sequence(
    uint32_t* uid_out,
    uint32_t* config_out,
    uint32_t* pages,
    bool* page_valid,
    int* max_page);

HitagSResult hitag_s_debug_read_sequence_ex(
    uint32_t* uid_out,
    uint32_t* config_out,
    uint32_t* pages,
    bool* page_valid,
    int* max_page,
    HitagSDebugReadReport* report);

#ifdef __cplusplus
}
#endif
