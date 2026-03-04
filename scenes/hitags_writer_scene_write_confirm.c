/**
 * @file hitags_writer_scene_write_confirm.c
 * @brief Write confirmation scene — shows ID and asks for confirmation
 */

#include "../hitags_writer_i.h"

static void hitags_writer_scene_write_confirm_callback(
    DialogExResult result,
    void* context) {
    HitagSApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, result);
}

void hitags_writer_scene_write_confirm_on_enter(void* context) {
    HitagSApp* app = context;
    DialogEx* dialog_ex = app->dialog_ex;

    char id_str[16];
    em4100_id_to_string(app->em4100_id, id_str);
    snprintf(
        app->text_store,
        sizeof(app->text_store),
        "Write EM4100\n%s\nto 8268 tag?",
        id_str);

    dialog_ex_set_header(dialog_ex, "Confirm Write", 64, 0, AlignCenter, AlignTop);
    dialog_ex_set_text(dialog_ex, app->text_store, 64, 16, AlignCenter, AlignTop);
    dialog_ex_set_icon(dialog_ex, 0, 12, &I_NFC_manual_60x50);
    dialog_ex_set_left_button_text(dialog_ex, "Cancel");
    dialog_ex_set_right_button_text(dialog_ex, "Write");
    dialog_ex_set_result_callback(dialog_ex, hitags_writer_scene_write_confirm_callback);
    dialog_ex_set_context(dialog_ex, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewDialogEx);
}

bool hitags_writer_scene_write_confirm_on_event(void* context, SceneManagerEvent event) {
    HitagSApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == DialogExResultRight) {
            /* Confirmed — proceed to write */
            scene_manager_next_scene(app->scene_manager, HitagSSceneWrite);
            consumed = true;
        } else if(event.event == DialogExResultLeft) {
            /* Cancelled — go back */
            scene_manager_previous_scene(app->scene_manager);
            consumed = true;
        }
    }

    return consumed;
}

void hitags_writer_scene_write_confirm_on_exit(void* context) {
    HitagSApp* app = context;
    dialog_ex_reset(app->dialog_ex);
}
