/**
 * @file hitags_writer_scene_write.c
 * @brief Write scene — executes the HiTag S 8268 write sequence via worker thread
 */

#include "../hitags_writer_i.h"

void hitags_writer_scene_write_on_enter(void* context) {
    HitagSApp* app = context;
    Popup* popup = app->popup;

    /* Show non-blocking "Writing" popup with dolphin icon */
    popup_set_header(popup, "Writing...", 89, 30, AlignCenter, AlignTop);
    popup_set_icon(popup, 0, 3, &I_RFIDDolphinSend_97x61);

    char id_str[16];
    em4100_id_to_string(app->em4100_id, id_str);
    snprintf(app->text_store, sizeof(app->text_store), "EM4100: %s", id_str);
    popup_set_text(popup, app->text_store, 89, 43, AlignCenter, AlignTop);

    view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewPopup);
    notification_message(app->notifications, &sequence_blink_start_magenta);

    /* Start write in worker thread — non-blocking! */
    hitags_writer_worker_start(app, HitagSWorkerWrite);
}

bool hitags_writer_scene_write_on_event(void* context, SceneManagerEvent event) {
    HitagSApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == HitagSEventWriteOk) {
            hitags_writer_worker_stop(app);
            notification_message(app->notifications, &sequence_success);
            scene_manager_next_scene(app->scene_manager, HitagSSceneWriteSuccess);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        /* User pressed Back — stop scanning and go back */
        hitags_writer_worker_stop(app);
        scene_manager_previous_scene(app->scene_manager);
        consumed = true;
    }

    return consumed;
}

void hitags_writer_scene_write_on_exit(void* context) {
    HitagSApp* app = context;
    hitags_writer_worker_stop(app);
    notification_message(app->notifications, &sequence_blink_stop);
    popup_reset(app->popup);
}
