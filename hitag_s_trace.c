/**
 * @file hitag_s_trace.c
 * @brief Debug trace lifecycle and persistence for HiTag S transactions.
 */

#include "hitag_s_trace.h"
#include <furi.h>
#include <storage/storage.h>

#define TAG                       "HitagSTrace"
#define HITAG_S_TRACE_MAX_BYTES   (48U * 1024U)
#define HITAG_S_TRACE_TRUNCATE_AT (HITAG_S_TRACE_MAX_BYTES - 96U)

static FuriString* g_debug_trace = NULL;
static bool g_trace_active = false;
static bool g_trace_truncated = false;

bool hitag_s_trace_is_active(void) {
    return g_trace_active && g_debug_trace;
}

void hitag_s_trace_append(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    hitag_s_trace_vappend(fmt, args);
    va_end(args);
}

void hitag_s_trace_vappend(const char* fmt, va_list args) {
    if(!hitag_s_trace_is_active()) return;
    if(g_trace_truncated) return;
    if(furi_string_size(g_debug_trace) >= HITAG_S_TRACE_TRUNCATE_AT) {
        furi_string_cat_str(g_debug_trace, "\n... TRACE TRUNCATED: memory budget reached ...\n");
        g_trace_truncated = true;
        return;
    }

    furi_string_cat_vprintf(g_debug_trace, fmt, args);
    if(furi_string_size(g_debug_trace) >= HITAG_S_TRACE_TRUNCATE_AT) {
        furi_string_left(g_debug_trace, HITAG_S_TRACE_TRUNCATE_AT);
        furi_string_cat_str(g_debug_trace, "\n... TRACE TRUNCATED: memory budget reached ...\n");
        g_trace_truncated = true;
    }
}

void hitag_s_debug_trace_start(void) {
    if(g_debug_trace) {
        furi_string_free(g_debug_trace);
    }
    g_debug_trace = furi_string_alloc();
    g_trace_truncated = false;
    furi_string_cat_str(g_debug_trace, "=== HiTag S Debug Trace v2 ===\n");
    g_trace_active = true;
    FURI_LOG_I(TAG, "Debug trace started");
}

void* hitag_s_debug_trace_stop(void) {
    g_trace_active = false;
    FuriString* result = g_debug_trace;
    g_debug_trace = NULL;
    FURI_LOG_I(TAG, "Debug trace stopped (%d bytes)", result ? (int)furi_string_size(result) : 0);
    return result;
}

bool hitag_s_debug_trace_save(void* storage_ptr, const char* path, void* trace_string) {
    FuriString* trace = (FuriString*)trace_string;
    if(!trace || furi_string_size(trace) == 0) return false;

    Storage* storage = (Storage*)storage_ptr;
    File* file = storage_file_alloc(storage);
    bool ok = false;

    if(storage_file_open(file, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        size_t len = furi_string_size(trace);
        size_t written = storage_file_write(file, furi_string_get_cstr(trace), len);
        ok = written == len;
        if(ok) {
            FURI_LOG_I(TAG, "Trace saved: %s (%d bytes)", path, (int)len);
        } else {
            FURI_LOG_E(TAG, "Trace write error: %d/%d", (int)written, (int)len);
        }
    } else {
        FURI_LOG_E(TAG, "Trace file open failed: %s", path);
    }

    storage_file_close(file);
    storage_file_free(file);
    return ok;
}
