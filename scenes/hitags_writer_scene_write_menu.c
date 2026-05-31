/**
 * @file hitags_writer_scene_write_menu.c
 * @brief Write submenu.
 */

#include "../hitags_writer_i.h"

typedef enum {
    WriteMenuIndexEm4100,
    WriteMenuIndexFile,
    WriteMenuIndexUid,
} WriteMenuIndex;

static void hitags_writer_scene_write_menu_callback(void* context, uint32_t index) {
    HitagSApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void hitags_writer_scene_write_menu_on_enter(void* context) {
    HitagSApp* app = context;
    Submenu* submenu = app->submenu;

    submenu_add_item(
        submenu,
        "Write EM4100 ID",
        WriteMenuIndexEm4100,
        hitags_writer_scene_write_menu_callback,
        app);
    submenu_add_item(
        submenu,
        "Load from File",
        WriteMenuIndexFile,
        hitags_writer_scene_write_menu_callback,
        app);
    submenu_add_item(
        submenu, "Write Tag UID", WriteMenuIndexUid, hitags_writer_scene_write_menu_callback, app);

    submenu_set_selected_item(
        submenu, scene_manager_get_scene_state(app->scene_manager, HitagSSceneWriteMenu));
    view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewSubmenu);
}

bool hitags_writer_scene_write_menu_on_event(void* context, SceneManagerEvent event) {
    HitagSApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        scene_manager_set_scene_state(app->scene_manager, HitagSSceneWriteMenu, event.event);
        if(event.event == WriteMenuIndexEm4100) {
            scene_manager_next_scene(app->scene_manager, HitagSSceneInputId);
            consumed = true;
        } else if(event.event == WriteMenuIndexFile) {
            scene_manager_next_scene(app->scene_manager, HitagSSceneSelectFile);
            consumed = true;
        } else if(event.event == WriteMenuIndexUid) {
            scene_manager_next_scene(app->scene_manager, HitagSSceneWriteUid);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        scene_manager_set_scene_state(app->scene_manager, HitagSSceneWriteMenu, 0);
    }

    return consumed;
}

void hitags_writer_scene_write_menu_on_exit(void* context) {
    HitagSApp* app = context;
    submenu_reset(app->submenu);
}
