#include "dp104_receiver.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if(!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while(0)

typedef struct fixture {
    dp104_receiver receiver;
    uint8_t visible[DP104_FRAME_BYTES];
    int ready, busy, publishes, releases;
} fixture;

static int ready(void *p) { return ((fixture *)p)->ready; }
static int publish(void *p, const uint8_t *rgb, unsigned leds)
{
    fixture *f = (fixture *)p;
    CHECK(leds == DP104_LED_COUNT);
    if(f->busy) return 0;
    memcpy(f->visible, rgb, sizeof(f->visible));
    ++f->publishes;
    return 1;
}
static void release(void *p) { ++((fixture *)p)->releases; }
static void init(fixture *f)
{
    dp104_output_hooks hooks;
    memset(f, 0, sizeof(*f));
    f->ready = 1;
    hooks.context = f; hooks.ready = ready; hooks.publish = publish; hooks.release = release;
    dp104_receiver_init(&f->receiver, hooks);
}
static uint8_t send(fixture *f, uint8_t *p, uint32_t now)
{
    uint8_t reply[DP104_REPORT_BYTES];
    dp104_receiver_handle(&f->receiver, p, DP104_REPORT_BYTES, reply, now);
    CHECK(!memcmp(p, reply, 3));
    return reply[3];
}
static void packet(uint8_t *p, uint8_t op, uint8_t txn)
{
    memset(p, 0, DP104_REPORT_BYTES); p[0] = DP104_COMMAND; p[1] = op; p[2] = txn;
}
static void begin(fixture *f, const uint8_t *frame, uint8_t txn, uint32_t now)
{
    uint8_t p[DP104_REPORT_BYTES];
    packet(p, DP104_OP_BEGIN, txn);
    dp104_write16(p + 4, DP104_LED_COUNT);
    dp104_write32(p + 8, dp104_crc32(frame, DP104_FRAME_BYTES));
    CHECK(send(f, p, now) == DP104_OK);
}
static void chunks(fixture *f, const uint8_t *frame, uint8_t txn, uint32_t now)
{
    int start;
    /* Deliberately out of order; every chunk exactly fills eight LED slots. */
    for(start = 96; start >= 0; start -= 8)
    {
        uint8_t p[DP104_REPORT_BYTES];
        packet(p, DP104_OP_DATA, txn); dp104_write16(p + 4, (uint16_t)start); p[6] = 8;
        memcpy(p + 7, frame + start * 3, 24);
        CHECK(send(f, p, now) == DP104_OK);
        CHECK(send(f, p, now) == DP104_OK); /* Duplicate is harmless. */
    }
}
static uint8_t commit(fixture *f, uint8_t txn, uint32_t now)
{
    uint8_t p[DP104_REPORT_BYTES]; packet(p, DP104_OP_COMMIT, txn); return send(f, p, now);
}

int main(void)
{
    fixture f;
    uint8_t frame[DP104_FRAME_BYTES], p[DP104_REPORT_BYTES], response[DP104_REPORT_BYTES];
    unsigned i;
    CHECK(dp104_crc32((const uint8_t *)"123456789", 9) == 0xcbf43926u);
    for(i = 0; i < sizeof(frame); ++i) frame[i] = (uint8_t)(i * 53u + 7u);
    init(&f);

    packet(p, DP104_OP_CAPS, 1);
    dp104_receiver_handle(&f.receiver, p, sizeof(p), p, 0); /* In-place request. */
    CHECK(p[3] == DP104_OK && !memcmp(p + 4, DP104_CAPS_MAGIC, 8));
    CHECK(dp104_read16(p + 14) == 104 && p[16] == 8 && p[17] == 1);
    CHECK(dp104_read16(p + 18) == 1000);
    begin(&f, frame, 3, 10);
    CHECK(commit(&f, 3, 11) == DP104_INCOMPLETE && f.publishes == 0);
    chunks(&f, frame, 3, 12);
    CHECK(f.publishes == 0); /* Staging never changes visible output. */
    CHECK(commit(&f, 4, 13) == DP104_BAD_STATE);
    f.busy = 1;
    CHECK(commit(&f, 3, 13) == DP104_BUSY && !f.receiver.direct_active);
    f.busy = 0;
    CHECK(commit(&f, 3, 14) == DP104_OK);
    CHECK(f.publishes == 1 && !memcmp(frame, f.visible, sizeof(frame)));
    CHECK(f.receiver.direct_active && !f.receiver.staging_active);

    begin(&f, frame, 5, 20); chunks(&f, frame, 5, 21);
    f.receiver.staging[311] ^= 1;
    CHECK(commit(&f, 5, 22) == DP104_BAD_CRC && f.publishes == 1);
    CHECK(!memcmp(frame, f.visible, sizeof(frame)));
    packet(p, DP104_OP_DATA, 5); dp104_write16(p + 4, 103); p[6] = 2;
    CHECK(send(&f, p, 23) == DP104_BAD_INDEX);
    dp104_write16(p + 4, 0xffff); p[6] = 255;
    CHECK(send(&f, p, 23) == DP104_BAD_INDEX);
    p[6] = 0; CHECK(send(&f, p, 23) == DP104_BAD_INDEX);
    dp104_write16(p + 4, 0); p[6] = 1; p[31] = 1;
    CHECK(send(&f, p, 23) == DP104_BAD_REQUEST);
    packet(p, DP104_OP_STATUS, 8);
    CHECK(send(&f, p, 1013) == DP104_OK && f.releases == 0);
    CHECK(send(&f, p, 1014) == DP104_OK && f.releases == 1);
    CHECK(!f.receiver.direct_active); /* Queries/partial frames did not renew. */
    dp104_receiver_poll(&f.receiver, 1020);
    CHECK(!f.receiver.staging_active);

    init(&f); begin(&f, frame, 7, 0xfffffff0u); chunks(&f, frame, 7, 0xfffffff1u);
    CHECK(commit(&f, 7, 0xfffffff2u) == DP104_OK);
    dp104_receiver_poll(&f.receiver, 985); CHECK(f.releases == 0);
    dp104_receiver_poll(&f.receiver, 986); CHECK(f.releases == 1);
    begin(&f, frame, 8, 990); chunks(&f, frame, 8, 991); CHECK(commit(&f, 8, 992) == DP104_OK);
    f.ready = 0; dp104_receiver_poll(&f.receiver, 993);
    CHECK(f.releases == 2 && !f.receiver.direct_active);
    packet(p, DP104_OP_CAPS, 9); CHECK(send(&f, p, 994) == DP104_UNSUPPORTED);
    f.ready = 1; begin(&f, frame, 10, 1000); chunks(&f, frame, 10, 1001);
    CHECK(commit(&f, 10, 1002) == DP104_OK);
    packet(p, DP104_OP_RELEASE, 11);
    CHECK(send(&f, p, 1003) == DP104_OK && f.releases == 3);
    CHECK(send(&f, p, 1004) == DP104_OK && f.releases == 3);

    packet(p, DP104_OP_CAPS, 12);
    dp104_receiver_handle(&f.receiver, p, 31, response, 1005); CHECK(response[3] == DP104_BAD_LENGTH);
    dp104_receiver_handle(&f.receiver, NULL, 0, response, 1005); CHECK(response[3] == DP104_BAD_LENGTH);
    p[3] = 1; CHECK(send(&f, p, 1005) == DP104_BAD_REQUEST);
    p[3] = 0; p[0] = 0x99; CHECK(send(&f, p, 1005) == DP104_UNSUPPORTED);
    packet(p, 255, 12); CHECK(send(&f, p, 1005) == DP104_BAD_REQUEST);
    printf("receiver: atomic frames, bounds, CRC, lease, rollover, fallback and in-place handling PASS\n");
    return 0;
}
