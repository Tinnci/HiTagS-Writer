/**
 * @file hitags_writer_scene_write_success.c
 * @brief Write success scene — shows success popup
 */

#include "../hitags_writer_i.h"

void hitags_writer_scene_write_success_on_enter(void* context) {
    HitagSApp* app = context;
    Popup* popup = app->popup;

    popup_set_header(popup, "Success!", 64, 10, AlignCenter, AlignTop);

    char id_str[16];
    em4100_id_to_string(app->em4100_id, id_str);
    snprintf(app->text_store, sizeof(app->text_store), "EM4100 %s\nwritten to 8268", id_str);
    popup_set_text(popup, app->text_store, 64, 30, AlignCenter, AlignTop);

    popup_set_context(popup, app);
    popup_set_callback(popup, hitags_writer_popup_timeout_callback);
    popup_set_timeout(popup, 2000);
    popup_enable_timeout(popup);

    view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewPopup);
    notification_message_block(app->notifications, &sequence_set_green_255);
}

bool hitags_writer_scene_write_success_on_event(void* context, SceneManagerEvent event) {
    HitagSApp* app = context;
    bool consumed = false;

    const uint32_t prev_scenes[] = {HitagSSceneStart};

    if((event.type == SceneManagerEventTypeBack) ||
       ((event.type == SceneManagerEventTypeCustom) &&
        (event.event == HitagSEventPopupClosed))) {
        scene_manager_search_and_switch_to_previous_scene_one_of(
            app->scene_manager, prev_scenes, COUNT_OF(prev_scenes));
        consumed = true;
    }

    return consumed;
}

void hitags_writer_scene_write_success_on_exit(void* context) {
    HitagSApp* app = context;
    notification_message_block(app->notifications, &sequence_reset_green);
    popup_reset(app->popup);
}
