/**
 * @file hitags_writer_scene_about.c
 * @brief About scene — app info and usage instructions
 */

#include "../hitags_writer_i.h"

void hitags_writer_scene_about_on_enter(void* context) {
    HitagSApp* app = context;
    Widget* widget = app->widget;

    widget_add_string_element(
        widget, 64, 2, AlignCenter, AlignTop, FontPrimary, "HiTagS Writer v0.1");

    widget_add_string_multiline_element(
        widget,
        0,
        16,
        AlignLeft,
        AlignTop,
        FontSecondary,
        "Write EM4100 card data to\n"
        "HiTag S 8268 magic chips.\n"
        "\n"
        "Supported chips:\n"
        "8268/F8268/F8278/K8678\n"
        "Default PWD: BBDD3399");

    view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewWidget);
}

bool hitags_writer_scene_about_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void hitags_writer_scene_about_on_exit(void* context) {
    HitagSApp* app = context;
    widget_reset(app->widget);
}
