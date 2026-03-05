/**
 * @file hitags_writer_scene_write_uid.c
 * @brief Write UID scene — input new UID and write to 8268 page 0
 *
 * 8268 magic chips allow writing page 0 (UID), which normal Hitag S
 * tags don't allow. This is the key "magic" feature for cloning.
 *
 * Flow: ByteInput (4 bytes UID) → Confirm → Write (worker) → Success/Fail
 */

#include "../hitags_writer_i.h"

typedef enum {
    WriteUidStateInput, /* ByteInput: enter UID */
    WriteUidStateConfirm, /* Confirm dialog */
    WriteUidStateWriting, /* Writing in progress */
} WriteUidState;

static void hitags_writer_scene_write_uid_byte_input_callback(void* context) {
    HitagSApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventNext);
}

static void hitags_writer_scene_write_uid_confirm_callback(DialogExResult result, void* context) {
    HitagSApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, result);
}

void hitags_writer_scene_write_uid_on_enter(void* context) {
    HitagSApp* app = context;

    /* Default: use last read UID if available, otherwise 0x00000000 */
    if(app->tag_uid != 0) {
        app->uid_input[0] = (app->tag_uid >> 24) & 0xFF;
        app->uid_input[1] = (app->tag_uid >> 16) & 0xFF;
        app->uid_input[2] = (app->tag_uid >> 8) & 0xFF;
        app->uid_input[3] = app->tag_uid & 0xFF;
    }

    /* Show ByteInput for 4 bytes */
    ByteInput* byte_input = app->byte_input;
    byte_input_set_header_text(byte_input, "Enter new UID (4 bytes)");
    byte_input_set_result_callback(
        byte_input,
        hitags_writer_scene_write_uid_byte_input_callback,
        NULL,
        app,
        app->uid_input,
        4);

    scene_manager_set_scene_state(app->scene_manager, HitagSSceneWriteUid, WriteUidStateInput);
    view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewByteInput);
}

bool hitags_writer_scene_write_uid_on_event(void* context, SceneManagerEvent event) {
    HitagSApp* app = context;
    bool consumed = false;
    WriteUidState state = scene_manager_get_scene_state(app->scene_manager, HitagSSceneWriteUid);

    if(event.type == SceneManagerEventTypeCustom) {
        if(state == WriteUidStateInput && event.event == HitagSEventNext) {
            /* Input done — show confirm dialog */
            app->target_uid = ((uint32_t)app->uid_input[0] << 24) |
                              ((uint32_t)app->uid_input[1] << 16) |
                              ((uint32_t)app->uid_input[2] << 8) | (uint32_t)app->uid_input[3];

            DialogEx* dialog = app->dialog_ex;
            dialog_ex_reset(dialog);

            snprintf(
                app->text_store,
                sizeof(app->text_store),
                "Write UID\n%08lX\nto 8268 page 0?",
                (unsigned long)app->target_uid);

            dialog_ex_set_header(dialog, "Write UID", 64, 0, AlignCenter, AlignTop);
            dialog_ex_set_text(dialog, app->text_store, 64, 16, AlignCenter, AlignTop);
            dialog_ex_set_left_button_text(dialog, "Cancel");
            dialog_ex_set_right_button_text(dialog, "Write");
            dialog_ex_set_result_callback(dialog, hitags_writer_scene_write_uid_confirm_callback);
            dialog_ex_set_context(dialog, app);

            scene_manager_set_scene_state(
                app->scene_manager, HitagSSceneWriteUid, WriteUidStateConfirm);
            view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewDialogEx);
            consumed = true;

        } else if(state == WriteUidStateConfirm && event.event == DialogExResultRight) {
            /* Confirmed — start writing */
            Popup* popup = app->popup;
            popup_reset(popup);
            popup_set_header(popup, "Writing UID...", 89, 30, AlignCenter, AlignTop);
            snprintf(
                app->text_store,
                sizeof(app->text_store),
                "UID: %08lX",
                (unsigned long)app->target_uid);
            popup_set_text(popup, app->text_store, 89, 43, AlignCenter, AlignTop);
            popup_set_icon(popup, 0, 3, &I_RFIDDolphinSend_97x61);

            scene_manager_set_scene_state(
                app->scene_manager, HitagSSceneWriteUid, WriteUidStateWriting);
            view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewPopup);
            notification_message(app->notifications, &sequence_blink_start_magenta);

            hitags_writer_worker_start(app, HitagSWorkerWriteUid);
            consumed = true;

        } else if(state == WriteUidStateConfirm && event.event == DialogExResultLeft) {
            /* Cancelled — go back to input */
            scene_manager_set_scene_state(
                app->scene_manager, HitagSSceneWriteUid, WriteUidStateInput);

            ByteInput* byte_input = app->byte_input;
            byte_input_set_header_text(byte_input, "Enter new UID (4 bytes)");
            byte_input_set_result_callback(
                byte_input,
                hitags_writer_scene_write_uid_byte_input_callback,
                NULL,
                app,
                app->uid_input,
                4);
            view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewByteInput);
            consumed = true;

        } else if(state == WriteUidStateWriting && event.event == HitagSEventWriteUidOk) {
            hitags_writer_worker_stop(app);
            notification_message(app->notifications, &sequence_blink_stop);
            notification_message(app->notifications, &sequence_success);

            /* Show success popup */
            Popup* popup = app->popup;
            popup_reset(popup);
            popup_set_header(popup, "UID Written!", 97, 12, AlignCenter, AlignTop);
            popup_set_icon(popup, 0, 9, &I_DolphinSuccess_91x55);
            snprintf(
                app->text_store,
                sizeof(app->text_store),
                "New UID:\n%08lX",
                (unsigned long)app->target_uid);
            popup_set_text(popup, app->text_store, 97, 25, AlignCenter, AlignTop);
            popup_set_context(popup, app);
            popup_set_callback(popup, hitags_writer_popup_timeout_callback);
            popup_set_timeout(popup, 2000);
            popup_enable_timeout(popup);
            view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewPopup);
            consumed = true;

        } else if(state == WriteUidStateWriting && event.event == HitagSEventWriteUidFailed) {
            hitags_writer_worker_stop(app);
            notification_message(app->notifications, &sequence_blink_stop);
            notification_message(app->notifications, &sequence_error);

            /* Show failure in widget */
            Widget* widget = app->widget;
            widget_reset(widget);
            widget_add_icon_element(widget, 83, 22, &I_WarningDolphinFlip_45x42);
            widget_add_string_element(
                widget, 40, 5, AlignCenter, AlignTop, FontPrimary, "UID Write Failed!");

            const char* errmsg;
            switch(app->last_result) {
            case HitagSResultTimeout:
                errmsg = "No tag detected.\nPlace 8268 tag on\nFlipper's back.";
                break;
            case HitagSResultNack:
                errmsg = "Auth rejected.\nWrong password or\nnot 8268 magic?";
                break;
            default:
                errmsg = "Write error.\nTry again.";
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

        } else if(state == WriteUidStateWriting && event.event == GuiButtonTypeLeft) {
            /* Back from failure widget */
            const uint32_t prev[] = {HitagSSceneStart};
            scene_manager_search_and_switch_to_previous_scene_one_of(
                app->scene_manager, prev, COUNT_OF(prev));
            consumed = true;

        } else if(state == WriteUidStateWriting && event.event == GuiButtonTypeRight) {
            /* Retry from failure widget */
            widget_reset(app->widget);
            Popup* popup = app->popup;
            popup_reset(popup);
            popup_set_header(popup, "Writing UID...", 89, 30, AlignCenter, AlignTop);
            snprintf(
                app->text_store,
                sizeof(app->text_store),
                "UID: %08lX",
                (unsigned long)app->target_uid);
            popup_set_text(popup, app->text_store, 89, 43, AlignCenter, AlignTop);
            popup_set_icon(popup, 0, 3, &I_RFIDDolphinSend_97x61);
            view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewPopup);
            notification_message(app->notifications, &sequence_blink_start_magenta);
            hitags_writer_worker_start(app, HitagSWorkerWriteUid);
            consumed = true;

        } else if(event.event == HitagSEventPopupClosed) {
            /* Success timeout — return to start */
            const uint32_t prev[] = {HitagSSceneStart};
            scene_manager_search_and_switch_to_previous_scene_one_of(
                app->scene_manager, prev, COUNT_OF(prev));
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        if(state == WriteUidStateWriting) {
            hitags_writer_worker_stop(app);
        }
        scene_manager_previous_scene(app->scene_manager);
        consumed = true;
    }

    return consumed;
}

void hitags_writer_scene_write_uid_on_exit(void* context) {
    HitagSApp* app = context;
    hitags_writer_worker_stop(app);
    notification_message(app->notifications, &sequence_blink_stop);
    popup_reset(app->popup);
    dialog_ex_reset(app->dialog_ex);
    byte_input_set_result_callback(app->byte_input, NULL, NULL, NULL, NULL, 0);
    widget_reset(app->widget);
}
