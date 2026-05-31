/**
 * @file hitags_worker.c
 * @brief Worker thread and operation dispatch for HiTagS Writer.
 */

#include "hitags_writer_i.h"

#define TAG "HitagSWorker"

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

static bool hitags_worker_probe_htu_once(const char* flow) {
    FURI_LOG_I(TAG, "%s: probing Hitag µ/8265 READ UID", flow);
    HitagHtuProbeInfo htu = {0};
    HitagSResult result = hitag_htu_probe_uid_sequence(&htu);
    if(result != HitagSResultOk || !htu.detected) return false;

    FURI_LOG_W(
        TAG,
        "%s: detected Hitag µ/8265 UID=%02X%02X%02X%02X%02X%02X; Hitag S flow not applicable",
        flow,
        htu.uid[0],
        htu.uid[1],
        htu.uid[2],
        htu.uid[3],
        htu.uid[4],
        htu.uid[5]);
    return true;
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
            "HTU Probe failed result=%d best=%s bits=%d candidates=%d",
            (int)app->last_result,
            app->htu_probe.method ? app->htu_probe.method : "none",
            (int)app->htu_probe.response_bits,
            (int)app->htu_probe.candidates_tried);
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
    bool htu_probe_done = false;
    for(int attempts = 1; !hitags_worker_should_stop(); attempts++) {
        app->last_result = hitag_s_read_uid_sequence(&app->tag_uid);

        if(app->last_result == HitagSResultOk) {
            FURI_LOG_I(TAG, "UID=%08lX (attempt %d)", (unsigned long)app->tag_uid, attempts);
            view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventReadOk);
            return;
        }

        if(!htu_probe_done) {
            htu_probe_done = true;
            if(hitags_worker_probe_htu_once("Read UID")) {
                view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventReadFailed);
                return;
            }
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
    bool htu_probe_done = false;

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

        if(!htu_probe_done) {
            htu_probe_done = true;
            if(hitags_worker_probe_htu_once("Read Tag Data")) {
                view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventReadFailed);
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

static void hitags_worker_debug_read(HitagSApp* app) {
    FURI_LOG_I(TAG, "Starting debug read with trace...");

    memset(app->dump_pages, 0, sizeof(app->dump_pages));
    memset(app->dump_valid, 0, sizeof(app->dump_valid));
    app->dump_max_page = 0;
    app->dump_read_count = 0;
    app->tag_uid = 0;
    app->debug_mode = HitagSModeStd;
    app->debug_stage = "-";

    if(app->debug_trace) {
        furi_string_free((FuriString*)app->debug_trace);
        app->debug_trace = NULL;
    }

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
        FURI_LOG_W(TAG, "Debug read captured only rejected noise; trace save disabled");
        app->debug_stage = "NOISE";
        furi_string_free((FuriString*)app->debug_trace);
        app->debug_trace = NULL;
        view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventDebugFailed);
    } else {
        FURI_LOG_W(TAG, "Debug read failed (result=%d)", (int)app->last_result);
        view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventDebugFailed);
    }
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
