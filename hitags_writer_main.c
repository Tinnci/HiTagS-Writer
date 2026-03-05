/**
 * @file hitags_writer.c
 * @brief HiTagS Writer — Main entry point
 *
 * Flipper Zero application for writing EM4100 card data to
 * HiTag S 8268 magic chips.
 */

#include "hitags_writer_i.h"

#define TAG "HitagSWriter"

/* --- Worker Thread --- */

static int32_t hitags_writer_worker_thread(void* context) {
    HitagSApp* app = context;

    if(app->worker_op == HitagSWorkerWrite) {
        /* Write: retry up to 15 times, then report failure.
         * Write sequence:
         *   1. Auth (UID_REQ → SELECT → 82xx password)
         *   2. Read current config page (page 1)
         *   3. Modify config for EM4100 TTF (TTFC=Manchester, TTFDR=fc/64, TTFM=pages 4-5)
         *   4. Write modified config to page 1
         *   5. Write EM4100 data to pages 4 and 5
         */
        Em4100HitagData hitag_data;
        em4100_prepare_hitag_data(app->em4100_id, &hitag_data);

        FURI_LOG_I(
            TAG,
            "Worker: Writing EM4100 ID %02X%02X%02X%02X%02X",
            app->em4100_id[0],
            app->em4100_id[1],
            app->em4100_id[2],
            app->em4100_id[3],
            app->em4100_id[4]);

        int attempts = 0;
        const int max_attempts = 15;

        while(true) {
            uint32_t flags = furi_thread_flags_get();
            if(flags & HITAGS_WORKER_FLAG_STOP) break;

            attempts++;

            /* Full write sequence with config read-modify-write */
            uint32_t config_page = 0;
            app->last_result =
                hitag_s_8268_write_em4100_sequence(app->password, &hitag_data, &config_page);

            if(app->last_result == HitagSResultOk) {
                FURI_LOG_I(
                    TAG,
                    "Worker: Write OK (attempt %d, config=%08lX)",
                    attempts,
                    (unsigned long)config_page);
                view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventWriteOk);
                break;
            }

            if(attempts >= max_attempts) {
                FURI_LOG_W(TAG, "Worker: Write failed after %d attempts", attempts);
                view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventWriteFailed);
                break;
            }

            uint32_t wait = furi_thread_flags_wait(HITAGS_WORKER_FLAG_STOP, FuriFlagWaitAny, 200);
            if(wait != (uint32_t)FuriFlagErrorTimeout) break;
        }
    } else if(app->worker_op == HitagSWorkerReadUid) {
        /* ReadUid: scan up to 15 times, then report failure */
        FURI_LOG_I(TAG, "Worker: Scanning for UID...");

        int attempts = 0;
        const int max_attempts = 15;

        while(true) {
            uint32_t flags = furi_thread_flags_get();
            if(flags & HITAGS_WORKER_FLAG_STOP) break;

            attempts++;
            app->last_result = hitag_s_read_uid_sequence(&app->tag_uid);

            if(app->last_result == HitagSResultOk) {
                FURI_LOG_I(
                    TAG, "Worker: UID=%08lX (attempt %d)", (unsigned long)app->tag_uid, attempts);
                view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventReadOk);
                break;
            }

            if(attempts >= max_attempts) {
                FURI_LOG_W(TAG, "Worker: UID read failed after %d attempts", attempts);
                view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventReadFailed);
                break;
            }

            uint32_t wait = furi_thread_flags_wait(HITAGS_WORKER_FLAG_STOP, FuriFlagWaitAny, 100);
            if(wait != (uint32_t)FuriFlagErrorTimeout) break;
        }
    } else if(app->worker_op == HitagSWorkerReadPages) {
        /* ReadPages: scan up to 15 times, then report failure */
        FURI_LOG_I(TAG, "Worker: Scanning to read tag data...");

        uint8_t page_addrs[3] = {1, 4, 5};
        int attempts = 0;
        const int max_attempts = 15;

        while(true) {
            uint32_t flags = furi_thread_flags_get();
            if(flags & HITAGS_WORKER_FLAG_STOP) break;

            attempts++;
            app->last_result = hitag_s_8268_read_sequence(
                app->password, app->read_pages, page_addrs, 3, &app->tag_uid);

            if(app->last_result == HitagSResultOk) {
                em4100_decode_hitag_data(app->read_pages[1], app->read_pages[2], app->read_id);
                FURI_LOG_I(
                    TAG,
                    "Worker: Read EM4100 %02X:%02X:%02X:%02X:%02X (attempt %d)",
                    app->read_id[0],
                    app->read_id[1],
                    app->read_id[2],
                    app->read_id[3],
                    app->read_id[4],
                    attempts);
                view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventReadOk);
                break;
            }

            if(attempts >= max_attempts) {
                FURI_LOG_W(TAG, "Worker: Read failed after %d attempts", attempts);
                view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventReadFailed);
                break;
            }

            uint32_t wait = furi_thread_flags_wait(HITAGS_WORKER_FLAG_STOP, FuriFlagWaitAny, 100);
            if(wait != (uint32_t)FuriFlagErrorTimeout) break;
        }
    } else if(app->worker_op == HitagSWorkerWriteUid) {
        /* WriteUid: UID_REQ → SELECT → Auth → Write page 0 (UID) */
        FURI_LOG_I(TAG, "Worker: Writing UID %08lX...", (unsigned long)app->target_uid);

        int attempts = 0;
        const int max_attempts = 15;

        while(true) {
            uint32_t flags = furi_thread_flags_get();
            if(flags & HITAGS_WORKER_FLAG_STOP) break;

            attempts++;

            /* Use generic write sequence: write page 0 with the new UID */
            uint32_t page_data[1] = {app->target_uid};
            uint8_t page_addrs[1] = {0};

            app->last_result =
                hitag_s_8268_write_sequence(app->password, page_data, page_addrs, 1);

            if(app->last_result == HitagSResultOk) {
                FURI_LOG_I(TAG, "Worker: Write UID OK (attempt %d)", attempts);
                view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventWriteUidOk);
                break;
            }

            if(attempts >= max_attempts) {
                FURI_LOG_W(TAG, "Worker: Write UID failed after %d attempts", attempts);
                view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventWriteUidFailed);
                break;
            }

            uint32_t wait = furi_thread_flags_wait(HITAGS_WORKER_FLAG_STOP, FuriFlagWaitAny, 200);
            if(wait != (uint32_t)FuriFlagErrorTimeout) break;
        }
    } else if(app->worker_op == HitagSWorkerFullDump) {
        /* FullDump: read all pages from tag */
        FURI_LOG_I(TAG, "Worker: Starting full tag dump...");

        memset(app->dump_pages, 0, sizeof(app->dump_pages));
        memset(app->dump_valid, 0, sizeof(app->dump_valid));
        app->dump_max_page = 0;
        app->dump_read_count = 0;

        int attempts = 0;
        const int max_attempts = 10;

        while(true) {
            uint32_t flags = furi_thread_flags_get();
            if(flags & HITAGS_WORKER_FLAG_STOP) break;

            attempts++;

            app->last_result = hitag_s_8268_read_all(
                app->password,
                app->dump_pages,
                app->dump_valid,
                &app->dump_max_page,
                &app->tag_uid);

            if(app->last_result == HitagSResultOk) {
                /* Count read pages */
                app->dump_read_count = 0;
                for(int p = 0; p <= app->dump_max_page; p++) {
                    if(app->dump_valid[p]) app->dump_read_count++;
                }

                FURI_LOG_I(
                    TAG,
                    "Worker: Dump OK — %d/%d pages (attempt %d)",
                    app->dump_read_count,
                    app->dump_max_page + 1,
                    attempts);
                view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventDumpOk);
                break;
            }

            if(attempts >= max_attempts) {
                FURI_LOG_W(TAG, "Worker: Dump failed after %d attempts", attempts);
                view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventDumpFailed);
                break;
            }

            uint32_t wait = furi_thread_flags_wait(HITAGS_WORKER_FLAG_STOP, FuriFlagWaitAny, 200);
            if(wait != (uint32_t)FuriFlagErrorTimeout) break;
        }
    } else if(app->worker_op == HitagSWorkerCloneDump) {
        /* CloneDump: write loaded dump data to 8268 (UID + config + data pages) */
        FURI_LOG_I(
            TAG,
            "Worker: Cloning dump UID=%08lX, %d pages...",
            (unsigned long)app->clone_uid,
            (int)app->clone_count);

        int attempts = 0;
        const int max_attempts = 15;

        while(true) {
            uint32_t flags = furi_thread_flags_get();
            if(flags & HITAGS_WORKER_FLAG_STOP) break;

            attempts++;

            app->last_result = hitag_s_8268_clone_sequence(
                app->password,
                app->clone_uid,
                app->clone_config,
                app->clone_pages,
                app->clone_addrs,
                app->clone_count);

            if(app->last_result == HitagSResultOk) {
                FURI_LOG_I(TAG, "Worker: Clone OK (attempt %d)", attempts);
                view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventCloneOk);
                break;
            }

            if(attempts >= max_attempts) {
                FURI_LOG_W(TAG, "Worker: Clone failed after %d attempts", attempts);
                view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventCloneFailed);
                break;
            }

            uint32_t wait2 = furi_thread_flags_wait(HITAGS_WORKER_FLAG_STOP, FuriFlagWaitAny, 200);
            if(wait2 != (uint32_t)FuriFlagErrorTimeout) break;
        }
    } else if(app->worker_op == HitagSWorkerWipeTag) {
        /* WipeTag: clear all pages and reset config to factory defaults */
        FURI_LOG_I(TAG, "Worker: Wiping tag...");

        int attempts = 0;
        const int max_attempts = 10;

        while(true) {
            uint32_t flags = furi_thread_flags_get();
            if(flags & HITAGS_WORKER_FLAG_STOP) break;

            attempts++;
            int wiped = 0;

            app->last_result =
                hitag_s_8268_wipe_sequence(app->password, 0 /* auto-detect */, &wiped);

            if(app->last_result == HitagSResultOk) {
                app->wipe_count = wiped;
                FURI_LOG_I(TAG, "Worker: Wipe OK, %d pages (attempt %d)", wiped, attempts);
                view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventWipeOk);
                break;
            }

            if(attempts >= max_attempts) {
                FURI_LOG_W(TAG, "Worker: Wipe failed after %d attempts", attempts);
                view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventWipeFailed);
                break;
            }

            uint32_t wait3 = furi_thread_flags_wait(HITAGS_WORKER_FLAG_STOP, FuriFlagWaitAny, 200);
            if(wait3 != (uint32_t)FuriFlagErrorTimeout) break;
        }
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
        /* Signal worker to stop */
        furi_thread_flags_set(furi_thread_get_id(app->worker_thread), HITAGS_WORKER_FLAG_STOP);
        furi_thread_join(app->worker_thread);
        app->worker_running = false;
    }
    app->worker_op = HitagSWorkerIdle;
}

/* --- Event Callbacks --- */

static bool hitags_writer_custom_event_callback(void* context, uint32_t event) {
    furi_assert(context);
    HitagSApp* app = context;
    return scene_manager_handle_custom_event(app->scene_manager, event);
}

static bool hitags_writer_back_event_callback(void* context) {
    furi_assert(context);
    HitagSApp* app = context;
    return scene_manager_handle_back_event(app->scene_manager);
}

/* --- Utility Callbacks --- */

void hitags_writer_popup_timeout_callback(void* context) {
    HitagSApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventPopupClosed);
}

void hitags_writer_widget_callback(GuiButtonType result, InputType type, void* context) {
    HitagSApp* app = context;
    if(type == InputTypeShort) {
        view_dispatcher_send_custom_event(app->view_dispatcher, result);
    }
}

/* --- Alloc / Free --- */

static HitagSApp* hitags_writer_alloc(void) {
    HitagSApp* app = malloc(sizeof(HitagSApp));

    /* Default password */
    app->password = HITAG_S_8268_PASSWORD;
    memset(app->em4100_id, 0, sizeof(app->em4100_id));
    app->tag_uid = 0;
    app->target_uid = 0;
    app->last_result = HitagSResultError;
    app->worker_running = false;
    app->worker_op = HitagSWorkerIdle;
    app->dump_max_page = 0;
    app->dump_read_count = 0;
    memset(app->uid_input, 0, sizeof(app->uid_input));
    memset(app->dump_pages, 0, sizeof(app->dump_pages));
    memset(app->dump_valid, 0, sizeof(app->dump_valid));

    /* Open services */
    app->storage = furi_record_open(RECORD_STORAGE);
    app->dialogs = furi_record_open(RECORD_DIALOGS);
    app->gui = furi_record_open(RECORD_GUI);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);

    /* String storage */
    app->file_path = furi_string_alloc_set(HITAGS_WRITER_APP_FOLDER);

    /* LFRFID protocol dict for file loading */
    app->dict = protocol_dict_alloc(lfrfid_protocols, LFRFIDProtocolMax);
    app->protocol_id = PROTOCOL_NO;

    /* Worker thread */
    app->worker_thread = furi_thread_alloc_ex(
        "HitagSWorker", HITAGS_WRITER_WORKER_STACK_SIZE, hitags_writer_worker_thread, app);

    /* Scene Manager */
    app->scene_manager = scene_manager_alloc(&hitags_writer_scene_handlers, app);

    /* View Dispatcher */
    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(
        app->view_dispatcher, hitags_writer_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, hitags_writer_back_event_callback);

    /* Submenu */
    app->submenu = submenu_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, HitagSViewSubmenu, submenu_get_view(app->submenu));

    /* Dialog */
    app->dialog_ex = dialog_ex_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, HitagSViewDialogEx, dialog_ex_get_view(app->dialog_ex));

    /* Popup */
    app->popup = popup_alloc();
    view_dispatcher_add_view(app->view_dispatcher, HitagSViewPopup, popup_get_view(app->popup));

    /* Widget */
    app->widget = widget_alloc();
    view_dispatcher_add_view(app->view_dispatcher, HitagSViewWidget, widget_get_view(app->widget));

    /* Byte Input */
    app->byte_input = byte_input_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, HitagSViewByteInput, byte_input_get_view(app->byte_input));

    /* Loading */
    app->loading = loading_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, HitagSViewLoading, loading_get_view(app->loading));

    return app;
}

static void hitags_writer_free(HitagSApp* app) {
    furi_assert(app);

    /* Stop worker if running */
    hitags_writer_worker_stop(app);
    furi_thread_free(app->worker_thread);

    /* String */
    furi_string_free(app->file_path);

    /* Protocol dict */
    protocol_dict_free(app->dict);

    /* Views */
    view_dispatcher_remove_view(app->view_dispatcher, HitagSViewSubmenu);
    submenu_free(app->submenu);

    view_dispatcher_remove_view(app->view_dispatcher, HitagSViewDialogEx);
    dialog_ex_free(app->dialog_ex);

    view_dispatcher_remove_view(app->view_dispatcher, HitagSViewPopup);
    popup_free(app->popup);

    view_dispatcher_remove_view(app->view_dispatcher, HitagSViewWidget);
    widget_free(app->widget);

    view_dispatcher_remove_view(app->view_dispatcher, HitagSViewByteInput);
    byte_input_free(app->byte_input);

    view_dispatcher_remove_view(app->view_dispatcher, HitagSViewLoading);
    loading_free(app->loading);

    /* Framework */
    view_dispatcher_free(app->view_dispatcher);
    scene_manager_free(app->scene_manager);

    /* Close records */
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_DIALOGS);

    free(app);
}

/* --- Entry Point --- */

int32_t hitags_writer_app(void* p) {
    UNUSED(p);

    HitagSApp* app = hitags_writer_alloc();

    FURI_LOG_I(TAG, "HiTagS Writer v0.3 starting...");

    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    scene_manager_next_scene(app->scene_manager, HitagSSceneStart);

    view_dispatcher_run(app->view_dispatcher);

    FURI_LOG_I(TAG, "HiTagS Writer exiting...");

    hitags_writer_free(app);

    return 0;
}
