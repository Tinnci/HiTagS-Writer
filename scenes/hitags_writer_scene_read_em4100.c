/**
 * @file hitags_writer_scene_read_em4100.c
 * @brief Official LF RFID air read scene for EM4100/125 kHz tags.
 */

#include "../hitags_writer_i.h"

static const NotificationSequence sequence_blink_set_yellow = {
    &message_blink_set_color_yellow,
    NULL,
};

static const NotificationSequence sequence_blink_set_green = {
    &message_blink_set_color_green,
    NULL,
};

static const NotificationSequence sequence_blink_set_cyan = {
    &message_blink_set_color_cyan,
    NULL,
};

static void hitags_writer_lfrfid_read_callback(
    LFRFIDWorkerReadResult result,
    ProtocolId protocol,
    void* context) {
    HitagSApp* app = context;
    uint32_t event = 0;

    if(result == LFRFIDWorkerReadSenseStart) {
        event = HitagSEventLfReadSenseStart;
    } else if(result == LFRFIDWorkerReadSenseEnd) {
        event = HitagSEventLfReadSenseEnd;
    } else if(result == LFRFIDWorkerReadSenseCardStart) {
        event = HitagSEventLfReadSenseCardStart;
    } else if(result == LFRFIDWorkerReadSenseCardEnd) {
        event = HitagSEventLfReadSenseCardEnd;
    } else if(result == LFRFIDWorkerReadStartASK) {
        event = HitagSEventLfReadStartASK;
    } else if(result == LFRFIDWorkerReadStartPSK) {
        event = HitagSEventLfReadStartPSK;
    } else if(result == LFRFIDWorkerReadDone) {
        event = HitagSEventLfReadDone;
        app->protocol_id_next = protocol;
    } else {
        return;
    }

    view_dispatcher_send_custom_event(app->view_dispatcher, event);
}

void hitags_writer_scene_read_em4100_on_enter(void* context) {
    HitagSApp* app = context;
    Popup* popup = app->popup;

    popup_set_header(popup, "Reading RFID...", 89, 30, AlignCenter, AlignTop);
    popup_set_text(popup, "ASK/PSK auto\nscan", 89, 43, AlignCenter, AlignTop);
    popup_set_icon(popup, 0, 3, &I_NFC_manual_60x50);

    app->read_type = LFRFIDWorkerReadTypeAuto;
    lfrfid_worker_start_thread(app->lfworker);
    lfrfid_worker_read_start(
        app->lfworker, LFRFIDWorkerReadTypeAuto, hitags_writer_lfrfid_read_callback, app);

    notification_message(app->notifications, &sequence_blink_start_cyan);
    view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewPopup);
}

bool hitags_writer_scene_read_em4100_on_event(void* context, SceneManagerEvent event) {
    HitagSApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == HitagSEventLfReadSenseStart) {
            notification_message(app->notifications, &sequence_blink_set_yellow);
            consumed = true;
        } else if(event.event == HitagSEventLfReadSenseCardStart) {
            notification_message(app->notifications, &sequence_blink_set_green);
            consumed = true;
        } else if(
            event.event == HitagSEventLfReadSenseEnd ||
            event.event == HitagSEventLfReadSenseCardEnd) {
            notification_message(app->notifications, &sequence_blink_set_cyan);
            consumed = true;
        } else if(event.event == HitagSEventLfReadStartASK) {
            popup_set_text(app->popup, "ASK reading...", 89, 43, AlignCenter, AlignTop);
            consumed = true;
        } else if(event.event == HitagSEventLfReadStartPSK) {
            popup_set_text(app->popup, "PSK reading...", 89, 43, AlignCenter, AlignTop);
            consumed = true;
        } else if(event.event == HitagSEventLfReadDone) {
            app->protocol_id = app->protocol_id_next;
            notification_message(app->notifications, &sequence_success);
            scene_manager_next_scene(app->scene_manager, HitagSSceneReadEm4100Success);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        scene_manager_previous_scene(app->scene_manager);
        consumed = true;
    }

    return consumed;
}

void hitags_writer_scene_read_em4100_on_exit(void* context) {
    HitagSApp* app = context;
    notification_message(app->notifications, &sequence_blink_stop);
    lfrfid_worker_stop(app->lfworker);
    lfrfid_worker_stop_thread(app->lfworker);
    popup_reset(app->popup);
}
