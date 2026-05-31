#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define HITAG_S_DUMP_MODEL_MAX_PAGES 64

typedef enum {
    HitagSPageStatusInvalid,
    HitagSPageStatusFromSelect,
    HitagSPageStatusReadOk,
} HitagSPageStatus;

typedef struct {
    uint32_t uid;
    uint32_t pages[HITAG_S_DUMP_MODEL_MAX_PAGES];
    HitagSPageStatus page_status[HITAG_S_DUMP_MODEL_MAX_PAGES];
    int max_page;
} HitagSTagDump;

typedef struct {
    uint32_t uid;
    uint32_t config;
    uint32_t pages[HITAG_S_DUMP_MODEL_MAX_PAGES];
    uint8_t addrs[HITAG_S_DUMP_MODEL_MAX_PAGES];
    size_t count;
} HitagSClonePlan;

static inline void hitag_s_dump_model_reset(HitagSTagDump* dump) {
    memset(dump, 0, sizeof(*dump));
    dump->max_page = 0;
}

static inline size_t hitag_s_dump_model_count_read_pages(const HitagSTagDump* dump) {
    size_t count = 0;
    for(int page = 0; page <= dump->max_page && page < HITAG_S_DUMP_MODEL_MAX_PAGES; page++) {
        if(dump->page_status[page] == HitagSPageStatusReadOk) count++;
    }
    return count;
}

static inline size_t hitag_s_dump_model_count_present_pages(const HitagSTagDump* dump) {
    size_t count = 0;
    for(int page = 0; page <= dump->max_page && page < HITAG_S_DUMP_MODEL_MAX_PAGES; page++) {
        if(dump->page_status[page] != HitagSPageStatusInvalid) count++;
    }
    return count;
}

static inline void
    hitag_s_dump_model_make_clone_plan(const HitagSTagDump* dump, HitagSClonePlan* plan) {
    memset(plan, 0, sizeof(*plan));
    plan->uid = dump->uid;
    if(dump->page_status[1] != HitagSPageStatusInvalid) {
        plan->config = dump->pages[1];
    }

    for(int page = 4; page <= dump->max_page && page < HITAG_S_DUMP_MODEL_MAX_PAGES; page++) {
        if(dump->page_status[page] != HitagSPageStatusReadOk) continue;
        plan->addrs[plan->count] = (uint8_t)page;
        plan->pages[plan->count] = dump->pages[page];
        plan->count++;
    }
}
