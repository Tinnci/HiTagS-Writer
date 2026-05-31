/**
 * @file hitags_writer_scene_read_menu.c
 * @brief Read submenu.
 */

#include "../hitags_writer_i.h"

typedef enum {
    ReadMenuIndexEm4100,
    ReadMenuIndexPages,
    ReadMenuIndexUid,
} ReadMenuIndex;

/* 8268 aliases used in docs/tests: "Read 8268 Pages", "Read 8268 UID". */

static void hitags_writer_scene_read_menu_callback(void* context, uint32_t index) {
    HitagSApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void hitags_writer_scene_read_menu_on_enter(void* context) {
    HitagSApp* app = context;
    Submenu* submenu = app->submenu;

    submenu_add_item(
        submenu, "Read EM4100", ReadMenuIndexEm4100, hitags_writer_scene_read_menu_callback, app);
    submenu_add_item(
        submenu, "Read Tag Data", ReadMenuIndexPages, hitags_writer_scene_read_menu_callback, app);
    submenu_add_item(
        submenu, "Read Tag UID", ReadMenuIndexUid, hitags_writer_scene_read_menu_callback, app);

    submenu_set_selected_item(
        submenu, scene_manager_get_scene_state(app->scene_manager, HitagSSceneReadMenu));
    view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewSubmenu);
}

bool hitags_writer_scene_read_menu_on_event(void* context, SceneManagerEvent event) {
    HitagSApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        scene_manager_set_scene_state(app->scene_manager, HitagSSceneReadMenu, event.event);
        if(event.event == ReadMenuIndexEm4100) {
            scene_manager_next_scene(app->scene_manager, HitagSSceneReadEm4100);
            consumed = true;
        } else if(event.event == ReadMenuIndexPages) {
            scene_manager_next_scene(app->scene_manager, HitagSSceneReadTag);
            consumed = true;
        } else if(event.event == ReadMenuIndexUid) {
            scene_manager_next_scene(app->scene_manager, HitagSSceneReadUid);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        scene_manager_set_scene_state(app->scene_manager, HitagSSceneReadMenu, 0);
    }

    return consumed;
}

void hitags_writer_scene_read_menu_on_exit(void* context) {
    HitagSApp* app = context;
    submenu_reset(app->submenu);
}
