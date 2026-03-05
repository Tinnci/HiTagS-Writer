/**
 * @file hitags_writer_scene_write_fail.c
 * @brief Write failure scene — shows error details
 */

#include "../hitags_writer_i.h"

void hitags_writer_scene_write_fail_on_enter(void* context) {
    HitagSApp* app = context;
    Widget* widget = app->widget;

    widget_add_icon_element(widget, 83, 22, &I_WarningDolphinFlip_45x42);

    widget_add_string_element(widget, 40, 5, AlignCenter, AlignTop, FontPrimary, "Write Failed!");

    const char* error_msg;
    switch(app->last_result) {
    case HitagSResultTimeout:
        error_msg = "No tag detected.\nPlace 8268 tag on\nFlipper's back.";
        break;
    case HitagSResultNack:
        error_msg = "Tag rejected\ncommand. Wrong pwd\nor not a 8268 chip?";
        break;
    case HitagSResultCrcError:
        error_msg = "CRC error in\ncommunication.";
        break;
    default:
        error_msg = "Unknown error.\nTry again.";
        break;
    }

    widget_add_string_multiline_element(
        widget, 40, 22, AlignCenter, AlignTop, FontSecondary, error_msg);

    widget_add_button_element(
        widget, GuiButtonTypeLeft, "Back", hitags_writer_widget_callback, app);

    widget_add_button_element(
        widget, GuiButtonTypeRight, "Retry", hitags_writer_widget_callback, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewWidget);
    notification_message(app->notifications, &sequence_set_red_255);
}

bool hitags_writer_scene_write_fail_on_event(void* context, SceneManagerEvent event) {
    HitagSApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == GuiButtonTypeLeft) {
            /* Back to start */
            const uint32_t prev_scenes[] = {HitagSSceneStart};
            scene_manager_search_and_switch_to_previous_scene_one_of(
                app->scene_manager, prev_scenes, COUNT_OF(prev_scenes));
            consumed = true;
        } else if(event.event == GuiButtonTypeRight) {
            /* Retry: go back to write scene */
            scene_manager_previous_scene(app->scene_manager);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        const uint32_t prev_scenes[] = {HitagSSceneStart};
        scene_manager_search_and_switch_to_previous_scene_one_of(
            app->scene_manager, prev_scenes, COUNT_OF(prev_scenes));
        consumed = true;
    }

    return consumed;
}

void hitags_writer_scene_write_fail_on_exit(void* context) {
    HitagSApp* app = context;
    notification_message_block(app->notifications, &sequence_reset_red);
    widget_reset(app->widget);
}
