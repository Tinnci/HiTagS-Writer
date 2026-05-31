/**
 * @file hitags_writer_scene_start.c
 * @brief Start scene — main menu
 */

#include "../hitags_writer_i.h"

typedef enum {
    SubmenuIndexRead,
    SubmenuIndexWrite,
    SubmenuIndexDump,
    SubmenuIndexTools,
} SubmenuIndex;

static void hitags_writer_scene_start_submenu_callback(void* context, uint32_t index) {
    HitagSApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void hitags_writer_scene_start_on_enter(void* context) {
    HitagSApp* app = context;
    Submenu* submenu = app->submenu;

    submenu_add_item(
        submenu, "Read", SubmenuIndexRead, hitags_writer_scene_start_submenu_callback, app);
    submenu_add_item(
        submenu, "Write", SubmenuIndexWrite, hitags_writer_scene_start_submenu_callback, app);
    submenu_add_item(
        submenu, "Dump", SubmenuIndexDump, hitags_writer_scene_start_submenu_callback, app);
    submenu_add_item(
        submenu, "Tools", SubmenuIndexTools, hitags_writer_scene_start_submenu_callback, app);

    submenu_set_selected_item(
        submenu, scene_manager_get_scene_state(app->scene_manager, HitagSSceneStart));

    view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewSubmenu);
}

bool hitags_writer_scene_start_on_event(void* context, SceneManagerEvent event) {
    HitagSApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        scene_manager_set_scene_state(app->scene_manager, HitagSSceneStart, event.event);

        switch(event.event) {
        case SubmenuIndexRead:
            scene_manager_next_scene(app->scene_manager, HitagSSceneReadMenu);
            consumed = true;
            break;
        case SubmenuIndexWrite:
            scene_manager_next_scene(app->scene_manager, HitagSSceneWriteMenu);
            consumed = true;
            break;
        case SubmenuIndexDump:
            scene_manager_next_scene(app->scene_manager, HitagSSceneDumpMenu);
            consumed = true;
            break;
        case SubmenuIndexTools:
            scene_manager_next_scene(app->scene_manager, HitagSSceneToolsMenu);
            consumed = true;
            break;
        default:
            break;
        }
    }

    return consumed;
}

void hitags_writer_scene_start_on_exit(void* context) {
    HitagSApp* app = context;
    submenu_reset(app->submenu);
}
