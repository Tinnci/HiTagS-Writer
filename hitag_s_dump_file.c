/**
 * @file hitag_s_dump_file.c
 * @brief HiTag S dump file persistence.
 */

#include "hitag_s_proto.h"
#include <furi.h>
#include <flipper_format/flipper_format.h>
#include <storage/storage.h>

#define TAG "HitagSDump"

#define HITAGS_DUMP_FILETYPE "HiTag S 8268 Dump"
#define HITAGS_DUMP_VERSION  1

bool hitag_s_dump_save(
    void* storage_ptr,
    const char* path,
    uint32_t uid,
    const uint32_t* pages,
    const bool* page_valid,
    int max_page) {
    Storage* storage = (Storage*)storage_ptr;
    FlipperFormat* file = flipper_format_file_alloc(storage);
    bool result = false;

    do {
        if(!flipper_format_file_open_always(file, path)) {
            FURI_LOG_E(TAG, "Dump save: can't open %s", path);
            break;
        }

        if(!flipper_format_write_header_cstr(file, HITAGS_DUMP_FILETYPE, HITAGS_DUMP_VERSION)) {
            break;
        }

        uint8_t uid_bytes[4] = {
            (uid >> 24) & 0xFF, (uid >> 16) & 0xFF, (uid >> 8) & 0xFF, uid & 0xFF};
        if(!flipper_format_write_hex(file, "UID", uid_bytes, 4)) break;

        uint32_t mp = (uint32_t)max_page;
        if(!flipper_format_write_uint32(file, "Max Page", &mp, 1)) break;

        if(page_valid[1]) {
            HitagSConfig cfg = hitag_s_parse_config(pages[1]);
            char comment[64];
            snprintf(
                comment,
                sizeof(comment),
                "MEMT=%d auth=%d LKP=%d LCON=%d TTFC=%d TTFDR=%d TTFM=%d",
                cfg.MEMT,
                cfg.auth,
                cfg.LKP,
                cfg.LCON,
                cfg.TTFC,
                cfg.TTFDR,
                cfg.TTFM);
            flipper_format_write_comment_cstr(file, comment);
        }

        bool write_ok = true;
        for(int p = 0; p <= max_page; p++) {
            if(!page_valid[p]) continue;

            char key[16];
            snprintf(key, sizeof(key), "Page %d", p);
            uint8_t page_bytes[4] = {
                (pages[p] >> 24) & 0xFF,
                (pages[p] >> 16) & 0xFF,
                (pages[p] >> 8) & 0xFF,
                pages[p] & 0xFF};
            if(!flipper_format_write_hex(file, key, page_bytes, 4)) {
                write_ok = false;
                break;
            }
        }
        if(!write_ok) break;

        result = true;
        FURI_LOG_I(TAG, "Dump saved to %s (%d pages)", path, max_page + 1);
    } while(false);

    flipper_format_free(file);
    return result;
}

bool hitag_s_dump_load(
    void* storage_ptr,
    const char* path,
    uint32_t* uid,
    uint32_t* pages,
    bool* page_valid,
    int* max_page) {
    Storage* storage = (Storage*)storage_ptr;
    FlipperFormat* file = flipper_format_file_alloc(storage);
    bool result = false;

    memset(pages, 0, HITAG_S_MAX_PAGES * sizeof(uint32_t));
    memset(page_valid, 0, HITAG_S_MAX_PAGES * sizeof(bool));
    *uid = 0;
    *max_page = 0;

    do {
        if(!flipper_format_file_open_existing(file, path)) {
            FURI_LOG_E(TAG, "Dump load: can't open %s", path);
            break;
        }

        uint32_t version = 0;
        FuriString* filetype = furi_string_alloc();
        if(!flipper_format_read_header(file, filetype, &version)) {
            furi_string_free(filetype);
            break;
        }
        if(furi_string_cmp_str(filetype, HITAGS_DUMP_FILETYPE) != 0 ||
           version != HITAGS_DUMP_VERSION) {
            FURI_LOG_E(TAG, "Dump load: wrong filetype or version");
            furi_string_free(filetype);
            break;
        }
        furi_string_free(filetype);

        uint8_t uid_bytes[4] = {0};
        if(!flipper_format_read_hex(file, "UID", uid_bytes, 4)) break;
        *uid = ((uint32_t)uid_bytes[0] << 24) | ((uint32_t)uid_bytes[1] << 16) |
               ((uint32_t)uid_bytes[2] << 8) | (uint32_t)uid_bytes[3];
        pages[0] = *uid;
        page_valid[0] = true;

        uint32_t mp = 0;
        if(!flipper_format_read_uint32(file, "Max Page", &mp, 1)) break;
        *max_page = (int)mp;

        for(int p = 0; p <= *max_page; p++) {
            char key[16];
            snprintf(key, sizeof(key), "Page %d", p);

            uint8_t page_bytes[4] = {0};
            if(flipper_format_read_hex(file, key, page_bytes, 4)) {
                pages[p] = ((uint32_t)page_bytes[0] << 24) | ((uint32_t)page_bytes[1] << 16) |
                           ((uint32_t)page_bytes[2] << 8) | (uint32_t)page_bytes[3];
                page_valid[p] = true;
            }
        }

        result = true;
        FURI_LOG_I(
            TAG,
            "Dump loaded from %s: UID=%08lX max_page=%d",
            path,
            (unsigned long)*uid,
            *max_page);
    } while(false);

    flipper_format_free(file);
    return result;
}
