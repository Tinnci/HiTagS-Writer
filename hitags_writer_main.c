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
        /* Write: retry up to 15 times, then report failure */
        Em4100HitagData hitag_data;
        em4100_prepare_hitag_data(app->em4100_id, &hitag_data);

        uint32_t pages[3] = {
            hitag_data.config_page,
            hitag_data.data_hi,
            hitag_data.data_lo,
        };
        uint8_t page_addrs[3] = {1, 4, 5};

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
            app->last_result =
                hitag_s_8268_write_sequence(app->password, pages, page_addrs, 3);

            if(app->last_result == HitagSResultOk) {
                FURI_LOG_I(TAG, "Worker: Write OK (attempt %d)", attempts);
                view_dispatcher_send_custom_event(
                    app->view_dispatcher, HitagSEventWriteOk);
                break;
            }

            if(attempts >= max_attempts) {
                FURI_LOG_W(TAG, "Worker: Write failed after %d attempts", attempts);
                view_dispatcher_send_custom_event(
                    app->view_dispatcher, HitagSEventWriteFailed);
                break;
            }

            uint32_t wait = furi_thread_flags_wait(
                HITAGS_WORKER_FLAG_STOP, FuriFlagWaitAny, 200);
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
                FURI_LOG_I(TAG, "Worker: UID=%08lX (attempt %d)",
                    (unsigned long)app->tag_uid, attempts);
                view_dispatcher_send_custom_event(
                    app->view_dispatcher, HitagSEventReadOk);
                break;
            }

            if(attempts >= max_attempts) {
                FURI_LOG_W(TAG, "Worker: UID read failed after %d attempts", attempts);
                view_dispatcher_send_custom_event(
                    app->view_dispatcher, HitagSEventReadFailed);
                break;
            }

            uint32_t wait = furi_thread_flags_wait(
                HITAGS_WORKER_FLAG_STOP, FuriFlagWaitAny, 100);
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
                em4100_decode_hitag_data(
                    app->read_pages[1], app->read_pages[2], app->read_id);
                FURI_LOG_I(
                    TAG,
                    "Worker: Read EM4100 %02X:%02X:%02X:%02X:%02X (attempt %d)",
                    app->read_id[0],
                    app->read_id[1],
                    app->read_id[2],
                    app->read_id[3],
                    app->read_id[4],
                    attempts);
                view_dispatcher_send_custom_event(
                    app->view_dispatcher, HitagSEventReadOk);
                break;
            }

            if(attempts >= max_attempts) {
                FURI_LOG_W(TAG, "Worker: Read failed after %d attempts", attempts);
                view_dispatcher_send_custom_event(
                    app->view_dispatcher, HitagSEventReadFailed);
                break;
            }

            uint32_t wait = furi_thread_flags_wait(
                HITAGS_WORKER_FLAG_STOP, FuriFlagWaitAny, 100);
            if(wait != (uint32_t)FuriFlagErrorTimeout) break;
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
    app->last_result = HitagSResultError;
    app->worker_running = false;
    app->worker_op = HitagSWorkerIdle;

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
    view_dispatcher_add_view(
        app->view_dispatcher, HitagSViewPopup, popup_get_view(app->popup));

    /* Widget */
    app->widget = widget_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, HitagSViewWidget, widget_get_view(app->widget));

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
