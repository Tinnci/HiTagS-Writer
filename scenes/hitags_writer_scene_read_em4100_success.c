/**
 * @file hitags_writer_scene_read_em4100_success.c
 * @brief Result screen for official LF RFID air reads.
 */

#include "../hitags_writer_i.h"
#include <strings.h>

#define TAG                               "HitagSReadEMOK"
#define HITAGS_READ_SUCCESS_MAX_HEX_WIDTH (7UL)

void hitags_writer_scene_read_em4100_success_on_enter(void* context) {
    HitagSApp* app = context;
    Widget* widget = app->widget;
    FuriString* display_text = furi_string_alloc();

    const char* protocol = protocol_dict_get_name(app->dict, app->protocol_id);
    const char* manufacturer = protocol_dict_get_manufacturer(app->dict, app->protocol_id);

    FURI_LOG_I(
        TAG,
        "Read EM4100 success: protocol=%s manufacturer=%s id=%d",
        protocol,
        manufacturer,
        (int)app->protocol_id);

    if(strcasecmp(protocol, manufacturer) != 0 && strcasecmp(manufacturer, "N/A") != 0) {
        furi_string_printf(display_text, "\e#%s %s\e#", manufacturer, protocol);
    } else {
        furi_string_printf(display_text, "\e#%s\e#", protocol);
    }
    widget_add_text_box_element(
        widget, 16, 2, 112, 14, AlignLeft, AlignTop, furi_string_get_cstr(display_text), true);

    furi_string_set(display_text, "HEX: ");
    const size_t data_size = protocol_dict_get_data_size(app->dict, app->protocol_id);
    uint8_t* data = malloc(data_size);
    protocol_dict_get_data(app->dict, app->protocol_id, data, data_size);

    FuriString* log_data = furi_string_alloc();
    for(size_t i = 0; i < data_size; i++) {
        furi_string_cat_printf(log_data, "%s%02X", i != 0 ? " " : "", data[i]);
        if(i == HITAGS_READ_SUCCESS_MAX_HEX_WIDTH) {
            furi_string_cat(display_text, " ...");
            break;
        }
        furi_string_cat_printf(display_text, "%s%02X", i != 0 ? " " : "", data[i]);
    }
    FURI_LOG_I(
        TAG, "Read EM4100 data: size=%d hex=%s", (int)data_size, furi_string_get_cstr(log_data));
    furi_string_free(log_data);

    const bool can_write_em4100 = app->protocol_id == LFRFIDProtocolEM4100 &&
                                  data_size >= EM4100_ID_SIZE;
    if(can_write_em4100) {
        memcpy(app->read_id, data, EM4100_ID_SIZE);
        memcpy(app->em4100_id, data, EM4100_ID_SIZE);
        FURI_LOG_I(TAG, "Read EM4100 result can be written to 8268");
    }

    free(data);

    FuriString* rendered_data = furi_string_alloc();
    protocol_dict_render_brief_data(app->dict, rendered_data, app->protocol_id);
    furi_string_cat_printf(display_text, "\n%s", furi_string_get_cstr(rendered_data));
    furi_string_free(rendered_data);

    widget_add_text_scroll_element(widget, 0, 16, 128, 35, furi_string_get_cstr(display_text));
    widget_add_button_element(
        widget, GuiButtonTypeLeft, "Retry", hitags_writer_widget_callback, app);
    if(can_write_em4100) {
        widget_add_button_element(
            widget, GuiButtonTypeRight, "Write", hitags_writer_widget_callback, app);
    } else {
        widget_add_button_element(
            widget, GuiButtonTypeCenter, "OK", hitags_writer_widget_callback, app);
    }
    notification_message(app->notifications, &sequence_set_green_255);
    view_dispatcher_switch_to_view(app->view_dispatcher, HitagSViewWidget);
    furi_string_free(display_text);
}

bool hitags_writer_scene_read_em4100_success_on_event(void* context, SceneManagerEvent event) {
    HitagSApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeBack) {
        scene_manager_previous_scene(app->scene_manager);
        consumed = true;
    } else if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == GuiButtonTypeLeft) {
            scene_manager_previous_scene(app->scene_manager);
            consumed = true;
        } else if(event.event == GuiButtonTypeRight && app->protocol_id == LFRFIDProtocolEM4100) {
            scene_manager_next_scene(app->scene_manager, HitagSSceneWriteConfirm);
            consumed = true;
        } else if(event.event == GuiButtonTypeCenter) {
            scene_manager_previous_scene(app->scene_manager);
            consumed = true;
        }
    }

    return consumed;
}

void hitags_writer_scene_read_em4100_success_on_exit(void* context) {
    HitagSApp* app = context;
    notification_message(app->notifications, &sequence_reset_green);
    widget_reset(app->widget);
}
