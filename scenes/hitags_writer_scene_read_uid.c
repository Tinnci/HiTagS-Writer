/**
 * @file hitags_writer_scene_read_uid.c
 * @brief Read UID scene — reads and displays tag UID via worker thread
 */

#include "../hitags_writer_i.h"

void hitags_writer_scene_read_uid_on_enter(void* context) {
    HitagSApp* app = context;
    Popup* popup = app->popup;

    /* Show scanning UI with dolphin icon */
    popup_set_header(popup, "Reading...", 89, 30, AlignCenter, AlignTop);
    popup_set_text(popup, "Place 8268 tag on\nFlipper's back", 89, 43, AlignCenter, AlignTop);
    popup_set_icon(popup, 0, 3, &I_NFC_manual_60x50);

    view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewPopup);
    notification_message(app->notifications, &sequence_blink_start_cyan);

    /* Start UID read in worker thread — non-blocking! */
    hitags_writer_worker_start(app, HitagSWorkerReadUid);
}

bool hitags_writer_scene_read_uid_on_event(void* context, SceneManagerEvent event) {
    HitagSApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == HitagSEventReadOk) {
            hitags_writer_worker_stop(app);
            notification_message(app->notifications, &sequence_blink_stop);
            notification_message(app->notifications, &sequence_success);

            /* Show UID results in widget */
            Widget* widget = app->widget;
            widget_reset(widget);

            widget_add_icon_element(widget, 0, 9, &I_DolphinSuccess_91x55);

            widget_add_string_element(
                widget, 97, 2, AlignCenter, AlignTop, FontPrimary, "Tag Found!");

            snprintf(
                app->text_store,
                sizeof(app->text_store),
                "UID: %08lX\n%02lX:%02lX:%02lX:%02lX",
                (unsigned long)app->tag_uid,
                (unsigned long)((app->tag_uid >> 24) & 0xFF),
                (unsigned long)((app->tag_uid >> 16) & 0xFF),
                (unsigned long)((app->tag_uid >> 8) & 0xFF),
                (unsigned long)(app->tag_uid & 0xFF));
            widget_add_string_multiline_element(
                widget, 97, 16, AlignCenter, AlignTop, FontSecondary, app->text_store);

            widget_add_button_element(
                widget, GuiButtonTypeCenter, "OK", hitags_writer_widget_callback, app);

            view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewWidget);
            consumed = true;
        } else if(event.event == HitagSEventReadFailed) {
            hitags_writer_worker_stop(app);
            notification_message(app->notifications, &sequence_blink_stop);
            notification_message(app->notifications, &sequence_error);

            /* Show error in widget */
            Widget* widget = app->widget;
            widget_reset(widget);

            widget_add_icon_element(widget, 83, 22, &I_WarningDolphinFlip_45x42);
            widget_add_string_element(
                widget, 40, 5, AlignCenter, AlignTop, FontPrimary, "Read Failed!");

            const char* errmsg;
            switch(app->last_result) {
            case HitagSResultTimeout:
                errmsg = "No tag detected.\nPlace 8268 tag on\nFlipper's back.";
                break;
            default:
                errmsg = "Could not read UID.\nTry again.";
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
        } else if(event.event == GuiButtonTypeCenter) {
            scene_manager_previous_scene(app->scene_manager);
            consumed = true;
        } else if(event.event == GuiButtonTypeLeft) {
            scene_manager_previous_scene(app->scene_manager);
            consumed = true;
        } else if(event.event == GuiButtonTypeRight) {
            /* Retry: restart worker */
            widget_reset(app->widget);
            popup_set_header(app->popup, "Reading...", 89, 30, AlignCenter, AlignTop);
            popup_set_text(app->popup, "Place 8268 tag on\nFlipper's back", 89, 43, AlignCenter, AlignTop);
            popup_set_icon(app->popup, 0, 3, &I_NFC_manual_60x50);
            view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewPopup);
            notification_message(app->notifications, &sequence_blink_start_cyan);
            hitags_writer_worker_start(app, HitagSWorkerReadUid);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        hitags_writer_worker_stop(app);
        scene_manager_previous_scene(app->scene_manager);
        consumed = true;
    }

    return consumed;
}

void hitags_writer_scene_read_uid_on_exit(void* context) {
    HitagSApp* app = context;
    hitags_writer_worker_stop(app);
    notification_message(app->notifications, &sequence_blink_stop);
    popup_reset(app->popup);
    widget_reset(app->widget);
}
