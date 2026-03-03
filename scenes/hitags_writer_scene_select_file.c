/**
 * @file hitags_writer_scene_select_file.c
 * @brief Select file scene — browse .rfid files and load EM4100 ID
 */

#include "../hitags_writer_i.h"

void hitags_writer_scene_select_file_on_enter(void* context) {
    HitagSApp* app = context;

    DialogsFileBrowserOptions browser_options;
    dialog_file_browser_set_basic_options(
        &browser_options, HITAGS_WRITER_APP_EXTENSION, NULL);
    browser_options.base_path = HITAGS_WRITER_APP_FOLDER;
    browser_options.hide_dot_files = true;

    bool result = dialog_file_browser_show(
        app->dialogs, app->file_path, app->file_path, &browser_options);

    if(result) {
        /* Try to load the file as LFRFID protocol data */
        ProtocolId protocol_id =
            lfrfid_dict_file_load(app->dict, furi_string_get_cstr(app->file_path));

        if(protocol_id != PROTOCOL_NO && protocol_id == LFRFIDProtocolEM4100) {
            /* Get EM4100 data (5 bytes) */
            size_t data_size = protocol_dict_get_data_size(app->dict, protocol_id);
            if(data_size >= EM4100_ID_SIZE) {
                uint8_t data[8] = {0};
                protocol_dict_get_data(app->dict, protocol_id, data, data_size);
                memcpy(app->em4100_id, data, EM4100_ID_SIZE);

                FURI_LOG_I(
                    "HitagSWriter",
                    "Loaded EM4100 ID: %02X%02X%02X%02X%02X",
                    app->em4100_id[0],
                    app->em4100_id[1],
                    app->em4100_id[2],
                    app->em4100_id[3],
                    app->em4100_id[4]);

                /* Proceed to write */
                scene_manager_next_scene(app->scene_manager, HitagSSceneWrite);
                return;
            }
        }

        /* If we get here, the file didn't contain valid EM4100 data */
        dialog_message_show_storage_error(app->dialogs, "Not a valid\nEM4100 file!");
    }

    /* Go back on cancel or error */
    scene_manager_previous_scene(app->scene_manager);
}

bool hitags_writer_scene_select_file_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void hitags_writer_scene_select_file_on_exit(void* context) {
    UNUSED(context);
}
