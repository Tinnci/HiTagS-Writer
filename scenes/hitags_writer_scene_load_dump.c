/**
 * @file hitags_writer_scene_load_dump.c
 * @brief Load Dump scene — browse .hts files, load tag dump, and clone to 8268
 *
 * Flow: File browser (.hts) → Show summary → Confirm → Clone (worker) → Success/Fail
 *
 * Uses hitag_s_dump_load() to parse the file, then hitag_s_8268_clone_sequence()
 * via the CloneDump worker to write UID + config + data pages to a target 8268 tag.
 */

#include "../hitags_writer_i.h"

#define HITAGS_DUMP_FOLDER    EXT_PATH("lfrfid")
#define HITAGS_DUMP_EXTENSION ".hts"

typedef enum {
    LoadDumpStateBrowser,
    LoadDumpStateConfirm,
    LoadDumpStateWriting,
} LoadDumpState;

static void hitags_writer_scene_load_dump_confirm_cb(
    DialogExResult result,
    void* context) {
    HitagSApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, result);
}

/**
 * Prepare clone data from loaded dump pages.
 * Selects which pages to write: data pages 4+ that have valid data.
 */
static void hitags_writer_prepare_clone_data(HitagSApp* app) {
    app->clone_count = 0;

    /* Collect data pages (4+) that have valid data */
    for(int p = 4; p < HITAG_S_MAX_PAGES; p++) {
        if(app->dump_valid[p]) {
            app->clone_addrs[app->clone_count] = (uint8_t)p;
            app->clone_pages[app->clone_count] = app->dump_pages[p];
            app->clone_count++;
        }
    }

    /* UID from page 0 */
    app->clone_uid = app->dump_pages[0];

    /* Config from page 1 */
    app->clone_config = app->dump_valid[1] ? app->dump_pages[1] : 0;

    FURI_LOG_I("LoadDump", "Clone prepared: UID=%08lX config=%08lX %d data pages",
        (unsigned long)app->clone_uid,
        (unsigned long)app->clone_config,
        (int)app->clone_count);
}

void hitags_writer_scene_load_dump_on_enter(void* context) {
    HitagSApp* app = context;

    /* Show file browser for .hts files */
    DialogsFileBrowserOptions browser_options;
    dialog_file_browser_set_basic_options(
        &browser_options, HITAGS_DUMP_EXTENSION, NULL);
    browser_options.base_path = HITAGS_DUMP_FOLDER;
    browser_options.hide_dot_files = true;

    scene_manager_set_scene_state(
        app->scene_manager, HitagSSceneLoadDump, LoadDumpStateBrowser);

    bool result = dialog_file_browser_show(
        app->dialogs, app->file_path, app->file_path, &browser_options);

    if(!result) {
        /* User cancelled */
        scene_manager_previous_scene(app->scene_manager);
        return;
    }

    /* Load the dump file */
    uint32_t loaded_uid = 0;
    int loaded_max_page = 0;

    bool loaded = hitag_s_dump_load(
        app->storage,
        furi_string_get_cstr(app->file_path),
        &loaded_uid,
        app->dump_pages,
        app->dump_valid,
        &loaded_max_page);

    if(!loaded) {
        dialog_message_show_storage_error(app->dialogs, "Failed to load\n.hts dump file!");
        scene_manager_previous_scene(app->scene_manager);
        return;
    }

    app->dump_max_page = loaded_max_page;

    /* Prepare clone data from loaded dump */
    hitags_writer_prepare_clone_data(app);

    /* Count valid data pages */
    int data_pages = 0;
    for(int p = 4; p <= loaded_max_page; p++) {
        if(app->dump_valid[p]) data_pages++;
    }

    /* Show confirm dialog with dump summary */
    DialogEx* dialog = app->dialog_ex;
    dialog_ex_reset(dialog);

    HitagSConfig cfg = hitag_s_parse_config(
        app->dump_valid[1] ? app->dump_pages[1] : 0);

    snprintf(
        app->text_store,
        sizeof(app->text_store),
        "UID: %08lX\nMEMT:%d %d data pgs\nauth:%d TTFM:%d",
        (unsigned long)loaded_uid,
        cfg.MEMT,
        data_pages,
        cfg.auth,
        cfg.TTFM);

    dialog_ex_set_header(dialog, "Clone from Dump", 64, 0, AlignCenter, AlignTop);
    dialog_ex_set_text(dialog, app->text_store, 64, 14, AlignCenter, AlignTop);
    dialog_ex_set_left_button_text(dialog, "Cancel");
    dialog_ex_set_right_button_text(dialog, "Clone");
    dialog_ex_set_result_callback(dialog, hitags_writer_scene_load_dump_confirm_cb);
    dialog_ex_set_context(dialog, app);

    scene_manager_set_scene_state(
        app->scene_manager, HitagSSceneLoadDump, LoadDumpStateConfirm);
    view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewDialogEx);
}

bool hitags_writer_scene_load_dump_on_event(void* context, SceneManagerEvent event) {
    HitagSApp* app = context;
    bool consumed = false;
    LoadDumpState state = scene_manager_get_scene_state(
        app->scene_manager, HitagSSceneLoadDump);

    if(event.type == SceneManagerEventTypeCustom) {
        if(state == LoadDumpStateConfirm && event.event == DialogExResultRight) {
            /* Confirmed — start cloning */
            Popup* popup = app->popup;
            popup_reset(popup);
            popup_set_header(popup, "Cloning...", 89, 30, AlignCenter, AlignTop);
            snprintf(
                app->text_store,
                sizeof(app->text_store),
                "UID: %08lX\n%d pages",
                (unsigned long)app->clone_uid,
                (int)app->clone_count);
            popup_set_text(popup, app->text_store, 89, 43, AlignCenter, AlignTop);
            popup_set_icon(popup, 0, 3, &I_RFIDDolphinSend_97x61);

            scene_manager_set_scene_state(
                app->scene_manager, HitagSSceneLoadDump, LoadDumpStateWriting);
            view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewPopup);
            notification_message(app->notifications, &sequence_blink_start_magenta);

            hitags_writer_worker_start(app, HitagSWorkerCloneDump);
            consumed = true;

        } else if(state == LoadDumpStateConfirm && event.event == DialogExResultLeft) {
            /* Cancelled */
            scene_manager_previous_scene(app->scene_manager);
            consumed = true;

        } else if(state == LoadDumpStateWriting && event.event == HitagSEventCloneOk) {
            hitags_writer_worker_stop(app);
            notification_message(app->notifications, &sequence_blink_stop);
            notification_message(app->notifications, &sequence_success);

            /* Success popup */
            Popup* popup = app->popup;
            popup_reset(popup);
            popup_set_header(popup, "Clone Done!", 97, 12, AlignCenter, AlignTop);
            popup_set_icon(popup, 0, 9, &I_DolphinSuccess_91x55);
            snprintf(
                app->text_store,
                sizeof(app->text_store),
                "UID: %08lX\n%d pages cloned",
                (unsigned long)app->clone_uid,
                (int)app->clone_count);
            popup_set_text(popup, app->text_store, 97, 25, AlignCenter, AlignTop);
            popup_set_context(popup, app);
            popup_set_callback(popup, hitags_writer_popup_timeout_callback);
            popup_set_timeout(popup, 2500);
            popup_enable_timeout(popup);
            view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewPopup);
            consumed = true;

        } else if(state == LoadDumpStateWriting && event.event == HitagSEventCloneFailed) {
            hitags_writer_worker_stop(app);
            notification_message(app->notifications, &sequence_blink_stop);
            notification_message(app->notifications, &sequence_error);

            /* Failure widget */
            Widget* widget = app->widget;
            widget_reset(widget);
            widget_add_icon_element(widget, 83, 22, &I_WarningDolphinFlip_45x42);
            widget_add_string_element(
                widget, 40, 5, AlignCenter, AlignTop, FontPrimary, "Clone Failed!");

            const char* errmsg;
            switch(app->last_result) {
            case HitagSResultTimeout:
                errmsg = "No tag detected.\nPlace 8268 tag on\nFlipper's back.";
                break;
            case HitagSResultNack:
                errmsg = "Auth rejected.\nWrong password or\nnot 8268 magic?";
                break;
            case HitagSResultError:
                errmsg = "Write verify\nfailed. Try again.";
                break;
            default:
                errmsg = "Clone error.\nTry again.";
                break;
            }

            widget_add_string_multiline_element(
                widget, 40, 22, AlignCenter, AlignTop, FontSecondary, errmsg);
            widget_add_button_element(
                widget, GuiButtonTypeLeft, "Back", hitags_writer_widget_callback, app);
            widget_add_button_element(
                widget, GuiButtonTypeRight, "Retry", hitags_writer_widget_callback, app);
            view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewWidget);
            consumed = true;

        } else if(state == LoadDumpStateWriting && event.event == GuiButtonTypeLeft) {
            /* Back from failure */
            const uint32_t prev[] = {HitagSSceneStart};
            scene_manager_search_and_switch_to_previous_scene_one_of(
                app->scene_manager, prev, COUNT_OF(prev));
            consumed = true;

        } else if(state == LoadDumpStateWriting && event.event == GuiButtonTypeRight) {
            /* Retry clone */
            widget_reset(app->widget);
            Popup* popup = app->popup;
            popup_reset(popup);
            popup_set_header(popup, "Cloning...", 89, 30, AlignCenter, AlignTop);
            snprintf(
                app->text_store,
                sizeof(app->text_store),
                "UID: %08lX\n%d pages",
                (unsigned long)app->clone_uid,
                (int)app->clone_count);
            popup_set_text(popup, app->text_store, 89, 43, AlignCenter, AlignTop);
            popup_set_icon(popup, 0, 3, &I_RFIDDolphinSend_97x61);
            view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewPopup);
            notification_message(app->notifications, &sequence_blink_start_magenta);
            hitags_writer_worker_start(app, HitagSWorkerCloneDump);
            consumed = true;

        } else if(event.event == HitagSEventPopupClosed) {
            /* Success timeout — return to start */
            const uint32_t prev[] = {HitagSSceneStart};
            scene_manager_search_and_switch_to_previous_scene_one_of(
                app->scene_manager, prev, COUNT_OF(prev));
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        if(state == LoadDumpStateWriting) {
            hitags_writer_worker_stop(app);
        }
        scene_manager_previous_scene(app->scene_manager);
        consumed = true;
    }

    return consumed;
}

void hitags_writer_scene_load_dump_on_exit(void* context) {
    HitagSApp* app = context;
    hitags_writer_worker_stop(app);
    notification_message(app->notifications, &sequence_blink_stop);
    popup_reset(app->popup);
    dialog_ex_reset(app->dialog_ex);
    widget_reset(app->widget);
}
