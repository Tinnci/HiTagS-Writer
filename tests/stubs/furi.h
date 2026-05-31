#pragma once

#define FURI_LOG_D(...)
#define FURI_LOG_I(...)
#define FURI_LOG_W(...)
#define FURI_LOG_E(...)

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define FURI_CRITICAL_ENTER()
#define FURI_CRITICAL_EXIT()
#define COUNT_OF(x) (sizeof(x) / sizeof((x)[0]))
#define UNUSED(x)   (void)(x)

typedef struct FuriString {
    char* data;
} FuriString;

static inline FuriString* furi_string_alloc(void) {
    return calloc(1, sizeof(FuriString));
}

static inline void furi_string_free(FuriString* value) {
    free(value);
}

static inline void furi_string_cat_str(FuriString* value, const char* str) {
    (void)value;
    (void)str;
}

static inline void furi_string_cat_vprintf(FuriString* value, const char* fmt, va_list args) {
    (void)value;
    (void)fmt;
    (void)args;
}

static inline size_t furi_string_size(FuriString* value) {
    (void)value;
    return 0;
}

static inline const char* furi_string_get_cstr(FuriString* value) {
    (void)value;
    return "";
}

static inline void furi_delay_us(uint32_t us) {
    (void)us;
}
