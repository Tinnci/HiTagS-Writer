/**
 * @file hitag_s_8268.c
 * @brief 8268-specific HiTag S operations built on session commands.
 */

#include "hitag_s_proto.h"
#include "hitag_s_trace.h"
#include "em4100_encode.h"
#include <furi.h>
#include <string.h>

#define TAG                                     "HitagS8268"
#define HITAG_S_DEBUG_READ_BUDGET_MS            30000
#define HITAG_S_DEBUG_READ_MAX_SESSION_ATTEMPTS 80

static void trace_append(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    hitag_s_trace_vappend(fmt, args);
    va_end(args);
}

static bool hitag_s_debug_read_budget_expired(uint32_t start_tick) {
    return (uint32_t)(furi_get_tick() - start_tick) >= HITAG_S_DEBUG_READ_BUDGET_MS;
}

HitagSResult hitag_s_read_uid_sequence(uint32_t* uid) {
    hitag_s_field_on();
    HitagSResult result = hitag_s_uid_request(uid);
    hitag_s_field_off();
    return result;
}

HitagSResult hitag_s_write_page_verify(uint8_t page, uint32_t data) {
    HitagSResult result = hitag_s_write_page(page, data);
    if(result != HitagSResultOk) return result;

    uint32_t readback = 0;
    result = hitag_s_read_page(page, &readback);
    if(result != HitagSResultOk) {
        FURI_LOG_W(TAG, "VERIFY page %d: readback failed", page);
        return result;
    }

    uint32_t mask = page == 1 ? 0xFFFFFF00UL : 0xFFFFFFFFUL;
    if((readback & mask) != (data & mask)) {
        FURI_LOG_E(
            TAG,
            "VERIFY page %d: MISMATCH wrote=%08lX read=%08lX",
            page,
            (unsigned long)data,
            (unsigned long)readback);
        return HitagSResultError;
    }

    FURI_LOG_I(TAG, "VERIFY page %d: OK (%08lX)", page, (unsigned long)readback);
    return HitagSResultOk;
}

HitagSResult hitag_s_8268_authenticate_multi(const uint32_t* passwords, size_t count) {
    static const uint32_t default_passwords[] = {
        HITAG_S_8268_PASSWORD,
        HITAG_S_8268_PASSWORD_ALT1,
        HITAG_S_8268_PASSWORD_ALT2,
        HITAG_S_8268_PASSWORD_ALT3,
        HITAG_S_8268_PASSWORD_ALT4,
    };

    const uint32_t* pwd_list = passwords;
    size_t pwd_count = count;
    if(pwd_list == NULL || pwd_count == 0) {
        pwd_list = default_passwords;
        pwd_count = COUNT_OF(default_passwords);
    }

    for(size_t i = 0; i < pwd_count; i++) {
        FURI_LOG_I(
            TAG,
            "Auth attempt %d/%d with password %08lX",
            (int)(i + 1),
            (int)pwd_count,
            (unsigned long)pwd_list[i]);

        HitagSResult result = hitag_s_8268_authenticate(pwd_list[i]);
        if(result == HitagSResultOk) return HitagSResultOk;
        if(result == HitagSResultTimeout) return HitagSResultTimeout;
    }

    return HitagSResultNack;
}

bool hitag_s_page_writable(uint32_t config_val, uint8_t page) {
    HitagSConfig cfg = hitag_s_parse_config(config_val);
    if(page == 0) return true;
    if(page == 1) return !cfg.LCON;
    if(page == 2 || page == 3) return !cfg.LKP;
    return !hitag_s_page_locked(&cfg, page);
}

HitagSResult hitag_s_write_uid(uint32_t new_uid) {
    FURI_LOG_I(TAG, "Writing UID page 0: %08lX", (unsigned long)new_uid);
    HitagSResult result = hitag_s_write_page(0, new_uid);
    if(result != HitagSResultOk) {
        FURI_LOG_E(TAG, "Write UID failed");
        return result;
    }
    FURI_LOG_I(TAG, "UID write command sent successfully");
    return HitagSResultOk;
}

static HitagSResult
    hitag_s_8268_select_and_auth(uint32_t password, uint32_t* uid, uint32_t* config) {
    HitagSResult result = hitag_s_uid_request(uid);
    if(result != HitagSResultOk) return result;

    result = hitag_s_select(*uid, config);
    if(result != HitagSResultOk) return result;

    return password != 0 ? hitag_s_8268_authenticate(password) :
                           hitag_s_8268_authenticate_multi(NULL, 0);
}

HitagSResult hitag_s_8268_write_sequence(
    uint32_t password,
    const uint32_t* pages,
    const uint8_t* page_addrs,
    size_t page_count) {
    uint32_t uid = 0;
    uint32_t config = 0;
    hitag_s_field_on();

    HitagSResult result = hitag_s_8268_select_and_auth(password, &uid, &config);
    if(result != HitagSResultOk) {
        hitag_s_field_off();
        return result;
    }

    for(size_t i = 0; i < page_count; i++) {
        result = hitag_s_write_page_verify(page_addrs[i], pages[i]);
        if(result != HitagSResultOk) {
            hitag_s_field_off();
            return result;
        }
    }

    hitag_s_field_off();
    return HitagSResultOk;
}

HitagSResult hitag_s_8268_write_em4100_sequence(
    uint32_t password,
    const Em4100HitagData* em_data,
    uint32_t* config_out) {
    uint32_t uid = 0;
    uint32_t config = 0;
    hitag_s_field_on();

    HitagSResult result = hitag_s_8268_select_and_auth(password, &uid, &config);
    if(result != HitagSResultOk) {
        hitag_s_field_off();
        return result;
    }

    uint32_t current_config = 0;
    result = hitag_s_read_page(1, &current_config);
    if(result != HitagSResultOk) {
        FURI_LOG_W(TAG, "EM4100 write: Can't read config, using SELECT value");
        current_config = config;
    }

    HitagSConfig cfg = hitag_s_parse_config(current_config);
    if(hitag_s_page_locked(&cfg, 4) || hitag_s_page_locked(&cfg, 5)) {
        hitag_s_field_off();
        return HitagSResultError;
    }

    uint32_t new_config = em4100_config_make_8268_ttf(current_config);
    if(config_out) *config_out = new_config;

    result = hitag_s_write_page_verify(4, em_data->data_hi);
    if(result == HitagSResultOk) result = hitag_s_write_page_verify(5, em_data->data_lo);
    if(result == HitagSResultOk) result = hitag_s_write_page_verify(1, new_config);

    hitag_s_field_off();
    return result;
}

HitagSResult hitag_s_8268_read_sequence(
    uint32_t password,
    uint32_t* pages,
    const uint8_t* page_addrs,
    size_t page_count,
    uint32_t* uid_out) {
    uint32_t uid = 0;
    uint32_t config = 0;
    hitag_s_field_on();

    HitagSResult result = hitag_s_8268_select_and_auth(password, &uid, &config);
    if(result != HitagSResultOk) {
        hitag_s_field_off();
        return result;
    }
    if(uid_out) *uid_out = uid;

    for(size_t i = 0; i < page_count; i++) {
        result = hitag_s_read_page(page_addrs[i], &pages[i]);
        if(result != HitagSResultOk) {
            hitag_s_field_off();
            return result;
        }
    }

    hitag_s_field_off();
    return HitagSResultOk;
}

HitagSResult hitag_s_8268_read_all(
    uint32_t password,
    uint32_t* pages,
    bool* page_valid,
    int* max_page_out,
    uint32_t* uid_out) {
    uint32_t uid = 0;
    uint32_t config = 0;
    hitag_s_field_on();

    HitagSResult result = hitag_s_8268_select_and_auth(password, &uid, &config);
    if(result != HitagSResultOk) {
        hitag_s_field_off();
        return result;
    }

    if(uid_out) *uid_out = uid;
    pages[0] = uid;
    page_valid[0] = true;

    HitagSConfig cfg = hitag_s_parse_config(config);
    int max_pg = hitag_s_max_page(&cfg);
    if(max_page_out) *max_page_out = max_pg;

    result = hitag_s_read_page(1, &pages[1]);
    if(result == HitagSResultOk) {
        page_valid[1] = true;
        config = pages[1];
        cfg = hitag_s_parse_config(config);
    } else {
        pages[1] = config;
        page_valid[1] = false;
    }

    for(uint8_t p = 2; p <= 3; p++) {
        if(cfg.auth && cfg.LKP) {
            page_valid[p] = false;
            continue;
        }
        result = hitag_s_read_page(p, &pages[p]);
        page_valid[p] = result == HitagSResultOk;
    }

    for(int p = 4; p <= max_pg; p++) {
        result = hitag_s_read_page((uint8_t)p, &pages[p]);
        page_valid[p] = result == HitagSResultOk;
    }
    for(int p = max_pg + 1; p < HITAG_S_MAX_PAGES; p++) {
        page_valid[p] = false;
    }

    hitag_s_field_off();
    return HitagSResultOk;
}

HitagSResult hitag_s_8268_clone_sequence(
    uint32_t password,
    uint32_t new_uid,
    uint32_t config,
    const uint32_t* data_pages,
    const uint8_t* data_addrs,
    size_t data_count) {
    uint32_t uid = 0;
    uint32_t current_config = 0;
    hitag_s_field_on();

    HitagSResult result = hitag_s_8268_select_and_auth(password, &uid, &current_config);
    if(result != HitagSResultOk) {
        hitag_s_field_off();
        return result;
    }

    for(size_t i = 0; i < data_count; i++) {
        if(!hitag_s_page_writable(current_config, data_addrs[i])) continue;
        result = hitag_s_write_page(data_addrs[i], data_pages[i]);
        if(result != HitagSResultOk) {
            hitag_s_field_off();
            return result;
        }
    }

    result = hitag_s_write_page(1, config);
    if(result == HitagSResultOk) result = hitag_s_write_uid(new_uid);

    hitag_s_field_off();
    return result;
}

HitagSResult hitag_s_8268_wipe_sequence(uint32_t password, int max_page, int* pages_wiped) {
    uint32_t uid = 0;
    uint32_t current_config = 0;
    int wiped = 0;
    hitag_s_field_on();

    HitagSResult result = hitag_s_8268_select_and_auth(password, &uid, &current_config);
    if(result != HitagSResultOk) {
        hitag_s_field_off();
        return result;
    }

    if(max_page <= 0) {
        HitagSConfig cfg = hitag_s_parse_config(current_config);
        max_page = hitag_s_max_page(&cfg);
    }

    for(int p = 4; p <= max_page; p++) {
        if(hitag_s_write_page((uint8_t)p, 0x00000000UL) == HitagSResultOk) wiped++;
    }
    if(hitag_s_write_page(2, HITAG_S_8268_PASSWORD) == HitagSResultOk) wiped++;
    if(hitag_s_write_page(3, 0x00000000UL) == HitagSResultOk) wiped++;

    HitagSConfig clean_cfg;
    memset(&clean_cfg, 0, sizeof(clean_cfg));
    clean_cfg.MEMT = 3;
    clean_cfg.pwdh0 = (HITAG_S_8268_PASSWORD >> 24) & 0xFF;
    if(hitag_s_write_page(1, hitag_s_pack_config(&clean_cfg)) == HitagSResultOk) wiped++;

    if(pages_wiped) *pages_wiped = wiped;
    hitag_s_field_off();
    return HitagSResultOk;
}

static HitagSResult hitag_s_debug_read_finish(
    HitagSResult result,
    uint32_t uid,
    const uint32_t* pages,
    const bool* page_valid,
    int max_page,
    const HitagSDebugReadReport* report) {
    int safe_max_page = max_page;
    if(safe_max_page < 0) safe_max_page = 0;
    if(safe_max_page >= HITAG_S_MAX_PAGES) safe_max_page = HITAG_S_MAX_PAGES - 1;

    int read_count = 0;
    for(int p = 0; p <= safe_max_page; p++) {
        if(page_valid[p]) read_count++;
    }

    if(report) {
        trace_append(
            "\n=== SUMMARY: %d/%d pages read, UID=%08lX, result=%d, mode=%s, stage=%s ===\n",
            read_count,
            safe_max_page + 1,
            (unsigned long)uid,
            (int)result,
            hitag_s_mode_name(report->session.mode),
            report->failure_stage ? report->failure_stage : "-");
    } else {
        trace_append(
            "\n=== SUMMARY: %d/%d pages read, UID=%08lX, result=%d ===\n",
            read_count,
            safe_max_page + 1,
            (unsigned long)uid,
            (int)result);
    }
    trace_append("\nPAGE TABLE:\n");
    for(int p = 0; p <= safe_max_page; p++) {
        const char* status = "";
        if(report) {
            switch(report->page_status[p]) {
            case HitagSPageStatusRead:
                status = " read";
                break;
            case HitagSPageStatusSkippedProtected:
                status = " skipped-protected";
                break;
            case HitagSPageStatusReadError:
                status = " read-error";
                break;
            default:
                status = " missing";
                break;
            }
        }
        if(page_valid[p]) {
            trace_append("  [%2d] %08lX%s\n", p, (unsigned long)pages[p], status);
        } else {
            trace_append("  [%2d] --------%s\n", p, status);
        }
    }

    hitag_s_field_off();
    trace_append("Field OFF\n");
    return result;
}

HitagSResult hitag_s_debug_read_sequence(
    uint32_t* uid_out,
    uint32_t* config_out,
    uint32_t* pages,
    bool* page_valid,
    int* max_page) {
    return hitag_s_debug_read_sequence_ex(uid_out, config_out, pages, page_valid, max_page, NULL);
}

HitagSResult hitag_s_debug_read_sequence_ex(
    uint32_t* uid_out,
    uint32_t* config_out,
    uint32_t* pages,
    bool* page_valid,
    int* max_page,
    HitagSDebugReadReport* report) {
    uint32_t uid = 0;
    uint32_t config = 0;
    HitagSDebugReadReport local_report;
    if(!report) report = &local_report;
    memset(report, 0, sizeof(*report));
    report->failure_stage = "-";
    for(size_t i = 0; i < HITAG_S_MAX_PAGES; i++) {
        report->page_status[i] = HitagSPageStatusMissing;
        page_valid[i] = false;
    }

    trace_append("\n=== DEBUG READ SEQUENCE v2 ===\n");
    hitag_s_field_on();
    trace_append(
        "Field ON: carrier=125000Hz duty=0.5 pull=release powerup_us=%d\n",
        HITAG_S_T_WAIT_POWERUP_US);

    HitagSSessionInfo session = {0};
    HitagSResult result = HitagSResultTimeout;
    uint32_t start_tick = furi_get_tick();
    size_t session_attempt = 0;
    while(!hitag_s_debug_read_budget_expired(start_tick) &&
          session_attempt < HITAG_S_DEBUG_READ_MAX_SESSION_ATTEMPTS) {
        session_attempt++;
        trace_append(
            "DEBUG_READ: session attempt %d budget_ms=%d elapsed_ticks=%lu\n",
            (int)session_attempt,
            HITAG_S_DEBUG_READ_BUDGET_MS,
            (unsigned long)(uint32_t)(furi_get_tick() - start_tick));
        result = hitag_s_open_session(&session);
        if(result == HitagSResultOk) break;
        if(session_attempt == 1 || (session_attempt % 5) == 0) {
            FURI_LOG_W(
                TAG,
                "Debug read still probing: attempt=%d elapsed=%lums free_heap=%d result=%d",
                (int)session_attempt,
                (unsigned long)(uint32_t)(furi_get_tick() - start_tick),
                (int)memmgr_get_free_heap(),
                (int)result);
        }
        trace_append(
            "DEBUG_READ: session attempt %d failed (result=%d), retrying within budget\n",
            (int)session_attempt,
            (int)result);
    }

    if(result != HitagSResultOk) {
        report->failure_stage = "UID";
        trace_append("ABORT: UID/SELECT session failed (result=%d)\n", (int)result);
        return hitag_s_debug_read_finish(result, uid, pages, page_valid, 0, report);
    }

    report->session = session;
    uid = session.uid;
    config = session.config;
    if(uid_out) *uid_out = uid;
    pages[0] = uid;
    page_valid[0] = true;
    report->page_status[0] = HitagSPageStatusRead;

    if(config_out) *config_out = config;

    HitagSConfig cfg = hitag_s_parse_config(config);
    int max_pg = hitag_s_max_page(&cfg);
    if(max_page) *max_page = max_pg;

    trace_append(
        "Config: MEMT=%d max_page=%d auth=%d LKP=%d LCON=%d\n",
        cfg.MEMT,
        max_pg,
        cfg.auth,
        cfg.LKP,
        cfg.LCON);
    trace_append("Config: TTFC=%d TTFDR=%d TTFM=%d\n", cfg.TTFC, cfg.TTFDR, cfg.TTFM);

    trace_append("READ_PAGE 1: trying real config read before using SELECT candidate\n");
    result = hitag_s_read_page(1, &pages[1]);
    if(result == HitagSResultOk) {
        page_valid[1] = true;
        report->page_status[1] = HitagSPageStatusRead;
        config = pages[1];
        cfg = hitag_s_parse_config(config);
        max_pg = hitag_s_max_page(&cfg);
        if(max_page) *max_page = max_pg;
        if(config_out) *config_out = config;
    } else {
        pages[1] = config;
        page_valid[1] = false;
        trace_append("  (using config from SELECT)\n");
        report->page_status[1] = HitagSPageStatusReadError;
    }

    bool need_auth = cfg.auth || !page_valid[1];
    bool authed = false;
    if(need_auth) {
        trace_append("\n--- AUTH MULTI ---\n");
        result = hitag_s_8268_authenticate_multi(NULL, 0);
        if(result != HitagSResultOk) {
            report->failure_stage = "AUTH";
            trace_append("ABORT: AUTH failed (result=%d)\n", (int)result);
            return hitag_s_debug_read_finish(result, uid, pages, page_valid, max_pg, report);
        }
        authed = true;

        if(!page_valid[1]) {
            result = hitag_s_read_page(1, &pages[1]);
            if(result == HitagSResultOk) {
                page_valid[1] = true;
                report->page_status[1] = HitagSPageStatusRead;
                config = pages[1];
                cfg = hitag_s_parse_config(config);
                max_pg = hitag_s_max_page(&cfg);
                if(max_page) *max_page = max_pg;
                if(config_out) *config_out = config;
            }
        }
    }

    for(uint8_t p = 2; p <= 3; p++) {
        if(authed && cfg.LKP) {
            page_valid[p] = false;
            report->page_status[p] = HitagSPageStatusSkippedProtected;
            trace_append("  page %d skipped: auth+LKP protected\n", p);
            continue;
        }
        result = hitag_s_read_page(p, &pages[p]);
        page_valid[p] = result == HitagSResultOk;
        report->page_status[p] = page_valid[p] ? HitagSPageStatusRead : HitagSPageStatusReadError;
    }

    for(int p = 4; p <= max_pg; p++) {
        result = hitag_s_read_page((uint8_t)p, &pages[p]);
        page_valid[p] = result == HitagSResultOk;
        report->page_status[p] = page_valid[p] ? HitagSPageStatusRead : HitagSPageStatusReadError;
    }
    for(int p = max_pg + 1; p < HITAG_S_MAX_PAGES; p++) {
        page_valid[p] = false;
        report->page_status[p] = HitagSPageStatusMissing;
    }

    report->failure_stage = "READ";
    return hitag_s_debug_read_finish(HitagSResultOk, uid, pages, page_valid, max_pg, report);
}
