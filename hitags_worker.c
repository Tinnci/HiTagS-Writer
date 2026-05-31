/**
 * @file hitags_worker.c
 * @brief Worker thread and operation dispatch for HiTagS Writer.
 */

#include "hitags_writer_i.h"

#define TAG                         "HitagSWorker"
#define WRITE_STIM_RESTORE_ATTEMPTS 5

static bool hitags_worker_should_stop(void) {
    return furi_thread_flags_get() & HITAGS_WORKER_FLAG_STOP;
}

static bool hitags_worker_wait_or_stop(uint32_t timeout_ms) {
    uint32_t wait = furi_thread_flags_wait(HITAGS_WORKER_FLAG_STOP, FuriFlagWaitAny, timeout_ms);
    return wait != (uint32_t)FuriFlagErrorTimeout;
}

static void hitags_worker_count_dump_pages(HitagSApp* app) {
    app->dump_read_count = 0;
    for(int p = 0; p <= app->dump_max_page; p++) {
        if(app->dump_valid[p]) app->dump_read_count++;
    }
}

static void hitags_worker_count_any_dump_pages(HitagSApp* app) {
    app->dump_read_count = 0;
    for(int p = 0; p < HITAG_S_MAX_PAGES; p++) {
        if(app->dump_valid[p]) app->dump_read_count++;
    }
}

static void hitags_worker_htu_probe(HitagSApp* app) {
    FURI_LOG_I(TAG, "HTU Probe: probing Hitag µ/8265 READ UID...");
    memset(&app->htu_probe, 0, sizeof(app->htu_probe));
    app->last_result = hitag_htu_probe_uid_sequence(&app->htu_probe);

    if(app->last_result == HitagSResultOk && app->htu_probe.detected) {
        FURI_LOG_W(
            TAG,
            "HTU Probe OK UID=%02X%02X%02X%02X%02X%02X method=%s bits=%d candidates=%d",
            app->htu_probe.uid[0],
            app->htu_probe.uid[1],
            app->htu_probe.uid[2],
            app->htu_probe.uid[3],
            app->htu_probe.uid[4],
            app->htu_probe.uid[5],
            app->htu_probe.method ? app->htu_probe.method : "?",
            (int)app->htu_probe.response_bits,
            (int)app->htu_probe.candidates_tried);
        view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventHtuProbeOk);
    } else {
        FURI_LOG_W(
            TAG,
            "HTU Probe failed result=%d best=%s bits=%d candidates=%d crc16=%04X first=%02X %02X %02X%s",
            (int)app->last_result,
            app->htu_probe.method ? app->htu_probe.method : "none",
            (int)app->htu_probe.response_bits,
            (int)app->htu_probe.candidates_tried,
            app->htu_probe.best_residue,
            app->htu_probe.best_prefix[0],
            app->htu_probe.best_prefix[1],
            app->htu_probe.best_prefix[2],
            app->htu_probe.ttf_broadcast ? " ttf_broadcast" : "");
        view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventHtuProbeFailed);
    }
}

static void hitags_worker_write_em4100(HitagSApp* app) {
    Em4100HitagData hitag_data;
    em4100_prepare_hitag_data(app->em4100_id, &hitag_data);

    FURI_LOG_I(
        TAG,
        "Writing EM4100 ID %02X%02X%02X%02X%02X",
        app->em4100_id[0],
        app->em4100_id[1],
        app->em4100_id[2],
        app->em4100_id[3],
        app->em4100_id[4]);

    const int max_attempts = 15;
    for(int attempts = 1; !hitags_worker_should_stop(); attempts++) {
        uint32_t config_page = 0;
        app->last_result =
            hitag_s_8268_write_em4100_sequence(app->password, &hitag_data, &config_page);

        if(app->last_result == HitagSResultOk) {
            FURI_LOG_I(
                TAG, "Write OK (attempt %d, config=%08lX)", attempts, (unsigned long)config_page);
            view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventWriteOk);
            return;
        }

        if(attempts >= max_attempts) {
            FURI_LOG_W(TAG, "Write failed after %d attempts", attempts);
            view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventWriteFailed);
            return;
        }

        if(hitags_worker_wait_or_stop(200)) return;
    }
}

static void hitags_worker_read_uid(HitagSApp* app) {
    FURI_LOG_I(TAG, "Scanning for UID...");

    const int max_attempts = 15;
    for(int attempts = 1; !hitags_worker_should_stop(); attempts++) {
        app->last_result = hitag_s_read_uid_sequence(&app->tag_uid);

        if(app->last_result == HitagSResultOk) {
            FURI_LOG_I(TAG, "UID=%08lX (attempt %d)", (unsigned long)app->tag_uid, attempts);
            view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventReadOk);
            return;
        }

        if(attempts >= max_attempts) {
            FURI_LOG_W(TAG, "UID read failed after %d attempts", attempts);
            view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventReadFailed);
            return;
        }

        if(hitags_worker_wait_or_stop(100)) return;
    }
}

static void hitags_worker_read_pages(HitagSApp* app) {
    FURI_LOG_I(TAG, "Scanning to read tag data...");

    uint8_t page_addrs[3] = {1, 4, 5};
    const int max_attempts = 15;

    for(int attempts = 1; !hitags_worker_should_stop(); attempts++) {
        app->last_result = hitag_s_8268_read_sequence(
            app->password, app->read_pages, page_addrs, 3, &app->tag_uid);

        if(app->last_result == HitagSResultOk) {
            if(!em4100_decode_hitag_data(app->read_pages[1], app->read_pages[2], app->read_id)) {
                FURI_LOG_W(TAG, "Pages 4/5 are not valid EM4100 data (attempt %d)", attempts);
                app->last_result = HitagSResultError;
            } else {
                FURI_LOG_I(
                    TAG,
                    "Read EM4100 %02X:%02X:%02X:%02X:%02X (attempt %d)",
                    app->read_id[0],
                    app->read_id[1],
                    app->read_id[2],
                    app->read_id[3],
                    app->read_id[4],
                    attempts);
                view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventReadOk);
                return;
            }
        }

        if(attempts >= max_attempts) {
            FURI_LOG_W(TAG, "Read failed after %d attempts", attempts);
            view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventReadFailed);
            return;
        }

        if(hitags_worker_wait_or_stop(100)) return;
    }
}

static void hitags_worker_write_uid(HitagSApp* app) {
    FURI_LOG_I(TAG, "Writing UID %08lX...", (unsigned long)app->target_uid);

    const int max_attempts = 15;
    for(int attempts = 1; !hitags_worker_should_stop(); attempts++) {
        uint32_t page_data[1] = {app->target_uid};
        uint8_t page_addrs[1] = {0};

        app->last_result = hitag_s_8268_write_sequence(app->password, page_data, page_addrs, 1);

        if(app->last_result == HitagSResultOk) {
            FURI_LOG_I(TAG, "Write UID OK (attempt %d)", attempts);
            view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventWriteUidOk);
            return;
        }

        if(attempts >= max_attempts) {
            FURI_LOG_W(TAG, "Write UID failed after %d attempts", attempts);
            view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventWriteUidFailed);
            return;
        }

        if(hitags_worker_wait_or_stop(200)) return;
    }
}

static void hitags_worker_full_dump(HitagSApp* app) {
    FURI_LOG_I(TAG, "Starting full tag dump...");

    memset(app->dump_pages, 0, sizeof(app->dump_pages));
    memset(app->dump_valid, 0, sizeof(app->dump_valid));
    app->dump_max_page = 0;
    app->dump_read_count = 0;

    const int max_attempts = 10;
    for(int attempts = 1; !hitags_worker_should_stop(); attempts++) {
        app->last_result = hitag_s_8268_read_all(
            app->password, app->dump_pages, app->dump_valid, &app->dump_max_page, &app->tag_uid);

        if(app->last_result == HitagSResultOk) {
            hitags_worker_count_dump_pages(app);
            FURI_LOG_I(
                TAG,
                "Dump OK — %d/%d pages (attempt %d)",
                app->dump_read_count,
                app->dump_max_page + 1,
                attempts);
            view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventDumpOk);
            return;
        }

        if(attempts >= max_attempts) {
            FURI_LOG_W(TAG, "Dump failed after %d attempts", attempts);
            view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventDumpFailed);
            return;
        }

        if(hitags_worker_wait_or_stop(200)) return;
    }
}

static void hitags_worker_clone_dump(HitagSApp* app) {
    FURI_LOG_I(
        TAG,
        "Cloning dump UID=%08lX, %d pages...",
        (unsigned long)app->clone_uid,
        (int)app->clone_count);

    const int max_attempts = 15;
    for(int attempts = 1; !hitags_worker_should_stop(); attempts++) {
        app->last_result = hitag_s_8268_clone_sequence(
            app->password,
            app->clone_uid,
            app->clone_config,
            app->clone_pages,
            app->clone_addrs,
            app->clone_count);

        if(app->last_result == HitagSResultOk) {
            FURI_LOG_I(TAG, "Clone OK (attempt %d)", attempts);
            view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventCloneOk);
            return;
        }

        if(attempts >= max_attempts) {
            FURI_LOG_W(TAG, "Clone failed after %d attempts", attempts);
            view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventCloneFailed);
            return;
        }

        if(hitags_worker_wait_or_stop(200)) return;
    }
}

static void hitags_worker_wipe_tag(HitagSApp* app) {
    FURI_LOG_I(TAG, "Wiping tag...");

    const int max_attempts = 10;
    for(int attempts = 1; !hitags_worker_should_stop(); attempts++) {
        int wiped = 0;
        app->last_result = hitag_s_8268_wipe_sequence(app->password, 0, &wiped);

        if(app->last_result == HitagSResultOk) {
            app->wipe_count = wiped;
            FURI_LOG_I(TAG, "Wipe OK, %d pages (attempt %d)", wiped, attempts);
            view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventWipeOk);
            return;
        }

        if(attempts >= max_attempts) {
            FURI_LOG_W(TAG, "Wipe failed after %d attempts", attempts);
            view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventWipeFailed);
            return;
        }

        if(hitags_worker_wait_or_stop(200)) return;
    }
}

static void hitags_worker_reset_debug_trace_state(HitagSApp* app, const char* stage) {
    memset(app->dump_pages, 0, sizeof(app->dump_pages));
    memset(app->dump_valid, 0, sizeof(app->dump_valid));
    app->dump_max_page = 0;
    app->dump_read_count = 0;
    app->tag_uid = 0;
    app->debug_mode = HitagSModeStd;
    app->debug_stage = stage;

    if(app->debug_trace) {
        furi_string_free((FuriString*)app->debug_trace);
        app->debug_trace = NULL;
    }
}

static void hitags_worker_debug_read(HitagSApp* app) {
    FURI_LOG_I(TAG, "Starting debug read with trace...");

    hitags_worker_reset_debug_trace_state(app, "-");
    app->tag_uid = 0;

    hitag_s_debug_trace_start();

    uint32_t config = 0;
    HitagSDebugReadReport report;
    app->last_result = hitag_s_debug_read_sequence_ex(
        &app->tag_uid, &config, app->dump_pages, app->dump_valid, &app->dump_max_page, &report);
    app->debug_mode = report.session.mode;
    app->debug_stage = report.failure_stage ? report.failure_stage : "-";

    app->debug_trace = hitag_s_debug_trace_stop();

    if(app->last_result == HitagSResultOk) {
        hitags_worker_count_dump_pages(app);
        FURI_LOG_I(TAG, "Debug read OK — %d pages", app->dump_read_count);
        view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventDebugOk);
    } else if(app->debug_trace && report.session.selected) {
        hitags_worker_count_any_dump_pages(app);
        FURI_LOG_W(
            TAG,
            "Debug read partial trace ready (result=%d, pages=%d)",
            (int)app->last_result,
            app->dump_read_count);
        view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventDebugPartial);
    } else if(app->debug_trace && report.htu_probe.detected) {
        hitags_worker_count_any_dump_pages(app);
        FURI_LOG_W(TAG, "Debug read detected Hitag µ/8265; trace ready");
        view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventDebugPartial);
    } else if(app->debug_trace && !report.session.selected && !report.htu_probe.detected) {
        FURI_LOG_W(TAG, "Debug read captured rejected noise; trace ready");
        app->debug_stage = "NOISE";
        hitags_worker_count_any_dump_pages(app);
        view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventDebugPartial);
    } else {
        FURI_LOG_W(TAG, "Debug read failed (result=%d)", (int)app->last_result);
        view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventDebugFailed);
    }
}

static void hitags_worker_ttf_timing(HitagSApp* app) {
    FURI_LOG_I(TAG, "Starting TTF timing diagnostic with trace...");

    hitags_worker_reset_debug_trace_state(app, "TTF_TIMING");
    hitag_s_debug_trace_start();
    app->last_result = hitag_s_8268_ttf_timing_diagnostic();
    app->debug_trace = hitag_s_debug_trace_stop();

    if(app->debug_trace) {
        FURI_LOG_W(TAG, "TTF timing diagnostic trace ready (result=%d)", (int)app->last_result);
        view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventDebugPartial);
    } else {
        FURI_LOG_W(TAG, "TTF timing diagnostic failed to capture trace");
        view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventDebugFailed);
    }
}

static void hitags_worker_disturb_test(HitagSApp* app) {
    FURI_LOG_I(TAG, "Starting disturb diagnostic with trace...");

    hitags_worker_reset_debug_trace_state(app, "DISTURB");
    hitag_s_debug_trace_start();
    app->last_result = hitag_s_8268_disturb_diagnostic();
    app->debug_trace = hitag_s_debug_trace_stop();

    if(app->debug_trace) {
        FURI_LOG_W(TAG, "Disturb diagnostic trace ready (result=%d)", (int)app->last_result);
        view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventDebugPartial);
    } else {
        FURI_LOG_W(TAG, "Disturb diagnostic failed to capture trace");
        view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventDebugFailed);
    }
}

static void hitags_worker_late_disturb_test(HitagSApp* app) {
    FURI_LOG_I(TAG, "Starting late disturb diagnostic with trace...");

    hitags_worker_reset_debug_trace_state(app, "LATE_DISTURB");
    hitag_s_debug_trace_start();
    app->last_result = hitag_s_8268_late_disturb_diagnostic();
    app->debug_trace = hitag_s_debug_trace_stop();

    if(app->debug_trace) {
        FURI_LOG_W(TAG, "Late disturb diagnostic trace ready (result=%d)", (int)app->last_result);
        view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventDebugPartial);
    } else {
        FURI_LOG_W(TAG, "Late disturb diagnostic failed to capture trace");
        view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventDebugFailed);
    }
}

static void hitags_worker_late_command_test(HitagSApp* app) {
    FURI_LOG_I(TAG, "Starting late command diagnostic with trace...");

    hitags_worker_reset_debug_trace_state(app, "LATE_COMMAND");
    hitag_s_debug_trace_start();
    app->last_result = hitag_s_8268_late_command_diagnostic();
    app->debug_trace = hitag_s_debug_trace_stop();

    if(app->debug_trace) {
        FURI_LOG_W(TAG, "Late command diagnostic trace ready (result=%d)", (int)app->last_result);
        view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventDebugPartial);
    } else {
        FURI_LOG_W(TAG, "Late command diagnostic failed to capture trace");
        view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventDebugFailed);
    }
}

static void hitags_worker_t5577_detect(HitagSApp* app) {
    FURI_LOG_I(TAG, "Starting T5577 detect with trace...");

    hitags_worker_reset_debug_trace_state(app, "T5577_DETECT");
    hitag_s_debug_trace_start();
    app->last_result = hitag_s_t5577_detect_diagnostic();
    app->debug_trace = hitag_s_debug_trace_stop();

    if(app->debug_trace) {
        FURI_LOG_W(TAG, "T5577 detect trace ready (result=%d)", (int)app->last_result);
        view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventDebugPartial);
    } else {
        FURI_LOG_W(TAG, "T5577 detect failed to capture trace");
        view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventDebugFailed);
    }
}

static void hitags_worker_em4x05_detect(HitagSApp* app) {
    FURI_LOG_I(TAG, "Starting EM4x05 detect with trace...");

    hitags_worker_reset_debug_trace_state(app, "EM4X05_DETECT");
    hitag_s_debug_trace_start();
    app->last_result = hitag_s_em4x05_detect_diagnostic();
    app->debug_trace = hitag_s_debug_trace_stop();

    if(app->debug_trace) {
        FURI_LOG_W(TAG, "EM4x05 detect trace ready (result=%d)", (int)app->last_result);
        view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventDebugPartial);
    } else {
        FURI_LOG_W(TAG, "EM4x05 detect failed to capture trace");
        view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventDebugFailed);
    }
}

typedef struct {
    FuriSemaphore* sem;
    ProtocolId protocol;
} HitagsWorkerLfReadSync;

static void hitags_worker_lf_read_sync_callback(
    LFRFIDWorkerReadResult result,
    ProtocolId protocol,
    void* context) {
    HitagsWorkerLfReadSync* sync = context;
    if(result == LFRFIDWorkerReadDone) {
        sync->protocol = protocol;
        furi_semaphore_release(sync->sem);
    }
}

static bool hitags_worker_read_em4100_once(HitagSApp* app, uint8_t id[EM4100_ID_SIZE]) {
    HitagsWorkerLfReadSync sync = {
        .sem = furi_semaphore_alloc(1, 0),
        .protocol = PROTOCOL_NO,
    };

    lfrfid_worker_start_thread(app->lfworker);
    lfrfid_worker_read_start(
        app->lfworker, LFRFIDWorkerReadTypeAuto, hitags_worker_lf_read_sync_callback, &sync);

    FuriStatus status = furi_semaphore_acquire(sync.sem, 3500);
    lfrfid_worker_stop(app->lfworker);
    lfrfid_worker_stop_thread(app->lfworker);

    bool ok = false;
    if(status == FuriStatusOk && sync.protocol == LFRFIDProtocolEM4100) {
        size_t data_size = protocol_dict_get_data_size(app->dict, sync.protocol);
        if(data_size >= EM4100_ID_SIZE) {
            uint8_t data[16] = {0};
            if(data_size > sizeof(data)) data_size = sizeof(data);
            protocol_dict_get_data(app->dict, sync.protocol, data, data_size);
            memcpy(id, data, EM4100_ID_SIZE);
            ok = true;
        }
    }

    furi_semaphore_free(sync.sem);
    return ok;
}

typedef struct {
    const char* method;
    uint8_t mask;
    bool with_mask;
    bool with_pass;
    uint32_t password;
} HitagsWorkerWriteTrial;

static void
    hitags_worker_t5577_prepare_em4100_id(const uint8_t id[EM4100_ID_SIZE], LFRFIDT5577* data) {
    uint64_t encoded = em4100_encode(id);
    memset(data, 0, sizeof(LFRFIDT5577));
    data->block[0] = LFRFID_T5577_MODULATION_MANCHESTER | LFRFID_T5577_BITRATE_RF_64 |
                     (2 << LFRFID_T5577_MAXBLOCK_SHIFT);
    data->block[1] = (uint32_t)(encoded >> 32);
    data->block[2] = (uint32_t)encoded;
    data->blocks_to_write = 3;
}

static void hitags_worker_t5577_write_em4100_id(const uint8_t id[EM4100_ID_SIZE]) {
    LFRFIDT5577 data;
    hitags_worker_t5577_prepare_em4100_id(id, &data);
    t5577_write(&data);
}

static void hitags_worker_t5577_write_em4100_trial(
    const uint8_t id[EM4100_ID_SIZE],
    const HitagsWorkerWriteTrial* trial) {
    LFRFIDT5577 data;
    hitags_worker_t5577_prepare_em4100_id(id, &data);
    data.mask = trial->mask;
    if(trial->with_mask) {
        t5577_write_with_mask(&data, 0, trial->with_pass, trial->password);
    } else {
        t5577_write(&data);
    }
}

static bool
    hitags_worker_id_equal(const uint8_t a[EM4100_ID_SIZE], const uint8_t b[EM4100_ID_SIZE]) {
    return memcmp(a, b, EM4100_ID_SIZE) == 0;
}

static void hitags_worker_trace_id(const char* prefix, const uint8_t id[EM4100_ID_SIZE]) {
    FURI_LOG_W(TAG, "%s%02X%02X%02X%02X%02X", prefix, id[0], id[1], id[2], id[3], id[4]);
    hitag_s_trace_append("%s%02X%02X%02X%02X%02X\n", prefix, id[0], id[1], id[2], id[3], id[4]);
}

static const char* hitags_worker_edge_model_classification(const HitagSPassiveTtfReport* report) {
    if(!report->had_activity || report->edge_count <= 2) return "no_activity";
    if(report->edge_count < 8) return "partial_noise";
    return "ttf_broadcast";
}

static void hitags_worker_sample_edge_model(const char* phase) {
    hitag_s_field_reset_hard(20);
    hitag_s_field_on_no_wait();

    HitagSPassiveTtfReport report = {0};
    (void)hitag_s_capture_passive_ttf(40000, &report);
    const char* classification = hitags_worker_edge_model_classification(&report);
    uint8_t ttf_score = strcmp(classification, "ttf_broadcast") == 0 ? 100 : 0;
    const char* clock_guess = strcmp(classification, "ttf_broadcast") == 0 ? "RF/64" : "unknown";

    hitag_s_trace_append(
        "EDGE_MODEL phase=%s first_edge_us=%lu edges=%d rx_bits=0 first=00000000 ttf_score=%u low_entropy=0 clock_guess=%s classification=%s\n",
        phase,
        (unsigned long)report.first_edge_us,
        (int)report.edge_count,
        ttf_score,
        clock_guess,
        classification);
    FURI_LOG_W(
        TAG,
        "EDGE_MODEL phase=%s first_edge_us=%lu edges=%d classification=%s",
        phase,
        (unsigned long)report.first_edge_us,
        (int)report.edge_count,
        classification);

    hitag_s_field_off();
}

static void hitags_worker_trace_write_read(
    const char* phase,
    bool read_ok,
    const uint8_t id[EM4100_ID_SIZE]) {
    hitag_s_trace_append(
        "WRITE_READ phase=%s ok=%d id=%02X%02X%02X%02X%02X\n",
        phase,
        read_ok,
        id[0],
        id[1],
        id[2],
        id[3],
        id[4]);
}

static bool hitags_worker_run_write_trial(
    HitagSApp* app,
    const HitagsWorkerWriteTrial* trial,
    const uint8_t original[EM4100_ID_SIZE],
    const uint8_t temp_id[EM4100_ID_SIZE]) {
    uint8_t after[EM4100_ID_SIZE] = {0};
    uint8_t after50[EM4100_ID_SIZE] = {0};
    uint8_t after150[EM4100_ID_SIZE] = {0};
    uint8_t after500[EM4100_ID_SIZE] = {0};

    hitag_s_trace_append(
        "WRITE_TRIAL method=%s block0=manchester_rf64_maxblock2 mask=%02X password=%s%08lX\n",
        trial->method,
        trial->mask,
        trial->with_pass ? "0x" : "none:",
        (unsigned long)(trial->with_pass ? trial->password : 0));
    hitags_worker_t5577_write_em4100_trial(temp_id, trial);

    furi_delay_ms(50);
    bool read50_ok = hitags_worker_read_em4100_once(app, after50);
    hitags_worker_trace_write_read("after50", read50_ok, after50);
    hitags_worker_sample_edge_model("after50");

    furi_delay_ms(100);
    bool read150_ok = hitags_worker_read_em4100_once(app, after150);
    hitags_worker_trace_write_read("after150", read150_ok, after150);
    hitags_worker_sample_edge_model("after150");

    furi_delay_ms(350);
    bool read500_ok = hitags_worker_read_em4100_once(app, after500);
    hitags_worker_trace_write_read("after500", read500_ok, after500);
    hitags_worker_sample_edge_model("after500");

    bool temp_seen = (read50_ok && hitags_worker_id_equal(after50, temp_id)) ||
                     (read150_ok && hitags_worker_id_equal(after150, temp_id)) ||
                     (read500_ok && hitags_worker_id_equal(after500, temp_id));
    bool original_stable =
        read50_ok && read150_ok && read500_ok && hitags_worker_id_equal(after50, original) &&
        hitags_worker_id_equal(after150, original) && hitags_worker_id_equal(after500, original);
    bool any_changed = temp_seen || (read50_ok && !hitags_worker_id_equal(after50, original)) ||
                       (read150_ok && !hitags_worker_id_equal(after150, original)) ||
                       (read500_ok && !hitags_worker_id_equal(after500, original)) || !read50_ok ||
                       !read150_ok || !read500_ok;

    memcpy(after, read500_ok ? after500 : (read150_ok ? after150 : after50), sizeof(after));
    const char* classification = "partial_noise";
    bool restore_needed = false;
    if(original_stable) {
        classification = "write_ignored";
        hitag_s_trace_append("WRITE_RULE after==original classification=write_ignored\n");
    } else if(temp_seen && hitags_worker_id_equal(after, temp_id)) {
        classification = "write_changed";
        hitag_s_trace_append("WRITE_RULE after==temp classification=write_changed\n");
        restore_needed = true;
    } else if(any_changed && !hitags_worker_id_equal(after, original)) {
        classification = "write_changed";
        hitag_s_trace_append("WRITE_RULE after!=original classification=write_changed\n");
        restore_needed = true;
    }

    bool restored = !restore_needed;
    if(restore_needed) {
        for(uint8_t attempt = 1; attempt <= WRITE_STIM_RESTORE_ATTEMPTS; attempt++) {
            hitags_worker_t5577_write_em4100_id(original);
            furi_delay_ms(150);
            memset(after, 0, sizeof(after));
            bool read_ok = hitags_worker_read_em4100_once(app, after);

            hitag_s_trace_append(
                "WRITE_STIM_RESTORE: attempt=%u read_ok=%d id=%02X%02X%02X%02X%02X\n",
                attempt,
                read_ok,
                after[0],
                after[1],
                after[2],
                after[3],
                after[4]);
            FURI_LOG_W(
                TAG,
                "WRITE_STIM_RESTORE: attempt=%u read_ok=%d id=%02X%02X%02X%02X%02X",
                attempt,
                read_ok,
                after[0],
                after[1],
                after[2],
                after[3],
                after[4]);

            if(read_ok && hitags_worker_id_equal(after, original)) {
                restored = true;
                break;
            }
        }
    }

    if(restore_needed && !restored) {
        app->debug_stage = "WRITE_STIM_RESTORE_FAILED";
        app->last_result = HitagSResultError;
        classification = "restore_failed";
        hitag_s_trace_append("WRITE_STIM_RESTORE_FAILED\n");
        FURI_LOG_E(TAG, "WRITE_STIM_RESTORE_FAILED");
    }

    hitag_s_trace_append(
        "WRITE_RESULT method=%s classification=%s before=%02X%02X%02X%02X%02X after=%02X%02X%02X%02X%02X restored=%d\n",
        trial->method,
        classification,
        original[0],
        original[1],
        original[2],
        original[3],
        original[4],
        after[0],
        after[1],
        after[2],
        after[3],
        after[4],
        restored);
    FURI_LOG_W(
        TAG,
        "WRITE_RESULT method=%s classification=%s restored=%d",
        trial->method,
        classification,
        restored);

    if(!restore_needed) {
        hitag_s_trace_append("WRITE_STIM: restored=0 temp_confirmed=0\n");
    } else if(restored) {
        hitag_s_trace_append("WRITE_STIM: temp_changed=%d restored=1\n", temp_seen);
    }

    return !restore_needed || restored;
}

static void hitags_worker_write_stimulus_verify(HitagSApp* app) {
    FURI_LOG_I(TAG, "Starting write matrix with trace...");

    hitags_worker_reset_debug_trace_state(app, "WRITE_MATRIX");
    hitag_s_debug_trace_start();
    hitag_s_trace_append("\n--- WRITE_MATRIX ---\n");

    uint8_t original[EM4100_ID_SIZE] = {0};
    uint8_t after[EM4100_ID_SIZE] = {0};
    uint8_t temp_id[EM4100_ID_SIZE] = {0};

    bool baseline_ok = false;
    for(uint8_t i = 0; i < 3; i++) {
        memset(after, 0, sizeof(after));
        bool read_ok = hitags_worker_read_em4100_once(app, after);
        hitag_s_trace_append(
            "BASELINE_READ index=%u ok=%d id=%02X%02X%02X%02X%02X\n",
            i + 1,
            read_ok,
            after[0],
            after[1],
            after[2],
            after[3],
            after[4]);
        if(read_ok && !baseline_ok) {
            memcpy(original, after, sizeof(original));
            baseline_ok = true;
        }
        hitags_worker_sample_edge_model(i == 0 ? "before" : "baseline");
    }

    if(!baseline_ok) {
        hitag_s_trace_append("WRITE_MATRIX_BEGIN card=target original=unread\n");
        hitag_s_trace_append(
            "WRITE_RESULT method=baseline classification=no_activity before=unread after=unread restored=0\n");
        FURI_LOG_W(TAG, "WRITE_MATRIX: before_read=failed");
        app->last_result = HitagSResultTimeout;
        app->debug_trace = hitag_s_debug_trace_stop();
        view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventDebugPartial);
        return;
    }

    memcpy(temp_id, original, sizeof(temp_id));
    temp_id[4] ^= 0x01;

    hitag_s_trace_append(
        "WRITE_MATRIX_BEGIN card=target original=%02X%02X%02X%02X%02X temp=%02X%02X%02X%02X%02X\n",
        original[0],
        original[1],
        original[2],
        original[3],
        original[4],
        temp_id[0],
        temp_id[1],
        temp_id[2],
        temp_id[3],
        temp_id[4]);
    hitag_s_trace_append("CONTROL_DELTA card=control status=run_same_matrix_on_real_T5577\n");
    hitag_s_trace_append("CLONE_DELTA card=clone status=compare_against_control_trace\n");
    hitags_worker_trace_id("WRITE_MATRIX: before=", original);
    hitags_worker_trace_id("WRITE_MATRIX: temp=", temp_id);

    HitagsWorkerWriteTrial trials[] = {
        {.method = "t5577_full", .mask = 0x07, .with_mask = false},
        {.method = "t5577_mask_data", .mask = 0x06, .with_mask = true},
        {.method = "t5577_mask_full", .mask = 0x07, .with_mask = true},
        {
            .method = "t5577_mask_full_pwd_00000000",
            .mask = 0x07,
            .with_mask = true,
            .with_pass = true,
            .password = 0x00000000UL,
        },
        {
            .method = "t5577_mask_full_pwd_FFFFFFFF",
            .mask = 0x07,
            .with_mask = true,
            .with_pass = true,
            .password = 0xFFFFFFFFUL,
        },
        {
            .method = "t5577_mask_full_pwd_51243648",
            .mask = 0x07,
            .with_mask = true,
            .with_pass = true,
            .password = 0x51243648UL,
        },
        {
            .method = "t5577_mask_full_pwd_app",
            .mask = 0x07,
            .with_mask = true,
            .with_pass = true,
            .password = app->password,
        },
    };

    app->last_result = HitagSResultOk;
    for(size_t i = 0; i < COUNT_OF(trials) && !hitags_worker_should_stop(); i++) {
        if(!hitags_worker_run_write_trial(app, &trials[i], original, temp_id)) break;
    }

    app->debug_trace = hitag_s_debug_trace_stop();
    view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventDebugPartial);
}

int32_t hitags_writer_worker_thread(void* context) {
    HitagSApp* app = context;

    switch(app->worker_op) {
    case HitagSWorkerWrite:
        hitags_worker_write_em4100(app);
        break;
    case HitagSWorkerReadUid:
        hitags_worker_read_uid(app);
        break;
    case HitagSWorkerReadPages:
        hitags_worker_read_pages(app);
        break;
    case HitagSWorkerWriteUid:
        hitags_worker_write_uid(app);
        break;
    case HitagSWorkerFullDump:
        hitags_worker_full_dump(app);
        break;
    case HitagSWorkerCloneDump:
        hitags_worker_clone_dump(app);
        break;
    case HitagSWorkerWipeTag:
        hitags_worker_wipe_tag(app);
        break;
    case HitagSWorkerDebugRead:
        hitags_worker_debug_read(app);
        break;
    case HitagSWorkerTtfTiming:
        hitags_worker_ttf_timing(app);
        break;
    case HitagSWorkerDisturbTest:
        hitags_worker_disturb_test(app);
        break;
    case HitagSWorkerLateDisturbTest:
        hitags_worker_late_disturb_test(app);
        break;
    case HitagSWorkerLateCommandTest:
        hitags_worker_late_command_test(app);
        break;
    case HitagSWorkerT5577Detect:
        hitags_worker_t5577_detect(app);
        break;
    case HitagSWorkerEm4x05Detect:
        hitags_worker_em4x05_detect(app);
        break;
    case HitagSWorkerWriteStimulusVerify:
        hitags_worker_write_stimulus_verify(app);
        break;
    case HitagSWorkerHtuProbe:
        hitags_worker_htu_probe(app);
        break;
    case HitagSWorkerIdle:
        break;
    }

    return 0;
}

void hitags_writer_worker_start(HitagSApp* app, HitagSWorkerOp op) {
    furi_assert(!app->worker_running);
    app->worker_op = op;
    app->worker_running = true;
    furi_thread_start(app->worker_thread);
}

void hitags_writer_worker_stop(HitagSApp* app) {
    if(app->worker_running) {
        furi_thread_flags_set(furi_thread_get_id(app->worker_thread), HITAGS_WORKER_FLAG_STOP);
        furi_thread_join(app->worker_thread);
        app->worker_running = false;
    }
    app->worker_op = HitagSWorkerIdle;
}
