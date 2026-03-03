/**
 * @file hitags_writer_scene_read_uid.c
 * @brief Read UID scene — reads and displays tag UID
 */

#include "../hitags_writer_i.h"

#define TAG "HitagSReadUID"

void hitags_writer_scene_read_uid_on_enter(void* context) {
    HitagSApp* app = context;
    Popup* popup = app->popup;

    /* Show scanning UI */
    popup_set_header(popup, "Reading...", 64, 20, AlignCenter, AlignTop);
    popup_set_text(popup, "Place 8268 tag on\nFlipper's back", 64, 35, AlignCenter, AlignTop);

    view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewPopup);
    notification_message(app->notifications, &sequence_blink_start_cyan);

    /* Execute UID read */
    app->last_result = hitag_s_read_uid_sequence(&app->tag_uid);

    if(app->last_result == HitagSResultOk) {
        FURI_LOG_I(TAG, "UID read: %08lX", (unsigned long)app->tag_uid);
        view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventReadOk);
    } else {
        FURI_LOG_W(TAG, "UID read failed");
        view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventReadFailed);
    }
}

bool hitags_writer_scene_read_uid_on_event(void* context, SceneManagerEvent event) {
    HitagSApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == HitagSEventReadOk) {
            /* Show UID in widget */
            notification_message(app->notifications, &sequence_blink_stop);
            notification_message(app->notifications, &sequence_success);

            Widget* widget = app->widget;
            widget_reset(widget);

            widget_add_string_element(
                widget, 64, 2, AlignCenter, AlignTop, FontPrimary, "Tag UID Found!");

            snprintf(
                app->text_store,
                sizeof(app->text_store),
                "UID: %08lX",
                (unsigned long)app->tag_uid);
            widget_add_string_element(
                widget, 64, 22, AlignCenter, AlignTop, FontSecondary, app->text_store);

            /* Break down UID bytes */
            snprintf(
                app->text_store,
                sizeof(app->text_store),
                "%02lX:%02lX:%02lX:%02lX",
                (unsigned long)((app->tag_uid >> 24) & 0xFF),
                (unsigned long)((app->tag_uid >> 16) & 0xFF),
                (unsigned long)((app->tag_uid >> 8) & 0xFF),
                (unsigned long)(app->tag_uid & 0xFF));
            widget_add_string_element(
                widget, 64, 36, AlignCenter, AlignTop, FontSecondary, app->text_store);

            widget_add_button_element(
                widget, GuiButtonTypeCenter, "OK", hitags_writer_widget_callback, app);

            view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewWidget);
            consumed = true;
        } else if(event.event == HitagSEventReadFailed) {
            notification_message(app->notifications, &sequence_blink_stop);
            notification_message(app->notifications, &sequence_error);

            Widget* widget = app->widget;
            widget_reset(widget);

            widget_add_string_element(
                widget, 64, 5, AlignCenter, AlignTop, FontPrimary, "No Tag Found");
            widget_add_string_element(
                widget,
                64,
                25,
                AlignCenter,
                AlignTop,
                FontSecondary,
                "Place a HiTag S\n8268 tag on\nFlipper's back");

            widget_add_button_element(
                widget, GuiButtonTypeLeft, "Back", hitags_writer_widget_callback, app);
            widget_add_button_element(
                widget, GuiButtonTypeRight, "Retry", hitags_writer_widget_callback, app);

            view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewWidget);
            consumed = true;
        } else if(event.event == GuiButtonTypeCenter || event.event == GuiButtonTypeLeft) {
            scene_manager_previous_scene(app->scene_manager);
            consumed = true;
        } else if(event.event == GuiButtonTypeRight) {
            /* Retry — re-enter scene */
            scene_manager_previous_scene(app->scene_manager);
            scene_manager_next_scene(app->scene_manager, HitagSSceneReadUid);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        scene_manager_previous_scene(app->scene_manager);
        consumed = true;
    }

    return consumed;
}

void hitags_writer_scene_read_uid_on_exit(void* context) {
    HitagSApp* app = context;
    notification_message(app->notifications, &sequence_blink_stop);
    popup_reset(app->popup);
    widget_reset(app->widget);
}
