/**
 * @file hitags_writer_scene_tools_menu.c
 * @brief Tools submenu.
 */

#include "../hitags_writer_i.h"

typedef enum {
    ToolsMenuIndexWipe,
    ToolsMenuIndexDebug,
    ToolsMenuIndexAbout,
} ToolsMenuIndex;

static void hitags_writer_scene_tools_menu_callback(void* context, uint32_t index) {
    HitagSApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void hitags_writer_scene_tools_menu_on_enter(void* context) {
    HitagSApp* app = context;
    Submenu* submenu = app->submenu;

    submenu_add_item(
        submenu, "Wipe Tag", ToolsMenuIndexWipe, hitags_writer_scene_tools_menu_callback, app);
    submenu_add_item(
        submenu, "Debug Read", ToolsMenuIndexDebug, hitags_writer_scene_tools_menu_callback, app);
    submenu_add_item(
        submenu, "About", ToolsMenuIndexAbout, hitags_writer_scene_tools_menu_callback, app);

    submenu_set_selected_item(
        submenu, scene_manager_get_scene_state(app->scene_manager, HitagSSceneToolsMenu));
    view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewSubmenu);
}

bool hitags_writer_scene_tools_menu_on_event(void* context, SceneManagerEvent event) {
    HitagSApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        scene_manager_set_scene_state(app->scene_manager, HitagSSceneToolsMenu, event.event);
        if(event.event == ToolsMenuIndexWipe) {
            scene_manager_next_scene(app->scene_manager, HitagSSceneWipeTag);
            consumed = true;
        } else if(event.event == ToolsMenuIndexDebug) {
            scene_manager_next_scene(app->scene_manager, HitagSSceneDebugRead);
            consumed = true;
        } else if(event.event == ToolsMenuIndexAbout) {
            scene_manager_next_scene(app->scene_manager, HitagSSceneAbout);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        scene_manager_set_scene_state(app->scene_manager, HitagSSceneToolsMenu, 0);
    }

    return consumed;
}

void hitags_writer_scene_tools_menu_on_exit(void* context) {
    HitagSApp* app = context;
    submenu_reset(app->submenu);
}
