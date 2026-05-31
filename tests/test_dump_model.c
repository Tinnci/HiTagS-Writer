#include <assert.h>
#include <stdint.h>

#include "hitag_s_dump_model.h"

static void test_clone_plan_uses_only_read_ok_data_pages(void) {
    HitagSTagDump dump;
    hitag_s_dump_model_reset(&dump);
    dump.uid = 0x11223344;
    dump.pages[0] = dump.uid;
    dump.page_status[0] = HitagSDumpPageStatusReadOk;
    dump.pages[1] = 0xDAA40000;
    dump.page_status[1] = HitagSDumpPageStatusFromSelect;
    dump.pages[4] = 0x11111111;
    dump.page_status[4] = HitagSDumpPageStatusReadOk;
    dump.pages[5] = 0x22222222;
    dump.page_status[5] = HitagSDumpPageStatusInvalid;
    dump.pages[6] = 0x33333333;
    dump.page_status[6] = HitagSDumpPageStatusReadOk;
    dump.max_page = 6;

    HitagSClonePlan plan;
    hitag_s_dump_model_make_clone_plan(&dump, &plan);

    assert(plan.uid == 0x11223344);
    assert(plan.config == 0xDAA40000);
    assert(plan.count == 2);
    assert(plan.addrs[0] == 4);
    assert(plan.pages[0] == 0x11111111);
    assert(plan.addrs[1] == 6);
    assert(plan.pages[1] == 0x33333333);
}

static void test_dump_read_count_counts_only_real_page_reads(void) {
    HitagSTagDump dump;
    hitag_s_dump_model_reset(&dump);
    dump.max_page = 3;
    dump.page_status[0] = HitagSDumpPageStatusReadOk;
    dump.page_status[1] = HitagSDumpPageStatusFromSelect;
    dump.page_status[2] = HitagSDumpPageStatusReadOk;
    dump.page_status[3] = HitagSDumpPageStatusInvalid;

    assert(hitag_s_dump_model_count_read_pages(&dump) == 2);
    assert(hitag_s_dump_model_count_present_pages(&dump) == 3);
}

int main(void) {
    test_clone_plan_uses_only_read_ok_data_pages();
    test_dump_read_count_counts_only_real_page_reads();
    return 0;
}
