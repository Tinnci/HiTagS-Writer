/**
 * @file hitag_s_trace.h
 * @brief Debug trace lifecycle and persistence for HiTag S transactions.
 */

#pragma once

#include <stdbool.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

void hitag_s_trace_append(const char* fmt, ...);

void hitag_s_trace_vappend(const char* fmt, va_list args);

bool hitag_s_trace_is_active(void);

void hitag_s_debug_trace_start(void);

void* hitag_s_debug_trace_stop(void);

bool hitag_s_debug_trace_save(void* storage, const char* path, void* trace_string);

#ifdef __cplusplus
}
#endif
