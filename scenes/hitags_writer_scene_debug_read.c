/**
 * @file hitags_writer_scene_debug_read.c
 * @brief Debug Read scene — full read with RF trace capture and save
 *
 * Performs a full debug read (UID + SELECT + Auth + Read all pages) while
 * capturing detailed RF edge timing data. The trace is saved to a .htsd
 * file for offline analysis with analyze_trace.py.
 */

#include "../hitags_writer_i.h"

#define HITAGS_TRACE_FOLDER    EXT_PATH("lfrfid")
#define HITAGS_TRACE_EXTENSION ".htsd"

typedef enum {
    DebugReadStateScanning,
    DebugReadStateSuccess,
    DebugReadStateFailed,
} DebugReadState;

static void hitags_writer_scene_debug_read_save_trace(HitagSApp* app) {
    if(!app->debug_trace) {
        notification_message(app->notifications, &sequence_error);
        return;
    }

    /* Generate filename from UID */
    FuriString* filename = furi_string_alloc();
    furi_string_printf(
        filename,
        "%s/Trace_%08lX%s",
        HITAGS_TRACE_FOLDER,
        (unsigned long)app->tag_uid,
        HITAGS_TRACE_EXTENSION);

    /* Ensure directory exists */
    storage_simply_mkdir(app->storage, HITAGS_TRACE_FOLDER);

    bool saved =
        hitag_s_debug_trace_save(app->storage, furi_string_get_cstr(filename), app->debug_trace);

    if(saved) {
        notification_message(app->notifications, &sequence_success);

        /* Show save success popup */
        Popup* popup = app->popup;
        popup_reset(popup);
        popup_set_header(popup, "Saved!", 97, 12, AlignCenter, AlignTop);
        popup_set_icon(popup, 0, 9, &I_DolphinSuccess_91x55);
        snprintf(
            app->text_store, sizeof(app->text_store), "Trace_%08lX", (unsigned long)app->tag_uid);
        popup_set_text(popup, app->text_store, 97, 25, AlignCenter, AlignTop);
        popup_set_context(popup, app);
        popup_set_callback(popup, hitags_writer_popup_timeout_callback);
        popup_set_timeout(popup, 2000);
        popup_enable_timeout(popup);
        view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewPopup);
    } else {
        notification_message(app->notifications, &sequence_error);
        dialog_message_show_storage_error(app->dialogs, "Save failed!");
    }

    furi_string_free(filename);
}

void hitags_writer_scene_debug_read_on_enter(void* context) {
    HitagSApp* app = context;
    Popup* popup = app->popup;

    popup_set_header(popup, "Debug Read", 89, 30, AlignCenter, AlignTop);
    popup_set_text(popup, "Reading with\ntrace capture", 89, 43, AlignCenter, AlignTop);
    popup_set_icon(popup, 0, 3, &I_NFC_manual_60x50);

    view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewPopup);
    notification_message(app->notifications, &sequence_blink_start_magenta);

    scene_manager_set_scene_state(
        app->scene_manager, HitagSSceneDebugRead, DebugReadStateScanning);
    hitags_writer_worker_start(app, HitagSWorkerDebugRead);
}

bool hitags_writer_scene_debug_read_on_event(void* context, SceneManagerEvent event) {
    HitagSApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        DebugReadState state =
            scene_manager_get_scene_state(app->scene_manager, HitagSSceneDebugRead);

        if(event.event == HitagSEventDebugOk) {
            hitags_writer_worker_stop(app);
            notification_message(app->notifications, &sequence_blink_stop);
            notification_message(app->notifications, &sequence_success);

            /* Build result widget */
            Widget* widget = app->widget;
            widget_reset(widget);

            widget_add_string_element(
                widget, 64, 0, AlignCenter, AlignTop, FontPrimary, "Debug Trace");

            snprintf(
                app->text_store,
                sizeof(app->text_store),
                "UID:%08lX\n%d/%d pgs read\nTrace ready",
                (unsigned long)app->tag_uid,
                app->dump_read_count,
                app->dump_max_page + 1);
            widget_add_string_multiline_element(
                widget, 64, 13, AlignCenter, AlignTop, FontSecondary, app->text_store);

            widget_add_button_element(
                widget, GuiButtonTypeLeft, "Back", hitags_writer_widget_callback, app);
            widget_add_button_element(
                widget, GuiButtonTypeCenter, "Save", hitags_writer_widget_callback, app);

            scene_manager_set_scene_state(
                app->scene_manager, HitagSSceneDebugRead, DebugReadStateSuccess);
            view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewWidget);
            consumed = true;

        } else if(event.event == HitagSEventDebugFailed) {
            hitags_writer_worker_stop(app);
            notification_message(app->notifications, &sequence_blink_stop);
            notification_message(app->notifications, &sequence_error);

            /* Even on failure, trace has useful data — offer to save */
            Widget* widget = app->widget;
            widget_reset(widget);

            widget_add_icon_element(widget, 83, 22, &I_WarningDolphinFlip_45x42);
            widget_add_string_element(
                widget, 40, 5, AlignCenter, AlignTop, FontPrimary, "Read Failed!");

            const char* errmsg;
            switch(app->last_result) {
            case HitagSResultTimeout:
                errmsg = "No tag found.\nPlace tag on\nFlipper back.";
                break;
            case HitagSResultNack:
                errmsg = "Auth rejected.\nTrace saved\nfor analysis.";
                break;
            default:
                errmsg = "Read error.\nTrace saved.";
                break;
            }

            widget_add_string_multiline_element(
                widget, 40, 22, AlignCenter, AlignTop, FontSecondary, errmsg);
            widget_add_button_element(
                widget, GuiButtonTypeLeft, "Back", hitags_writer_widget_callback, app);
            widget_add_button_element(
                widget, GuiButtonTypeCenter, "Save", hitags_writer_widget_callback, app);
            widget_add_button_element(
                widget, GuiButtonTypeRight, "Retry", hitags_writer_widget_callback, app);

            scene_manager_set_scene_state(
                app->scene_manager, HitagSSceneDebugRead, DebugReadStateFailed);
            view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewWidget);
            consumed = true;

        } else if(event.event == GuiButtonTypeLeft) {
            scene_manager_previous_scene(app->scene_manager);
            consumed = true;

        } else if(event.event == GuiButtonTypeCenter) {
            /* Save trace — works in both success and failure states */
            hitags_writer_scene_debug_read_save_trace(app);
            consumed = true;

        } else if(event.event == GuiButtonTypeRight && state == DebugReadStateFailed) {
            /* Retry */
            widget_reset(app->widget);
            Popup* popup = app->popup;
            popup_reset(popup);
            popup_set_header(popup, "Debug Read", 89, 30, AlignCenter, AlignTop);
            popup_set_text(popup, "Reading with\ntrace capture", 89, 43, AlignCenter, AlignTop);
            popup_set_icon(popup, 0, 3, &I_NFC_manual_60x50);
            scene_manager_set_scene_state(
                app->scene_manager, HitagSSceneDebugRead, DebugReadStateScanning);
            view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewPopup);
            notification_message(app->notifications, &sequence_blink_start_magenta);
            hitags_writer_worker_start(app, HitagSWorkerDebugRead);
            consumed = true;

        } else if(event.event == HitagSEventPopupClosed) {
            /* Save popup timeout — return to result widget */
            view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewWidget);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        hitags_writer_worker_stop(app);
        scene_manager_previous_scene(app->scene_manager);
        consumed = true;
    }

    return consumed;
}

void hitags_writer_scene_debug_read_on_exit(void* context) {
    HitagSApp* app = context;
    hitags_writer_worker_stop(app);
    notification_message(app->notifications, &sequence_blink_stop);
    popup_reset(app->popup);
    widget_reset(app->widget);
}
