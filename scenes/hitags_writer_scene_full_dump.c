/**
 * @file hitags_writer_scene_full_dump.c
 * @brief Full Dump scene — reads all pages from 8268 tag and displays summary
 *
 * Performs full tag dump using hitag_s_8268_read_all() and displays
 * a summary of all pages with their data. Includes save to file.
 */

#include "../hitags_writer_i.h"

#define HITAGS_DUMP_FOLDER    EXT_PATH("lfrfid")
#define HITAGS_DUMP_EXTENSION ".hts"

typedef enum {
    FullDumpStateScanning,
    FullDumpStateSuccess,
    FullDumpStateFailed,
} FullDumpState;

static void hitags_writer_scene_full_dump_save_file(HitagSApp* app) {
    /* Generate filename from UID */
    FuriString* filename = furi_string_alloc();
    furi_string_printf(
        filename,
        "%s/HiTagS_%08lX%s",
        HITAGS_DUMP_FOLDER,
        (unsigned long)app->tag_uid,
        HITAGS_DUMP_EXTENSION);

    /* Ensure directory exists */
    storage_simply_mkdir(app->storage, HITAGS_DUMP_FOLDER);

    bool saved = hitag_s_dump_save(
        app->storage,
        furi_string_get_cstr(filename),
        app->tag_uid,
        app->dump_pages,
        app->dump_valid,
        app->dump_max_page);

    if(saved) {
        notification_message(app->notifications, &sequence_success);

        /* Show save success in popup */
        Popup* popup = app->popup;
        popup_reset(popup);
        popup_set_header(popup, "Saved!", 97, 12, AlignCenter, AlignTop);
        popup_set_icon(popup, 0, 9, &I_DolphinSuccess_91x55);
        snprintf(
            app->text_store, sizeof(app->text_store), "%08lX.hts", (unsigned long)app->tag_uid);
        popup_set_text(popup, app->text_store, 97, 25, AlignCenter, AlignTop);
        popup_set_context(popup, app);
        popup_set_callback(popup, hitags_writer_popup_timeout_callback);
        popup_set_timeout(popup, 2000);
        popup_enable_timeout(popup);
        view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewPopup);
    } else {
        notification_message(app->notifications, &sequence_error);
        dialog_message_show_storage_error(app->dialogs, "Failed to\nsave dump!");
    }

    furi_string_free(filename);
}

void hitags_writer_scene_full_dump_on_enter(void* context) {
    HitagSApp* app = context;
    Popup* popup = app->popup;

    popup_set_header(popup, "Dumping...", 89, 30, AlignCenter, AlignTop);
    popup_set_text(popup, "Reading pages\nfrom 8268 tag", 89, 43, AlignCenter, AlignTop);
    popup_set_icon(popup, 0, 3, &I_NFC_manual_60x50);

    view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewPopup);
    notification_message(app->notifications, &sequence_blink_start_cyan);

    scene_manager_set_scene_state(app->scene_manager, HitagSSceneFullDump, FullDumpStateScanning);
    hitags_writer_worker_start(app, HitagSWorkerFullDump);
}

bool hitags_writer_scene_full_dump_on_event(void* context, SceneManagerEvent event) {
    HitagSApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        FullDumpState state =
            scene_manager_get_scene_state(app->scene_manager, HitagSSceneFullDump);

        if(event.event == HitagSEventDumpOk) {
            hitags_writer_worker_stop(app);
            notification_message(app->notifications, &sequence_blink_stop);
            notification_message(app->notifications, &sequence_success);

            /* Build result display in widget */
            Widget* widget = app->widget;
            widget_reset(widget);

            widget_add_string_element(
                widget, 64, 0, AlignCenter, AlignTop, FontPrimary, "Tag Dump");

            /* Summary line */
            HitagSConfig cfg = hitag_s_parse_config(app->dump_pages[1]);
            snprintf(
                app->text_store,
                sizeof(app->text_store),
                "UID:%08lX MEMT:%d\n%d/%d pgs auth:%d",
                (unsigned long)app->tag_uid,
                cfg.MEMT,
                app->dump_read_count,
                app->dump_max_page + 1,
                cfg.auth);
            widget_add_string_multiline_element(
                widget, 64, 13, AlignCenter, AlignTop, FontSecondary, app->text_store);

            widget_add_button_element(
                widget, GuiButtonTypeLeft, "Back", hitags_writer_widget_callback, app);
            widget_add_button_element(
                widget, GuiButtonTypeCenter, "Save", hitags_writer_widget_callback, app);
            widget_add_button_element(
                widget, GuiButtonTypeRight, "Log", hitags_writer_widget_callback, app);

            scene_manager_set_scene_state(
                app->scene_manager, HitagSSceneFullDump, FullDumpStateSuccess);
            view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewWidget);
            consumed = true;

        } else if(event.event == HitagSEventDumpFailed) {
            hitags_writer_worker_stop(app);
            notification_message(app->notifications, &sequence_blink_stop);
            notification_message(app->notifications, &sequence_error);

            Widget* widget = app->widget;
            widget_reset(widget);

            widget_add_icon_element(widget, 83, 22, &I_WarningDolphinFlip_45x42);
            widget_add_string_element(
                widget, 40, 5, AlignCenter, AlignTop, FontPrimary, "Dump Failed!");

            const char* errmsg;
            switch(app->last_result) {
            case HitagSResultTimeout:
                errmsg = "No tag found.\nPlace tag on\nFlipper back.";
                break;
            case HitagSResultNack:
                errmsg = "Auth rejected.\nWrong password\nor not 8268?";
                break;
            default:
                errmsg = "Read error.\nTry again.";
                break;
            }

            widget_add_string_multiline_element(
                widget, 40, 22, AlignCenter, AlignTop, FontSecondary, errmsg);
            widget_add_button_element(
                widget, GuiButtonTypeLeft, "Back", hitags_writer_widget_callback, app);
            widget_add_button_element(
                widget, GuiButtonTypeRight, "Retry", hitags_writer_widget_callback, app);

            scene_manager_set_scene_state(
                app->scene_manager, HitagSSceneFullDump, FullDumpStateFailed);
            view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewWidget);
            consumed = true;

        } else if(event.event == GuiButtonTypeLeft) {
            scene_manager_previous_scene(app->scene_manager);
            consumed = true;

        } else if(event.event == GuiButtonTypeCenter && state == FullDumpStateSuccess) {
            /* Save dump to file */
            hitags_writer_scene_full_dump_save_file(app);
            consumed = true;

        } else if(event.event == GuiButtonTypeRight && state == FullDumpStateSuccess) {
            /* Log all pages to serial console */
            FURI_LOG_I("Dump", "=== Full Tag Dump ===");
            FURI_LOG_I("Dump", "UID: %08lX", (unsigned long)app->tag_uid);
            for(int p = 0; p <= app->dump_max_page; p++) {
                if(app->dump_valid[p]) {
                    FURI_LOG_I("Dump", "Page[%2d]: %08lX", p, (unsigned long)app->dump_pages[p]);
                } else {
                    FURI_LOG_I("Dump", "Page[%2d]: --------", p);
                }
            }
            FURI_LOG_I("Dump", "=== End Dump ===");
            notification_message(app->notifications, &sequence_single_vibro);
            consumed = true;

        } else if(event.event == GuiButtonTypeRight && state == FullDumpStateFailed) {
            /* Retry */
            widget_reset(app->widget);
            Popup* popup = app->popup;
            popup_reset(popup);
            popup_set_header(popup, "Dumping...", 89, 30, AlignCenter, AlignTop);
            popup_set_text(popup, "Reading pages\nfrom 8268 tag", 89, 43, AlignCenter, AlignTop);
            popup_set_icon(popup, 0, 3, &I_NFC_manual_60x50);
            scene_manager_set_scene_state(
                app->scene_manager, HitagSSceneFullDump, FullDumpStateScanning);
            view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewPopup);
            notification_message(app->notifications, &sequence_blink_start_cyan);
            hitags_writer_worker_start(app, HitagSWorkerFullDump);
            consumed = true;

        } else if(event.event == HitagSEventPopupClosed) {
            /* Save success popup timeout — return to dump result widget */
            /* Re-show the success widget (dump data is still in memory) */
            view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewWidget);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        hitags_writer_worker_stop(app);
        scene_manager_previous_scene(app->scene_manager);
        consumed = true;
    }

    return consumed;
}

void hitags_writer_scene_full_dump_on_exit(void* context) {
    HitagSApp* app = context;
    hitags_writer_worker_stop(app);
    notification_message(app->notifications, &sequence_blink_stop);
    popup_reset(app->popup);
    widget_reset(app->widget);
}
