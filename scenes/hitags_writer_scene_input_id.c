/**
 * @file hitags_writer_scene_input_id.c
 * @brief Input ID scene — byte input for 5-byte EM4100 ID
 */

#include "../hitags_writer_i.h"

static void hitags_writer_scene_input_id_callback(void* context) {
    HitagSApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventNext);
}

void hitags_writer_scene_input_id_on_enter(void* context) {
    HitagSApp* app = context;
    ByteInput* byte_input = app->byte_input;

    byte_input_set_header_text(byte_input, "Enter EM4100 ID (5 bytes)");

    byte_input_set_result_callback(
        byte_input,
        hitags_writer_scene_input_id_callback,
        NULL, /* changed callback not needed */
        app,
        app->em4100_id,
        EM4100_ID_SIZE);

    view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewByteInput);
}

bool hitags_writer_scene_input_id_on_event(void* context, SceneManagerEvent event) {
    HitagSApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == HitagSEventNext) {
            /* ID entered, proceed to confirmation */
            scene_manager_next_scene(app->scene_manager, HitagSSceneWriteConfirm);
            consumed = true;
        }
    }

    return consumed;
}

void hitags_writer_scene_input_id_on_exit(void* context) {
    HitagSApp* app = context;
    byte_input_set_result_callback(app->byte_input, NULL, NULL, NULL, NULL, 0);
}
