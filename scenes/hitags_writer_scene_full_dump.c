/**
 * @file hitags_writer_scene_full_dump.c
 * @brief Full Dump scene — reads all pages from 8268 tag and displays summary
 *
 * Performs full tag dump using hitag_s_8268_read_all() and displays
 * a summary of all pages with their data. Useful for debugging and
 * understanding tag contents before cloning.
 */

#include "../hitags_writer_i.h"

void hitags_writer_scene_full_dump_on_enter(void* context) {
    HitagSApp* app = context;
    Popup* popup = app->popup;

    popup_set_header(popup, "Dumping Tag...", 89, 30, AlignCenter, AlignTop);
    popup_set_text(popup, "Reading all pages\nfrom 8268 tag", 89, 43, AlignCenter, AlignTop);
    popup_set_icon(popup, 0, 3, &I_NFC_manual_60x50);

    view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewPopup);
    notification_message(app->notifications, &sequence_blink_start_cyan);

    hitags_writer_worker_start(app, HitagSWorkerFullDump);
}

bool hitags_writer_scene_full_dump_on_event(void* context, SceneManagerEvent event) {
    HitagSApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
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

            /* Show first few pages (limited by widget space) */
            /* Page 0: UID, Page 1: Config, Page 4-5: Data */
            /* We use the text_store for formatted display, showing key pages */

            widget_add_button_element(
                widget, GuiButtonTypeLeft, "Back", hitags_writer_widget_callback, app);
            widget_add_button_element(
                widget, GuiButtonTypeCenter, "Details", hitags_writer_widget_callback, app);

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
                errmsg = "No tag detected.\nPlace 8268 tag on\nFlipper's back.";
                break;
            case HitagSResultNack:
                errmsg = "Auth rejected.\nWrong password or\nnot 8268?";
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

            view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewWidget);
            consumed = true;

        } else if(event.event == GuiButtonTypeLeft) {
            scene_manager_previous_scene(app->scene_manager);
            consumed = true;

        } else if(event.event == GuiButtonTypeCenter) {
            /* Details: log all pages to serial console for debugging */
            FURI_LOG_I("Dump", "=== Full Tag Dump ===");
            FURI_LOG_I("Dump", "UID: %08lX", (unsigned long)app->tag_uid);
            for(int p = 0; p <= app->dump_max_page; p++) {
                if(app->dump_valid[p]) {
                    FURI_LOG_I("Dump", "Page[%2d]: %08lX",
                        p, (unsigned long)app->dump_pages[p]);
                } else {
                    FURI_LOG_I("Dump", "Page[%2d]: --------", p);
                }
            }
            FURI_LOG_I("Dump", "=== End Dump ===");

            /* Show notification that dump was logged */
            notification_message(app->notifications, &sequence_single_vibro);
            consumed = true;

        } else if(event.event == GuiButtonTypeRight) {
            /* Retry */
            widget_reset(app->widget);
            Popup* popup = app->popup;
            popup_reset(popup);
            popup_set_header(popup, "Dumping Tag...", 89, 30, AlignCenter, AlignTop);
            popup_set_text(popup, "Reading all pages\nfrom 8268 tag", 89, 43, AlignCenter, AlignTop);
            popup_set_icon(popup, 0, 3, &I_NFC_manual_60x50);
            view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewPopup);
            notification_message(app->notifications, &sequence_blink_start_cyan);
            hitags_writer_worker_start(app, HitagSWorkerFullDump);
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
