#include "dp104_usb_stage.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if(!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while(0)

typedef struct fixture {
    int locked;
    int bad_lock;
    int busy;
    int reject;
    unsigned enters, leaves, queued;
    uintptr_t port;
    const uint8_t *retained;
} fixture;

static uint32_t enter(void *context)
{
    fixture *f = (fixture *)context;
    if(f->locked) f->bad_lock = 1;
    f->locked = 1;
    ++f->enters;
    return 0x1234u;
}
static void leave(void *context, uint32_t saved)
{
    fixture *f = (fixture *)context;
    if(!f->locked || saved != 0x1234u) f->bad_lock = 1;
    f->locked = 0;
    ++f->leaves;
}
static int busy(void *context, uintptr_t port)
{
    fixture *f = (fixture *)context;
    if(!f->locked || port != f->port) f->bad_lock = 1;
    return f->busy;
}
static int queue(void *context, uintptr_t port, const uint8_t *payload, unsigned length)
{
    fixture *f = (fixture *)context;
    if(!f->locked || port != f->port || length != DP104_REPORT_BYTES ||
       ((uintptr_t)payload & 3u)) f->bad_lock = 1;
    if(f->reject) return 0;
    f->retained = payload;
    f->busy = 1;
    ++f->queued;
    return 1;
}

int main(void)
{
    fixture f = {0};
    dp104_usb_stage stage = {0};
    dp104_usb_stage_hooks hooks = {&f, enter, leave, busy, queue};
    uint8_t report[DP104_REPORT_BYTES], expected[DP104_REPORT_BYTES];
    unsigned i;
    f.port = 0x84;
    for(i = 0; i < sizeof(report); ++i) report[i] = (uint8_t)(i + 1);
    memcpy(expected, report, sizeof(report));
    CHECK(dp104_usb_stage_send(&stage, hooks, f.port, report, sizeof(report)));
    CHECK(f.retained == (const uint8_t *)stage.words);
    memset(report, 0xee, sizeof(report));
    CHECK(!memcmp(f.retained, expected, sizeof(expected)));
    CHECK(!dp104_usb_stage_send(&stage, hooks, f.port, report, sizeof(report)));
    CHECK(!dp104_usb_stage_send(&stage, hooks, f.port + 1, report, sizeof(report)));
    CHECK(f.queued == 1 && !memcmp(f.retained, expected, sizeof(expected)));
    f.busy = 0; /* Synthetic completion, not a physical USB observation. */
    CHECK(dp104_usb_stage_send(&stage, hooks, f.port, report, sizeof(report)));
    CHECK(f.queued == 2 && !memcmp(f.retained, report, sizeof(report)));
    f.busy = 0;
    f.reject = 1;
    CHECK(!dp104_usb_stage_send(&stage, hooks, f.port, report, sizeof(report)));
    CHECK(!f.busy && f.queued == 2);
    CHECK(!dp104_usb_stage_send(&stage, hooks, f.port, report, sizeof(report) - 1));
    CHECK(!dp104_usb_stage_send(0, hooks, f.port, report, sizeof(report)));
    hooks.queue = 0;
    CHECK(!dp104_usb_stage_send(&stage, hooks, f.port, report, sizeof(report)));
    CHECK(!f.bad_lock && !f.locked && f.enters == f.leaves && f.enters == 5);
    return 0;
}
