/**
 * @file hitags_writer.c
 * @brief HiTagS Writer — Main entry point
 *
 * Flipper Zero application for writing EM4100 card data to
 * HiTag S 8268 magic chips.
 */

#include "hitags_writer_i.h"

#define TAG "HitagSWriter"

/* --- Event Callbacks --- */

static bool hitags_writer_custom_event_callback(void* context, uint32_t event) {
    furi_assert(context);
    HitagSApp* app = context;
    return scene_manager_handle_custom_event(app->scene_manager, event);
}

static bool hitags_writer_back_event_callback(void* context) {
    furi_assert(context);
    HitagSApp* app = context;
    return scene_manager_handle_back_event(app->scene_manager);
}

/* --- Utility Callbacks --- */

void hitags_writer_popup_timeout_callback(void* context) {
    HitagSApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventPopupClosed);
}

void hitags_writer_widget_callback(GuiButtonType result, InputType type, void* context) {
    HitagSApp* app = context;
    if(type == InputTypeShort) {
        view_dispatcher_send_custom_event(app->view_dispatcher, result);
    }
}

void hitags_writer_text_input_callback(void* context) {
    HitagSApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, HitagSEventNext);
}

/* --- Alloc / Free --- */

static HitagSApp* hitags_writer_alloc(void) {
    HitagSApp* app = malloc(sizeof(HitagSApp));

    /* Default password */
    app->password = HITAG_S_8268_PASSWORD;
    memset(app->em4100_id, 0, sizeof(app->em4100_id));
    app->tag_uid = 0;
    app->last_result = HitagSResultError;

    /* Open services */
    app->storage = furi_record_open(RECORD_STORAGE);
    app->dialogs = furi_record_open(RECORD_DIALOGS);
    app->gui = furi_record_open(RECORD_GUI);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);

    /* String storage */
    app->file_path = furi_string_alloc_set(HITAGS_WRITER_APP_FOLDER);

    /* LFRFID protocol dict for file loading */
    app->dict = protocol_dict_alloc(lfrfid_protocols, LFRFIDProtocolMax);
    app->protocol_id = PROTOCOL_NO;

    /* Scene Manager */
    app->scene_manager = scene_manager_alloc(&hitags_writer_scene_handlers, app);

    /* View Dispatcher */
    app->view_dispatcher = view_dispatcher_alloc();
    /* Note: view_dispatcher_enable_queue is deprecated in newer SDK */
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(
        app->view_dispatcher, hitags_writer_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, hitags_writer_back_event_callback);

    /* Submenu */
    app->submenu = submenu_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, HitagSViewSubmenu, submenu_get_view(app->submenu));

    /* Dialog */
    app->dialog_ex = dialog_ex_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, HitagSViewDialogEx, dialog_ex_get_view(app->dialog_ex));

    /* Popup */
    app->popup = popup_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, HitagSViewPopup, popup_get_view(app->popup));

    /* Widget */
    app->widget = widget_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, HitagSViewWidget, widget_get_view(app->widget));

    /* Text Input */
    app->text_input = text_input_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, HitagSViewTextInput, text_input_get_view(app->text_input));

    /* Byte Input */
    app->byte_input = byte_input_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, HitagSViewByteInput, byte_input_get_view(app->byte_input));

    return app;
}

static void hitags_writer_free(HitagSApp* app) {
    furi_assert(app);

    /* String */
    furi_string_free(app->file_path);

    /* Protocol dict */
    protocol_dict_free(app->dict);

    /* Views */
    view_dispatcher_remove_view(app->view_dispatcher, HitagSViewSubmenu);
    submenu_free(app->submenu);

    view_dispatcher_remove_view(app->view_dispatcher, HitagSViewDialogEx);
    dialog_ex_free(app->dialog_ex);

    view_dispatcher_remove_view(app->view_dispatcher, HitagSViewPopup);
    popup_free(app->popup);

    view_dispatcher_remove_view(app->view_dispatcher, HitagSViewWidget);
    widget_free(app->widget);

    view_dispatcher_remove_view(app->view_dispatcher, HitagSViewTextInput);
    text_input_free(app->text_input);

    view_dispatcher_remove_view(app->view_dispatcher, HitagSViewByteInput);
    byte_input_free(app->byte_input);

    /* Framework */
    view_dispatcher_free(app->view_dispatcher);
    scene_manager_free(app->scene_manager);

    /* Close records */
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_DIALOGS);

    free(app);
}

/* --- Entry Point --- */

int32_t hitags_writer_app(void* p) {
    UNUSED(p);

    HitagSApp* app = hitags_writer_alloc();

    FURI_LOG_I(TAG, "HiTagS Writer starting...");

    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    scene_manager_next_scene(app->scene_manager, HitagSSceneStart);

    view_dispatcher_run(app->view_dispatcher);

    FURI_LOG_I(TAG, "HiTagS Writer exiting...");

    hitags_writer_free(app);

    return 0;
}
