/**
 * @file hitags_writer_scene_dump_menu.c
 * @brief Dump submenu.
 */

#include "../hitags_writer_i.h"

typedef enum {
    DumpMenuIndexFull,
    DumpMenuIndexClone,
} DumpMenuIndex;

static void hitags_writer_scene_dump_menu_callback(void* context, uint32_t index) {
    HitagSApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void hitags_writer_scene_dump_menu_on_enter(void* context) {
    HitagSApp* app = context;
    Submenu* submenu = app->submenu;

    submenu_add_item(
        submenu, "Full Tag Dump", DumpMenuIndexFull, hitags_writer_scene_dump_menu_callback, app);
    submenu_add_item(
        submenu,
        "Load & Clone Dump",
        DumpMenuIndexClone,
        hitags_writer_scene_dump_menu_callback,
        app);

    submenu_set_selected_item(
        submenu, scene_manager_get_scene_state(app->scene_manager, HitagSSceneDumpMenu));
    view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewSubmenu);
}

bool hitags_writer_scene_dump_menu_on_event(void* context, SceneManagerEvent event) {
    HitagSApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        scene_manager_set_scene_state(app->scene_manager, HitagSSceneDumpMenu, event.event);
        if(event.event == DumpMenuIndexFull) {
            scene_manager_next_scene(app->scene_manager, HitagSSceneFullDump);
            consumed = true;
        } else if(event.event == DumpMenuIndexClone) {
            scene_manager_next_scene(app->scene_manager, HitagSSceneLoadDump);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        scene_manager_set_scene_state(app->scene_manager, HitagSSceneDumpMenu, 0);
    }

    return consumed;
}

void hitags_writer_scene_dump_menu_on_exit(void* context) {
    HitagSApp* app = context;
    submenu_reset(app->submenu);
}
