/**
 * @file hitags_writer_scene_write.c
 * @brief Write scene — executes the HiTag S 8268 write sequence
 */

#include "../hitags_writer_i.h"

#define TAG "HitagSWrite"

void hitags_writer_scene_write_on_enter(void* context) {
    HitagSApp* app = context;
    Popup* popup = app->popup;

    /* Show writing UI */
    popup_set_header(popup, "Writing...", 89, 30, AlignCenter, AlignTop);

    char id_str[16];
    em4100_id_to_string(app->em4100_id, id_str);
    snprintf(app->text_store, sizeof(app->text_store), "EM4100: %s", id_str);
    popup_set_text(popup, app->text_store, 89, 43, AlignCenter, AlignTop);

    view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewPopup);
    notification_message(app->notifications, &sequence_blink_start_magenta);

    /* Prepare EM4100 data for HiTag S pages */
    Em4100HitagData hitag_data;
    em4100_prepare_hitag_data(app->em4100_id, &hitag_data);

    /* Pages to write:
     * Page 1: Config page (TTF settings for EM4100 emulation)
     * Page 4: EM4100 data upper 32 bits
     * Page 5: EM4100 data lower 32 bits
     */
    uint32_t pages[3] = {
        hitag_data.config_page,
        hitag_data.data_hi,
        hitag_data.data_lo,
    };
    uint8_t page_addrs[3] = {1, 4, 5};

    FURI_LOG_I(
        TAG,
        "Writing EM4100 ID %02X%02X%02X%02X%02X to 8268",
        app->em4100_id[0],
        app->em4100_id[1],
        app->em4100_id[2],
        app->em4100_id[3],
        app->em4100_id[4]);

    /* Execute the write sequence */
    app->last_result = hitag_s_8268_write_sequence(
        app->password, pages, page_addrs, 3);

    if(app->last_result == HitagSResultOk) {
        FURI_LOG_I(TAG, "Write successful!");
        view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventWriteOk);
    } else {
        FURI_LOG_E(TAG, "Write failed with result %d", app->last_result);
        view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventWriteFailed);
    }
}

bool hitags_writer_scene_write_on_event(void* context, SceneManagerEvent event) {
    HitagSApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == HitagSEventWriteOk) {
            notification_message(app->notifications, &sequence_success);
            scene_manager_next_scene(app->scene_manager, HitagSSceneWriteSuccess);
            consumed = true;
        } else if(event.event == HitagSEventWriteFailed) {
            notification_message(app->notifications, &sequence_error);
            scene_manager_next_scene(app->scene_manager, HitagSSceneWriteFail);
            consumed = true;
        }
    }

    return consumed;
}

void hitags_writer_scene_write_on_exit(void* context) {
    HitagSApp* app = context;
    notification_message(app->notifications, &sequence_blink_stop);
    popup_reset(app->popup);
}
