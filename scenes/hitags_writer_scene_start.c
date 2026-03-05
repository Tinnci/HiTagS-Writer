/**
 * @file hitags_writer_scene_start.c
 * @brief Start scene — main menu
 */

#include "../hitags_writer_i.h"

typedef enum {
    SubmenuIndexWriteEM4100,
    SubmenuIndexLoadFile,
    SubmenuIndexReadTag,
    SubmenuIndexReadUID,
    SubmenuIndexWriteUID,
    SubmenuIndexFullDump,
    SubmenuIndexLoadDump,
    SubmenuIndexAbout,
} SubmenuIndex;

static void hitags_writer_scene_start_submenu_callback(void* context, uint32_t index) {
    HitagSApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void hitags_writer_scene_start_on_enter(void* context) {
    HitagSApp* app = context;
    Submenu* submenu = app->submenu;

    submenu_add_item(
        submenu,
        "Write EM4100 ID",
        SubmenuIndexWriteEM4100,
        hitags_writer_scene_start_submenu_callback,
        app);

    submenu_add_item(
        submenu,
        "Load from File",
        SubmenuIndexLoadFile,
        hitags_writer_scene_start_submenu_callback,
        app);

    submenu_add_item(
        submenu,
        "Read Tag Data",
        SubmenuIndexReadTag,
        hitags_writer_scene_start_submenu_callback,
        app);

    submenu_add_item(
        submenu,
        "Read Tag UID",
        SubmenuIndexReadUID,
        hitags_writer_scene_start_submenu_callback,
        app);

    submenu_add_item(
        submenu,
        "Write Tag UID",
        SubmenuIndexWriteUID,
        hitags_writer_scene_start_submenu_callback,
        app);

    submenu_add_item(
        submenu,
        "Full Tag Dump",
        SubmenuIndexFullDump,
        hitags_writer_scene_start_submenu_callback,
        app);

    submenu_add_item(
        submenu,
        "Load & Clone Dump",
        SubmenuIndexLoadDump,
        hitags_writer_scene_start_submenu_callback,
        app);

    submenu_add_item(
        submenu,
        "About",
        SubmenuIndexAbout,
        hitags_writer_scene_start_submenu_callback,
        app);

    submenu_set_selected_item(
        submenu,
        scene_manager_get_scene_state(app->scene_manager, HitagSSceneStart));

    view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewSubmenu);
}

bool hitags_writer_scene_start_on_event(void* context, SceneManagerEvent event) {
    HitagSApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        scene_manager_set_scene_state(app->scene_manager, HitagSSceneStart, event.event);

        switch(event.event) {
        case SubmenuIndexWriteEM4100:
            scene_manager_next_scene(app->scene_manager, HitagSSceneInputId);
            consumed = true;
            break;
        case SubmenuIndexLoadFile:
            scene_manager_next_scene(app->scene_manager, HitagSSceneSelectFile);
            consumed = true;
            break;
        case SubmenuIndexReadTag:
            scene_manager_next_scene(app->scene_manager, HitagSSceneReadTag);
            consumed = true;
            break;
        case SubmenuIndexReadUID:
            scene_manager_next_scene(app->scene_manager, HitagSSceneReadUid);
            consumed = true;
            break;
        case SubmenuIndexWriteUID:
            scene_manager_next_scene(app->scene_manager, HitagSSceneWriteUid);
            consumed = true;
            break;
        case SubmenuIndexFullDump:
            scene_manager_next_scene(app->scene_manager, HitagSSceneFullDump);
            consumed = true;
            break;
        case SubmenuIndexLoadDump:
            scene_manager_next_scene(app->scene_manager, HitagSSceneLoadDump);
            consumed = true;
            break;
        case SubmenuIndexAbout:
            scene_manager_next_scene(app->scene_manager, HitagSSceneAbout);
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
