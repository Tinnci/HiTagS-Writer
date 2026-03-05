/**
 * @file hitags_writer_scene_wipe_tag.c
 * @brief Wipe Tag scene — confirm and wipe 8268 to factory defaults
 *
 * Flow: Confirm dialog → Wipe (worker) → Success/Fail
 */

#include "../hitags_writer_i.h"

typedef enum {
    WipeTagStateConfirm,
    WipeTagStateWiping,
} WipeTagState;

static void hitags_writer_scene_wipe_tag_confirm_cb(DialogExResult result, void* context) {
    HitagSApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, result);
}

void hitags_writer_scene_wipe_tag_on_enter(void* context) {
    HitagSApp* app = context;

    /* Show warning/confirm dialog */
    DialogEx* dialog = app->dialog_ex;
    dialog_ex_reset(dialog);

    dialog_ex_set_header(dialog, "Wipe 8268 Tag?", 64, 0, AlignCenter, AlignTop);
    dialog_ex_set_text(
        dialog,
        "This will ERASE all data\n"
        "and reset config+PWD\n"
        "to factory defaults.",
        64,
        16,
        AlignCenter,
        AlignTop);
    dialog_ex_set_left_button_text(dialog, "Cancel");
    dialog_ex_set_right_button_text(dialog, "Wipe!");
    dialog_ex_set_result_callback(dialog, hitags_writer_scene_wipe_tag_confirm_cb);
    dialog_ex_set_context(dialog, app);

    scene_manager_set_scene_state(app->scene_manager, HitagSSceneWipeTag, WipeTagStateConfirm);
    view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewDialogEx);
}

bool hitags_writer_scene_wipe_tag_on_event(void* context, SceneManagerEvent event) {
    HitagSApp* app = context;
    bool consumed = false;
    WipeTagState state = scene_manager_get_scene_state(app->scene_manager, HitagSSceneWipeTag);

    if(event.type == SceneManagerEventTypeCustom) {
        if(state == WipeTagStateConfirm && event.event == DialogExResultRight) {
            /* Confirmed — start wiping */
            Popup* popup = app->popup;
            popup_reset(popup);
            popup_set_header(popup, "Wiping Tag...", 89, 30, AlignCenter, AlignTop);
            popup_set_text(popup, "Place tag on\nFlipper back", 89, 43, AlignCenter, AlignTop);
            popup_set_icon(popup, 0, 3, &I_RFIDDolphinSend_97x61);

            scene_manager_set_scene_state(
                app->scene_manager, HitagSSceneWipeTag, WipeTagStateWiping);
            view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewPopup);
            notification_message(app->notifications, &sequence_blink_start_red);

            hitags_writer_worker_start(app, HitagSWorkerWipeTag);
            consumed = true;

        } else if(state == WipeTagStateConfirm && event.event == DialogExResultLeft) {
            /* Cancelled */
            scene_manager_previous_scene(app->scene_manager);
            consumed = true;

        } else if(state == WipeTagStateWiping && event.event == HitagSEventWipeOk) {
            hitags_writer_worker_stop(app);
            notification_message(app->notifications, &sequence_blink_stop);
            notification_message(app->notifications, &sequence_success);

            /* Show success popup */
            Popup* popup = app->popup;
            popup_reset(popup);
            popup_set_header(popup, "Tag Wiped!", 97, 12, AlignCenter, AlignTop);
            popup_set_icon(popup, 0, 9, &I_DolphinSuccess_91x55);
            snprintf(
                app->text_store,
                sizeof(app->text_store),
                "%d pgs wiped\nConfig reset",
                app->wipe_count);
            popup_set_text(popup, app->text_store, 97, 25, AlignCenter, AlignTop);
            popup_set_context(popup, app);
            popup_set_callback(popup, hitags_writer_popup_timeout_callback);
            popup_set_timeout(popup, 2500);
            popup_enable_timeout(popup);
            view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewPopup);
            consumed = true;

        } else if(state == WipeTagStateWiping && event.event == HitagSEventWipeFailed) {
            hitags_writer_worker_stop(app);
            notification_message(app->notifications, &sequence_blink_stop);
            notification_message(app->notifications, &sequence_error);

            /* Show failure widget */
            Widget* widget = app->widget;
            widget_reset(widget);
            widget_add_icon_element(widget, 83, 22, &I_WarningDolphinFlip_45x42);
            widget_add_string_element(
                widget, 40, 5, AlignCenter, AlignTop, FontPrimary, "Wipe Failed!");

            const char* errmsg;
            switch(app->last_result) {
            case HitagSResultTimeout:
                errmsg = "No tag found.\nPlace tag on\nFlipper back.";
                break;
            case HitagSResultNack:
                errmsg = "Auth rejected.\nWrong password\nor not 8268?";
                break;
            default:
                errmsg = "Wipe error.\nTry again.";
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

        } else if(state == WipeTagStateWiping && event.event == GuiButtonTypeLeft) {
            /* Back from failure */
            const uint32_t prev[] = {HitagSSceneStart};
            scene_manager_search_and_switch_to_previous_scene_one_of(
                app->scene_manager, prev, COUNT_OF(prev));
            consumed = true;

        } else if(state == WipeTagStateWiping && event.event == GuiButtonTypeRight) {
            /* Retry wipe */
            widget_reset(app->widget);
            Popup* popup = app->popup;
            popup_reset(popup);
            popup_set_header(popup, "Wiping Tag...", 89, 30, AlignCenter, AlignTop);
            popup_set_text(popup, "Place tag on\nFlipper back", 89, 43, AlignCenter, AlignTop);
            popup_set_icon(popup, 0, 3, &I_RFIDDolphinSend_97x61);
            view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewPopup);
            notification_message(app->notifications, &sequence_blink_start_red);
            hitags_writer_worker_start(app, HitagSWorkerWipeTag);
            consumed = true;

        } else if(event.event == HitagSEventPopupClosed) {
            /* Success timeout — return to start */
            const uint32_t prev[] = {HitagSSceneStart};
            scene_manager_search_and_switch_to_previous_scene_one_of(
                app->scene_manager, prev, COUNT_OF(prev));
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        if(state == WipeTagStateWiping) {
            hitags_writer_worker_stop(app);
        }
        scene_manager_previous_scene(app->scene_manager);
        consumed = true;
    }

    return consumed;
}

void hitags_writer_scene_wipe_tag_on_exit(void* context) {
    HitagSApp* app = context;
    hitags_writer_worker_stop(app);
    notification_message(app->notifications, &sequence_blink_stop);
    popup_reset(app->popup);
    dialog_ex_reset(app->dialog_ex);
    widget_reset(app->widget);
}
