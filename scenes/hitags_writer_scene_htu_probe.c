/**
 * @file hitags_writer_scene_htu_probe.c
 * @brief Dedicated Hitag µ / 8265 READ UID probe scene.
 */

#include "../hitags_writer_i.h"

typedef enum {
    HtuProbeStateScanning,
    HtuProbeStateResult,
} HtuProbeState;

static void hitags_writer_scene_htu_probe_show_scanning(HitagSApp* app) {
    Popup* popup = app->popup;
    popup_reset(popup);
    popup_set_header(popup, "HTU Probe", 89, 30, AlignCenter, AlignTop);
    popup_set_text(popup, "READ UID\nPlace tag", 89, 43, AlignCenter, AlignTop);
    popup_set_icon(popup, 0, 3, &I_NFC_manual_60x50);
    view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewPopup);
}

static void hitags_writer_scene_htu_probe_show_result(HitagSApp* app, bool ok) {
    Widget* widget = app->widget;
    widget_reset(widget);

    if(ok) {
        widget_add_icon_element(widget, 0, 9, &I_DolphinSuccess_91x55);
        widget_add_string_element(widget, 97, 2, AlignCenter, AlignTop, FontPrimary, "HTU Found");
        snprintf(
            app->text_store,
            sizeof(app->text_store),
            "%02X%02X%02X%02X%02X%02X\n%s %db",
            app->htu_probe.uid[0],
            app->htu_probe.uid[1],
            app->htu_probe.uid[2],
            app->htu_probe.uid[3],
            app->htu_probe.uid[4],
            app->htu_probe.uid[5],
            app->htu_probe.method ? app->htu_probe.method : "?",
            (int)app->htu_probe.response_bits);
        widget_add_string_multiline_element(
            widget, 97, 17, AlignCenter, AlignTop, FontSecondary, app->text_store);
        widget_add_button_element(
            widget, GuiButtonTypeLeft, "Back", hitags_writer_widget_callback, app);
        widget_add_button_element(
            widget, GuiButtonTypeRight, "Retry", hitags_writer_widget_callback, app);
    } else {
        widget_add_icon_element(widget, 83, 22, &I_WarningDolphinFlip_45x42);
        widget_add_string_element(widget, 40, 5, AlignCenter, AlignTop, FontPrimary, "No HTU UID");
        if(app->htu_probe.had_activity) {
            snprintf(
                app->text_store,
                sizeof(app->text_store),
                "%s %db\n%d candidates",
                app->htu_probe.method ? app->htu_probe.method : "noise",
                (int)app->htu_probe.response_bits,
                (int)app->htu_probe.candidates_tried);
        } else {
            snprintf(app->text_store, sizeof(app->text_store), "No response\nTry again.");
        }
        widget_add_string_multiline_element(
            widget, 40, 22, AlignCenter, AlignTop, FontSecondary, app->text_store);
        widget_add_button_element(
            widget, GuiButtonTypeLeft, "Back", hitags_writer_widget_callback, app);
        widget_add_button_element(
            widget, GuiButtonTypeRight, "Retry", hitags_writer_widget_callback, app);
    }

    scene_manager_set_scene_state(app->scene_manager, HitagSSceneHtuProbe, HtuProbeStateResult);
    view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewWidget);
}

void hitags_writer_scene_htu_probe_on_enter(void* context) {
    HitagSApp* app = context;

    hitags_writer_scene_htu_probe_show_scanning(app);
    notification_message(app->notifications, &sequence_blink_start_cyan);
    scene_manager_set_scene_state(app->scene_manager, HitagSSceneHtuProbe, HtuProbeStateScanning);
    hitags_writer_worker_start(app, HitagSWorkerHtuProbe);
}

bool hitags_writer_scene_htu_probe_on_event(void* context, SceneManagerEvent event) {
    HitagSApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == HitagSEventHtuProbeOk) {
            hitags_writer_worker_stop(app);
            notification_message(app->notifications, &sequence_blink_stop);
            notification_message(app->notifications, &sequence_success);
            hitags_writer_scene_htu_probe_show_result(app, true);
            consumed = true;
        } else if(event.event == HitagSEventHtuProbeFailed) {
            hitags_writer_worker_stop(app);
            notification_message(app->notifications, &sequence_blink_stop);
            notification_message(app->notifications, &sequence_error);
            hitags_writer_scene_htu_probe_show_result(app, false);
            consumed = true;
        } else if(event.event == GuiButtonTypeLeft) {
            scene_manager_previous_scene(app->scene_manager);
            consumed = true;
        } else if(event.event == GuiButtonTypeRight) {
            hitags_writer_scene_htu_probe_show_scanning(app);
            notification_message(app->notifications, &sequence_blink_start_cyan);
            scene_manager_set_scene_state(
                app->scene_manager, HitagSSceneHtuProbe, HtuProbeStateScanning);
            hitags_writer_worker_start(app, HitagSWorkerHtuProbe);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        hitags_writer_worker_stop(app);
        scene_manager_previous_scene(app->scene_manager);
        consumed = true;
    }

    return consumed;
}

void hitags_writer_scene_htu_probe_on_exit(void* context) {
    HitagSApp* app = context;
    hitags_writer_worker_stop(app);
    notification_message(app->notifications, &sequence_blink_stop);
    popup_reset(app->popup);
    widget_reset(app->widget);
}
