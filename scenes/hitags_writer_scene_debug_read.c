/**
 * @file hitags_writer_scene_debug_read.c
 * @brief Debug Read scene — full read with RF trace capture and save
 *
 * Performs a full debug read (UID + SELECT + Auth + Read all pages) while
 * capturing detailed RF edge timing data. The trace is saved to a .htsd
 * file for offline analysis with analyze_trace.py.
 */

#include "../hitags_writer_i.h"
#include <string.h>

#define HITAGS_TRACE_FOLDER    EXT_PATH("lfrfid")
#define HITAGS_TRACE_EXTENSION ".htsd"

typedef enum {
    DebugReadStateConfirm,
    DebugReadStateRiskConfirm,
    DebugReadStateScanning,
    DebugReadStateSuccess,
    DebugReadStatePartial,
    DebugReadStateFailed,
} DebugReadState;

static void hitags_writer_scene_debug_read_confirm_callback(DialogExResult result, void* context) {
    HitagSApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, result);
}

static HitagSWorkerOp hitags_writer_scene_debug_read_worker_op(HitagSApp* app) {
    switch(app->debug_tool) {
    case HitagSDebugToolTtfTiming:
        return HitagSWorkerTtfTiming;
    case HitagSDebugToolDisturb:
        return HitagSWorkerDisturbTest;
    case HitagSDebugToolLateDisturb:
        return HitagSWorkerLateDisturbTest;
    case HitagSDebugToolLateCommand:
        return HitagSWorkerLateCommandTest;
    case HitagSDebugToolT5577Detect:
        return HitagSWorkerT5577Detect;
    case HitagSDebugToolEm4x05Detect:
        return HitagSWorkerEm4x05Detect;
    case HitagSDebugToolWriteStimulusVerify:
        return HitagSWorkerWriteStimulusVerify;
    case HitagSDebugToolRead:
    default:
        return HitagSWorkerDebugRead;
    }
}

static const char* hitags_writer_scene_debug_read_title(HitagSApp* app) {
    switch(app->debug_tool) {
    case HitagSDebugToolTtfTiming:
        return "TTF Timing";
    case HitagSDebugToolDisturb:
        return "Disturb Test";
    case HitagSDebugToolLateDisturb:
        return "Late Disturb";
    case HitagSDebugToolLateCommand:
        return "Late Command";
    case HitagSDebugToolT5577Detect:
        return "T5577 Detect";
    case HitagSDebugToolEm4x05Detect:
        return "EM4x05 Detect";
    case HitagSDebugToolWriteStimulusVerify:
        /* Former label: "Write Stimulus". */
        return "Write Matrix";
    case HitagSDebugToolRead:
    default:
        return "Debug Read";
    }
}

static const char* hitags_writer_scene_debug_read_scanning_text(HitagSApp* app) {
    switch(app->debug_tool) {
    case HitagSDebugToolTtfTiming:
        return "Measuring TTF\nfirst edges";
    case HitagSDebugToolDisturb:
        return "Sweeping early\npause patterns";
    case HitagSDebugToolLateDisturb:
        return "Sweeping late\npause patterns";
    case HitagSDebugToolLateCommand:
        return "Sweeping late\nUID commands";
    case HitagSDebugToolT5577Detect:
        return "Reading T5577\nblock 0";
    case HitagSDebugToolEm4x05Detect:
        return "Probing EM4x05\ncommands";
    case HitagSDebugToolWriteStimulusVerify:
        return "Write matrix\nthen restore";
    case HitagSDebugToolRead:
    default:
        return "Reading with\ntrace capture";
    }
}

static const char* hitags_writer_scene_debug_read_status_text(HitagSApp* app) {
    if(app->debug_stage && strcmp(app->debug_stage, "NOISE") == 0) {
        return "Noise/TTF Only\nTrace ready";
    }
    if(app->debug_stage && strcmp(app->debug_stage, "TTF_TIMING") == 0) {
        return "TTF timing\nTrace ready";
    }
    if(app->debug_stage && strcmp(app->debug_stage, "DISTURB") == 0) {
        return "Disturb matrix\nTrace ready";
    }
    if(app->debug_stage && strcmp(app->debug_stage, "LATE_DISTURB") == 0) {
        return "Late disturb\nTrace ready";
    }
    if(app->debug_stage && strcmp(app->debug_stage, "LATE_COMMAND") == 0) {
        return "Late command\nTrace ready";
    }
    if(app->debug_stage && strcmp(app->debug_stage, "T5577_DETECT") == 0) {
        return "T5577 detect\nTrace ready";
    }
    if(app->debug_stage && strcmp(app->debug_stage, "EM4X05_DETECT") == 0) {
        return "EM4x05 detect\nTrace ready";
    }
    if(app->debug_stage && strcmp(app->debug_stage, "WRITE_MATRIX") == 0) {
        return "Write matrix\nTrace ready";
    }
    if(app->debug_stage && strcmp(app->debug_stage, "WRITE_STIM") == 0) {
        return "Write stimulus\nTrace ready";
    }
    if(app->debug_stage && strcmp(app->debug_stage, "WRITE_STIM_RESTORE_FAILED") == 0) {
        return "Restore failed\nTrace ready";
    }

    if(app->tag_uid == 0) {
        return "No UID yet\nTrace ready";
    }

    switch(app->last_result) {
    case HitagSResultTimeout:
        snprintf(
            app->text_store,
            sizeof(app->text_store),
            "Mode:%s\nStage:%s timeout",
            hitag_s_mode_name(app->debug_mode),
            app->debug_stage);
        return app->text_store;
    case HitagSResultNack:
        snprintf(
            app->text_store,
            sizeof(app->text_store),
            "Mode:%s\nStage:%s NACK",
            hitag_s_mode_name(app->debug_mode),
            app->debug_stage);
        return app->text_store;
    case HitagSResultCrcError:
        snprintf(
            app->text_store,
            sizeof(app->text_store),
            "Mode:%s\nStage:%s CRC",
            hitag_s_mode_name(app->debug_mode),
            app->debug_stage);
        return app->text_store;
    default:
        snprintf(
            app->text_store,
            sizeof(app->text_store),
            "Mode:%s\nStage:%s error",
            hitag_s_mode_name(app->debug_mode),
            app->debug_stage);
        return app->text_store;
    }
}

static const char* hitags_writer_scene_debug_read_failed_text(HitagSApp* app) {
    if(app->debug_stage && strcmp(app->debug_stage, "NOISE") == 0) {
        return "Noise/TTF Only\nTrace failed.";
    }
    return "No valid UID\nSave disabled.";
}

static void hitags_writer_scene_debug_read_start_worker(HitagSApp* app) {
    Popup* popup = app->popup;
    popup_reset(popup);
    popup_set_header(
        popup, hitags_writer_scene_debug_read_title(app), 89, 30, AlignCenter, AlignTop);
    popup_set_text(
        popup, hitags_writer_scene_debug_read_scanning_text(app), 89, 43, AlignCenter, AlignTop);
    popup_set_icon(popup, 0, 3, &I_NFC_manual_60x50);

    view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewPopup);
    notification_message(app->notifications, &sequence_blink_start_magenta);

    scene_manager_set_scene_state(
        app->scene_manager, HitagSSceneDebugRead, DebugReadStateScanning);
    hitags_writer_worker_start(app, hitags_writer_scene_debug_read_worker_op(app));
}

static void hitags_writer_scene_debug_read_show_write_risk_confirm(HitagSApp* app) {
    DialogEx* dialog = app->dialog_ex;
    dialog_ex_reset(dialog);
    dialog_ex_set_header(dialog, "Write Matrix", 64, 0, AlignCenter, AlignTop);
    dialog_ex_set_text(
        dialog, "This changes card\nwrites temp ID\nthen restores", 64, 14, AlignCenter, AlignTop);
    dialog_ex_set_icon(dialog, 0, 12, &I_NFC_manual_60x50);
    dialog_ex_set_left_button_text(dialog, "Cancel");
    dialog_ex_set_right_button_text(dialog, "Run");
    dialog_ex_set_result_callback(dialog, hitags_writer_scene_debug_read_confirm_callback);
    dialog_ex_set_context(dialog, app);

    scene_manager_set_scene_state(
        app->scene_manager, HitagSSceneDebugRead, DebugReadStateRiskConfirm);
    view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewDialogEx);
}

static void hitags_writer_scene_debug_read_save_trace(HitagSApp* app) {
    if(!app->debug_trace) {
        notification_message(app->notifications, &sequence_error);
        return;
    }

    FuriString* filename = furi_string_alloc();
    FuriString* basename = furi_string_alloc();
    uint32_t timestamp = furi_hal_rtc_get_timestamp();
    if(timestamp == 0) timestamp = furi_get_tick();
    if(app->tag_uid == 0) {
        furi_string_printf(basename, "Trace_NoUID_%08lX", (unsigned long)timestamp);
    } else {
        furi_string_printf(
            basename, "Trace_%08lX_%08lX", (unsigned long)app->tag_uid, (unsigned long)timestamp);
    }
    furi_string_printf(
        filename,
        "%s/%s%s",
        HITAGS_TRACE_FOLDER,
        furi_string_get_cstr(basename),
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
        snprintf(app->text_store, sizeof(app->text_store), "%s", furi_string_get_cstr(basename));
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

    furi_string_free(basename);
    furi_string_free(filename);
}

void hitags_writer_scene_debug_read_on_enter(void* context) {
    HitagSApp* app = context;

    if(app->debug_tool == HitagSDebugToolWriteStimulusVerify) {
        DialogEx* dialog = app->dialog_ex;
        dialog_ex_reset(dialog);
        dialog_ex_set_header(dialog, "Write Matrix", 64, 0, AlignCenter, AlignTop);
        dialog_ex_set_text(
            dialog, "Baseline + detect\nNo write yet", 64, 14, AlignCenter, AlignTop);
        dialog_ex_set_icon(dialog, 0, 12, &I_NFC_manual_60x50);
        dialog_ex_set_left_button_text(dialog, "Cancel");
        dialog_ex_set_right_button_text(dialog, "Next");
        dialog_ex_set_result_callback(dialog, hitags_writer_scene_debug_read_confirm_callback);
        dialog_ex_set_context(dialog, app);

        scene_manager_set_scene_state(
            app->scene_manager, HitagSSceneDebugRead, DebugReadStateConfirm);
        view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewDialogEx);
    } else {
        hitags_writer_scene_debug_read_start_worker(app);
    }
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
                "UID:%08lX\nMode:%s\n%d/%d pgs read",
                (unsigned long)app->tag_uid,
                hitag_s_mode_name(app->debug_mode),
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

        } else if(event.event == HitagSEventDebugPartial) {
            hitags_writer_worker_stop(app);
            notification_message(app->notifications, &sequence_blink_stop);
            notification_message(app->notifications, &sequence_success);

            Widget* widget = app->widget;
            widget_reset(widget);

            widget_add_icon_element(widget, 83, 22, &I_WarningDolphinFlip_45x42);
            widget_add_string_element(
                widget, 40, 5, AlignCenter, AlignTop, FontPrimary, "Trace Ready");

            widget_add_string_multiline_element(
                widget,
                40,
                22,
                AlignCenter,
                AlignTop,
                FontSecondary,
                hitags_writer_scene_debug_read_status_text(app));
            widget_add_button_element(
                widget, GuiButtonTypeLeft, "Back", hitags_writer_widget_callback, app);
            widget_add_button_element(
                widget, GuiButtonTypeCenter, "Save", hitags_writer_widget_callback, app);
            widget_add_button_element(
                widget, GuiButtonTypeRight, "Retry", hitags_writer_widget_callback, app);

            scene_manager_set_scene_state(
                app->scene_manager, HitagSSceneDebugRead, DebugReadStatePartial);
            view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewWidget);
            consumed = true;

        } else if(event.event == HitagSEventDebugFailed) {
            hitags_writer_worker_stop(app);
            notification_message(app->notifications, &sequence_blink_stop);
            notification_message(app->notifications, &sequence_error);

            Widget* widget = app->widget;
            widget_reset(widget);

            widget_add_icon_element(widget, 83, 22, &I_WarningDolphinFlip_45x42);
            widget_add_string_element(
                widget, 40, 5, AlignCenter, AlignTop, FontPrimary, "No Trace");
            widget_add_string_multiline_element(
                widget,
                40,
                22,
                AlignCenter,
                AlignTop,
                FontSecondary,
                hitags_writer_scene_debug_read_failed_text(app));
            widget_add_button_element(
                widget, GuiButtonTypeLeft, "Back", hitags_writer_widget_callback, app);
            widget_add_button_element(
                widget, GuiButtonTypeRight, "Retry", hitags_writer_widget_callback, app);

            scene_manager_set_scene_state(
                app->scene_manager, HitagSSceneDebugRead, DebugReadStateFailed);
            view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewWidget);
            consumed = true;

        } else if(
            state == DebugReadStateConfirm && event.event == DialogExResultRight &&
            app->debug_tool == HitagSDebugToolWriteStimulusVerify) {
            hitags_writer_scene_debug_read_show_write_risk_confirm(app);
            consumed = true;

        } else if(
            (state == DebugReadStateConfirm || state == DebugReadStateRiskConfirm) &&
            event.event == DialogExResultRight) {
            dialog_ex_reset(app->dialog_ex);
            hitags_writer_scene_debug_read_start_worker(app);
            consumed = true;

        } else if(
            ((state == DebugReadStateConfirm || state == DebugReadStateRiskConfirm) &&
             event.event == DialogExResultLeft) ||
            event.event == GuiButtonTypeLeft) {
            scene_manager_previous_scene(app->scene_manager);
            consumed = true;

        } else if(event.event == GuiButtonTypeCenter) {
            /* Save trace — works in both success and failure states */
            hitags_writer_scene_debug_read_save_trace(app);
            consumed = true;

        } else if(
            event.event == GuiButtonTypeRight &&
            (state == DebugReadStateFailed || state == DebugReadStatePartial)) {
            /* Retry */
            widget_reset(app->widget);
            hitags_writer_scene_debug_read_start_worker(app);
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
    dialog_ex_reset(app->dialog_ex);
    widget_reset(app->widget);
}
