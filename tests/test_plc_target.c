#include "dp104_plc_target.h"
#include "dp104_version.h"
#include "dp104_wire.h"
#include "dp104_wire_stream.h"

#include <stdio.h>
#include <string.h>

#ifndef DP104_PLC_HOST_TEST
#error "Compile this test and dp104_plc_target.c with DP104_PLC_HOST_TEST"
#endif

#define CHECK(expr) do { if(!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); ++failures; \
} } while(0)

#define RECEIVER_SNAPSHOT_OP 0xf3u
#define PRE_FIX_RX_DMA_BYTES 72u

/* Test-only synthetic bindings. The coefficient basis is not vendor-table proof. */
dp104_plc_patch_state __dp104_plc_state;
const uint32_t __dp104_twiddle_basis[DP104_TWIDDLE_WORDS] = {
    [1] = UINT32_C(0x3f800000)
};
volatile uint8_t dp104_plc_test_key_buffer[DP104_FRAME_BYTES];
volatile uint8_t dp104_plc_test_dirty;

static uint32_t fake_ms;
static uint32_t fake_primask;
static unsigned irq_saves;
static unsigned irq_restores;
static unsigned queue_pops;
static unsigned fake_faults;
static int queue_pending;
static int overwrite_slot_on_restore;
static uint8_t fake_queue_slot[DP104_PLC_QUEUE_SLOT_BYTES];
static uint8_t tx_frames[160][DP104_WIRE_BYTES];
static uint32_t tx_lengths[160];
static uint32_t tx_modes[160];
static unsigned tx_count;
static int failures;
static uint32_t fake_dma_flags_value;
static uint32_t fake_dma_remaining_value = DP104_PLC_RX_DMA_BYTES;
static uint32_t fake_uart_control_value = 0x10u;
static uint32_t fake_uart_status_value = 0xd0u;
static unsigned fake_uart_status_reads, fake_uart_data_reads, fake_dma_irq_enables;
uint32_t dp104_plc_test_dma_flags(void) { return fake_dma_flags_value; }
uint32_t dp104_plc_test_dma_remaining(void) { return fake_dma_remaining_value; }
void dp104_plc_test_dma_clear(uint32_t flags) { fake_dma_flags_value &= ~flags; }
void dp104_plc_test_dma_enable_irq(void) { ++fake_dma_irq_enables; }
uint32_t dp104_plc_test_uart_control(void) { return fake_uart_control_value; }
uint32_t dp104_plc_test_uart_status(void) { ++fake_uart_status_reads; return fake_uart_status_value; }
uint32_t dp104_plc_test_uart_data(void) { ++fake_uart_data_reads; return 0; }


static size_t accepted_uart_extension_size(const uint8_t *bytes, size_t length)
{
    size_t extension_length = dp104_wire_frame_size(bytes, length);
    if(extension_length == 0) return 0;
    if(extension_length == length) return extension_length;
    if(extension_length % 7u == 0 &&
       extension_length + DP104_WIRE_HEADER_BYTES == length &&
       bytes[extension_length] == DP104_WIRE_RESPONSE &&
       bytes[extension_length + 1u] == DP104_WIRE_TYPE &&
       bytes[extension_length + 2u] == 0)
        return extension_length;
    return 0;
}

typedef struct master_stream_capture {
    uint8_t responses[2][DP104_REPORT_BYTES];
    unsigned count;
    unsigned legacy_count;
} master_stream_capture;

static int drain_master_stream(dp104_wire_stream *stream,
                               master_stream_capture *capture)
{
    uint8_t record[DP104_WIRE_STREAM_FRAME_MAX];
    size_t record_length;
    uint8_t is_extension;
    int result;

    for(;;)
    {
        uint8_t report[DP104_REPORT_BYTES];
        result = dp104_wire_stream_next(stream, record, sizeof(record),
                                        &record_length, &is_extension);
        if(result == 0) return 1;
        if(result < 0) return 0;
        if(!is_extension)
        {
            ++capture->legacy_count;
            return 0;
        }
        if(capture->count >= 2 ||
           !dp104_wire_decode(DP104_WIRE_RESPONSE, record, record_length,
                              report, sizeof(report)))
            return 0;
        memcpy(capture->responses[capture->count], report, sizeof(report));
        ++capture->count;
    }
}

uint32_t dp104_plc_test_millis(void)
{
    return fake_ms;
}

uint32_t dp104_plc_test_irq_save(void)
{
    uint32_t previous = fake_primask;
    if(fake_primask) ++fake_faults;
    fake_primask = 1;
    ++irq_saves;
    return previous;
}

void dp104_plc_test_irq_restore(uint32_t primask)
{
    if(!fake_primask) ++fake_faults;
    fake_primask = primask;
    ++irq_restores;
    if(overwrite_slot_on_restore)
    {
        memset(fake_queue_slot, 0xee, sizeof(fake_queue_slot));
        overwrite_slot_on_restore = 0;
    }
}

void dp104_plc_test_legacy_pop(uint8_t **out)
{
    if(!out || !fake_primask) ++fake_faults;
    ++queue_pops;
    if(queue_pending)
    {
        *out = fake_queue_slot;
        queue_pending = 0;
    }
    else
        *out = 0;
}

void dp104_plc_test_uart_send(uint8_t *bytes, uint32_t length, uint32_t mode)
{
    if(fake_primask || !bytes || length > DP104_WIRE_BYTES ||
       accepted_uart_extension_size(bytes, length) == 0 || mode != 1)
        ++fake_faults;
    if(tx_count >= sizeof(tx_frames) / sizeof(tx_frames[0]) ||
       length > DP104_WIRE_BYTES)
    {
        ++fake_faults;
        return;
    }
    memcpy(tx_frames[tx_count], bytes, length);
    tx_lengths[tx_count] = length;
    tx_modes[tx_count] = mode;
    ++tx_count;
}

static void make_request(uint8_t request[DP104_REPORT_BYTES], uint8_t op, uint8_t txn)
{
    memset(request, 0, DP104_REPORT_BYTES);
    request[0] = DP104_COMMAND;
    request[1] = op;
    request[2] = txn;
}

static void enqueue_bytes(const uint8_t *bytes, unsigned length, int overwrite_after_pop)
{
    memset(fake_queue_slot, 0, sizeof(fake_queue_slot));
    if(length > DP104_PLC_QUEUE_DATA_BYTES)
    {
        ++fake_faults;
        length = DP104_PLC_QUEUE_DATA_BYTES;
    }
    memcpy(fake_queue_slot, bytes, length);
    fake_queue_slot[DP104_PLC_QUEUE_DATA_BYTES] = (uint8_t)length;
    queue_pending = 1;
    overwrite_slot_on_restore = overwrite_after_pop;
}

static int send_request(const uint8_t request[DP104_REPORT_BYTES],
                        uint8_t response[DP104_REPORT_BYTES])
{
    uint8_t frame[DP104_WIRE_BYTES];
    uint8_t *legacy = 0;
    size_t frame_length;
    size_t first_fragment;
    unsigned before = tx_count;
    if(!dp104_wire_encode(DP104_WIRE_REQUEST, request, DP104_REPORT_BYTES,
                          frame, sizeof(frame))) return 0;
    frame_length = dp104_wire_frame_size(frame, sizeof(frame));
    if(frame_length != DP104_WIRE_BYTES || frame_length > sizeof(frame)) return 0;
    first_fragment = frame_length / 2u;
    dp104_plc_uart_rx(frame, (uint32_t)first_fragment);
    dp104_plc_queue_hook(&legacy);
    if(legacy || tx_count != before) return 0;
    dp104_plc_uart_rx(frame + first_fragment,
                      (uint32_t)(frame_length - first_fragment));
    dp104_plc_queue_hook(&legacy);
    if(legacy || tx_count != before + 1u ||
       accepted_uart_extension_size(tx_frames[before], tx_lengths[before]) == 0 ||
       tx_modes[before] != 1)
        return 0;
    return dp104_wire_decode(DP104_WIRE_RESPONSE, tx_frames[before],
                             dp104_wire_frame_size(tx_frames[before],
                                                   tx_lengths[before]),
                             response, DP104_REPORT_BYTES);
}

static int send_status_request(uint8_t request[DP104_REPORT_BYTES], uint8_t status)
{
    uint8_t response[DP104_REPORT_BYTES];
    return send_request(request, response) && response[0] == DP104_COMMAND &&
           response[1] == request[1] && response[2] == request[2] &&
           response[3] == status;
}

static int send_empty_queue(void)
{
    uint8_t *legacy = 0;
    queue_pending = 0;
    dp104_plc_queue_hook(&legacy);
    return legacy == 0;
}

static int queue_set_order(uint8_t txn, const uint8_t order[3], const uint8_t tag[4],
                           uint8_t expected_status)
{
    uint8_t request[DP104_REPORT_BYTES];
    make_request(request, DP104_PLC_CAL_SET_ORDER, txn);
    memcpy(request + 4, order, 3);
    memcpy(request + 7, tag, 4);
    memset(request + 11, txn ? txn : 1, 16);
    {
        uint8_t response[DP104_REPORT_BYTES];
        unsigned i;
        if(!send_request(request, response) || response[0] != DP104_COMMAND ||
           response[1] != request[1] || response[2] != txn || response[3] != expected_status)
            return 0;
        if(expected_status == DP104_OK)
        {
            if(memcmp(response + 4, order, 3) || memcmp(response + 7, request + 11, 16))
                return 0;
            for(i = 23; i < 32; ++i) if(response[i]) return 0;
        }
        return 1;
    }
}

static int queue_pulse(uint8_t txn, uint8_t channel, uint8_t intensity,
                       uint8_t expected_status)
{
    uint8_t request[DP104_REPORT_BYTES];
    make_request(request, DP104_PLC_CAL_RAW_PULSE, txn);
    request[4] = channel;
    request[5] = intensity;
    return send_status_request(request, expected_status);
}

static int send_app_info(const uint8_t nonce[16], uint8_t txn,
                         uint8_t response[DP104_REPORT_BYTES])
{
    uint8_t request[DP104_REPORT_BYTES];
    make_request(request, APP_INFO_OP, txn);
    memcpy(request + 4, nonce, 16);
    return send_request(request, response);
}

static int send_receiver_snapshot(const uint8_t nonce[4], uint8_t txn,
                                  uint8_t response[DP104_REPORT_BYTES])
{
    uint8_t request[DP104_REPORT_BYTES];
    make_request(request, RECEIVER_SNAPSHOT_OP, txn);
    memcpy(request + 4, nonce, 4);
    return send_request(request, response);
}

static int check_app_info(const uint8_t nonce[16], uint8_t txn,
                          uint8_t ready,
                          const uint8_t response[DP104_REPORT_BYTES])
{
    return response[0] == DP104_COMMAND && response[1] == APP_INFO_OP &&
           response[2] == txn && response[3] == DP104_OK &&
           memcmp(response + 4, DP104_CAPS_MAGIC, 8) == 0 &&
           response[12] == COMPAT_VERSION_MAJOR &&
           response[13] == COMPAT_VERSION_MINOR &&
           response[14] == COMPAT_VERSION_PATCH &&
           response[15] == ready &&
           memcmp(response + 16, nonce, 16) == 0;
}

static int check_empty_error_tail(const uint8_t response[DP104_REPORT_BYTES])
{
    unsigned i;
    for(i = 4; i < DP104_REPORT_BYTES; ++i)
        if(response[i]) return 0;
    return 1;
}

static int check_irq_balance(void)
{
    return fake_primask == 0 && irq_saves == irq_restores && fake_faults == 0;
}

static int reset_diagnostic_fixture(void)
{
    /* Model the image startup BSS clear before each independent boot fixture. */
    memset(&__dp104_plc_state, 0, sizeof(__dp104_plc_state));
    memset((void *)dp104_plc_test_key_buffer, 0,
           sizeof(dp104_plc_test_key_buffer));
    dp104_plc_test_dirty = 0;
    fake_ms = 0;
    fake_primask = 0;
    irq_saves = 0;
    irq_restores = 0;
    queue_pops = 0;
    fake_faults = 0;
    queue_pending = 0;
    overwrite_slot_on_restore = 0;
    memset(fake_queue_slot, 0, sizeof(fake_queue_slot));
    memset(tx_frames, 0, sizeof(tx_frames));
    memset(tx_lengths, 0, sizeof(tx_lengths));
    memset(tx_modes, 0, sizeof(tx_modes));
    tx_count = 0;
    return dp104_plc_target_init();
}

static void make_diagnostic_request(uint8_t request[DP104_REPORT_BYTES],
                                    uint8_t transaction,
                                    const uint8_t nonce[4], uint16_t offset)
{
    make_request(request, DP104_PLC_DIAGNOSTIC_OP, transaction);
    if(nonce) memcpy(request + 4, nonce, 4);
    dp104_write16(request + 8, offset);
}

static int send_diagnostic_request(uint8_t transaction,
                                   const uint8_t nonce[4], uint16_t offset,
                                   uint8_t response[DP104_REPORT_BYTES])
{
    uint8_t request[DP104_REPORT_BYTES];
    make_diagnostic_request(request, transaction, nonce, offset);
    return send_request(request, response);
}

static const uint8_t *find_diagnostic_record(
    const uint8_t blob[DP104_PLC_DIAGNOSTIC_TOTAL_BYTES], uint8_t event,
    uint8_t operation, uint8_t transaction)
{
    const uint16_t count = dp104_read16(blob + 14);
    uint16_t i;
    for(i = 0; i < count; ++i)
    {
        const uint8_t *record = blob + DP104_PLC_DIAGNOSTIC_HEADER_BYTES +
            (size_t)i * DP104_PLC_DIAGNOSTIC_RECORD_BYTES;
        if(record[24] == event && record[27] == operation &&
           record[28] == transaction)
            return record;
    }
    return NULL;
}

static void test_plc_diagnostic(void)
{
    const uint8_t preflight_nonce[4] = {0x11, 0x22, 0x33, 0x44};
    const uint8_t arm_nonce[4] = {0x51, 0x62, 0x73, 0x84};
    const uint8_t freeze_nonce[4] = {0xa1, 0xb2, 0xc3, 0xd4};
    const uint8_t wrong_nonce[4] = {0x19, 0x28, 0x37, 0x46};
    const uint8_t f3_nonce[4] = {0x91, 0x82, 0x73, 0x64};
    const uint8_t color_order[3] = {0, 1, 2};
    uint8_t request[DP104_REPORT_BYTES];
    uint8_t response[DP104_REPORT_BYTES];
    uint8_t f3_response[DP104_REPORT_BYTES];
    uint8_t wire[DP104_WIRE_BYTES];
    uint8_t short_wire[DP104_WIRE_BYTES - 1u];
    uint8_t raw72[PRE_FIX_RX_DMA_BYTES];
    uint8_t oversized[DP104_PLC_RX_DMA_BYTES + 1u];
    uint8_t blob[DP104_PLC_DIAGNOSTIC_TOTAL_BYTES];
    uint8_t records_before[DP104_PLC_DIAGNOSTIC_RECORD_CAPACITY]
                          [DP104_PLC_DIAGNOSTIC_RECORD_BYTES];
    uint8_t rgb[DP104_FRAME_BYTES];
    uint8_t *legacy;
    const uint8_t *record;
    size_t wire_length;
    uint16_t offset;
    unsigned i;
    int partial_commit_candidate = 0;
    int complete_commit_candidate = 0;

    CHECK(reset_diagnostic_fixture());

    /* A short reserved D2 envelope is consumed by the existing parser and
     * cannot produce a local F5 success response. */
    memset(short_wire, DP104_WIRE_SYMBOL_MIN, sizeof(short_wire));
    short_wire[0] = DP104_WIRE_REQUEST;
    short_wire[1] = DP104_WIRE_TYPE;
    short_wire[2] = DP104_WIRE_ENCODED_BYTES - 1u;
    dp104_plc_uart_rx(short_wire, (uint32_t)sizeof(short_wire));
    legacy = 0;
    dp104_plc_queue_hook(&legacy);
    CHECK(legacy == 0 && tx_count == 0);
    CHECK(__dp104_plc_state.uart_stream.used == 0);

    /* F5 preflight exposes only the fixed schema and does not arm/freeze. */
    CHECK(send_diagnostic_request(0x31, preflight_nonce,
                                  DP104_PLC_DIAGNOSTIC_PREFLIGHT, response));
    CHECK(response[0] == DP104_COMMAND &&
          response[1] == DP104_PLC_DIAGNOSTIC_OP && response[2] == 0x31 &&
          response[3] == DP104_OK);
    CHECK(memcmp(response + 4, preflight_nonce, 4) == 0);
    CHECK(memcmp(response + 8, "P5RX", 4) == 0);
    CHECK(dp104_read16(response + 12) == DP104_PLC_DIAGNOSTIC_PREFLIGHT);
    CHECK(dp104_read16(response + 14) == DP104_PLC_DIAGNOSTIC_TOTAL_BYTES);
    CHECK(dp104_read32(response + 16) == 1u);
    CHECK(dp104_read32(response + 20) == DP104_PLC_DIAGNOSTIC_RECORD_BYTES);
    CHECK(dp104_read32(response + 24) == DP104_PLC_DIAGNOSTIC_RECORD_CAPACITY);
    CHECK(dp104_read32(response + 28) == DP104_PLC_DIAGNOSTIC_TOTAL_BYTES);
    CHECK(!__dp104_plc_state.diagnostic.armed &&
          !__dp104_plc_state.diagnostic.frozen);

    /* Reject invalid transaction, nonce, request status, tail, and page offset. */
    make_diagnostic_request(request, 0, preflight_nonce,
                            DP104_PLC_DIAGNOSTIC_PREFLIGHT);
    CHECK(send_request(request, response));
    CHECK(response[3] == DP104_BAD_REQUEST && check_empty_error_tail(response));
    make_diagnostic_request(request, 0x32, (const uint8_t[4]){0, 0, 0, 0},
                            DP104_PLC_DIAGNOSTIC_PREFLIGHT);
    CHECK(send_request(request, response));
    CHECK(response[3] == DP104_BAD_REQUEST && check_empty_error_tail(response));
    make_diagnostic_request(request, 0x33, preflight_nonce,
                            DP104_PLC_DIAGNOSTIC_PREFLIGHT);
    request[3] = 1;
    CHECK(send_request(request, response));
    CHECK(response[3] == DP104_BAD_REQUEST && check_empty_error_tail(response));
    make_diagnostic_request(request, 0x34, preflight_nonce,
                            DP104_PLC_DIAGNOSTIC_PREFLIGHT);
    request[10] = 1;
    CHECK(send_request(request, response));
    CHECK(response[3] == DP104_BAD_REQUEST && check_empty_error_tail(response));
    CHECK(send_diagnostic_request(0x35, preflight_nonce, 16, response));
    CHECK(response[3] == DP104_BAD_REQUEST && check_empty_error_tail(response));
    CHECK(!__dp104_plc_state.diagnostic.frozen);

    /* Arm resets metadata only: it does not clear old ring storage or the
     * independent ISR samples at the agreed target offsets. */
    __dp104_plc_state.diagnostic.records[0][0] = 0xa5;
    __dp104_plc_state.rx_sr_before_dr = UINT32_C(0x99887766);
    __dp104_plc_state.rx_cndtr_after_dr = 12;
    __dp104_plc_state.rx_sample_count = 34;
    CHECK(send_diagnostic_request(0x36, arm_nonce,
                                  DP104_PLC_DIAGNOSTIC_ARM, response));
    CHECK(response[3] == DP104_OK && memcmp(response + 4, arm_nonce, 4) == 0);
    CHECK(memcmp(response + 8, "P5RX", 4) == 0);
    CHECK(dp104_read16(response + 12) == DP104_PLC_DIAGNOSTIC_ARM);
    CHECK(dp104_read16(response + 14) == DP104_PLC_DIAGNOSTIC_TOTAL_BYTES);
    for(i = 16; i < DP104_REPORT_BYTES; ++i) CHECK(response[i] == 0);
    CHECK(__dp104_plc_state.diagnostic.armed &&
          !__dp104_plc_state.diagnostic.frozen);
    CHECK(__dp104_plc_state.diagnostic.record_count == 0 &&
          __dp104_plc_state.diagnostic.write_index == 0);
    CHECK(__dp104_plc_state.diagnostic.records[0][0] == 0xa5);
    CHECK(__dp104_plc_state.rx_sr_before_dr == UINT32_C(0x99887766) &&
          __dp104_plc_state.rx_cndtr_after_dr == 12 &&
          __dp104_plc_state.rx_sample_count == 34);
    __dp104_plc_state.color_order[0] = color_order[0];
    __dp104_plc_state.color_order[1] = color_order[1];
    __dp104_plc_state.color_order[2] = color_order[2];
    __dp104_plc_state.color_order_verified = 1;

    /* Invalid >128-byte callback and the historical 72-byte boundary are metadata
     * observations only. A parser drop is recorded separately. */
    memset(oversized, 0xff, sizeof(oversized));
    __dp104_plc_state.rx_sr_before_dr = UINT32_C(0x10203040);
    __dp104_plc_state.rx_cndtr_after_dr = 73;
    __dp104_plc_state.rx_sample_count = 100;
    dp104_plc_uart_rx(oversized, (uint32_t)sizeof(oversized));
    CHECK(__dp104_plc_state.diagnostic.rx_push_failures == 1);
    CHECK(__dp104_plc_state.uart_stream.dropping != 0);
    memset(raw72, 0xff, sizeof(raw72));
    __dp104_plc_state.rx_sr_before_dr = UINT32_C(0x11223344);
    __dp104_plc_state.rx_cndtr_after_dr = 0;
    __dp104_plc_state.rx_sample_count = 101;
    dp104_plc_uart_rx(raw72, (uint32_t)sizeof(raw72));
    CHECK(__dp104_plc_state.diagnostic.rx_dma_full_callbacks == 1);
    legacy = 0;
    dp104_plc_queue_hook(&legacy);
    CHECK(legacy == 0 && __dp104_plc_state.uart_stream.used == 0);
    CHECK(__dp104_plc_state.uart_stream.dropping != 0);

    /* Split the fixed D2 prefix across callbacks. The candidate record keeps
     * only command/op/txn/index/count plus callback and parser metadata. */
    make_request(request, DP104_OP_DATA, 0x42);
    dp104_write16(request + 4, 8);
    request[6] = 8;
    for(i = 0; i < 24; ++i) request[7 + i] = (uint8_t)(0x20u + i);
    CHECK(dp104_wire_encode(DP104_WIRE_REQUEST, request, sizeof(request),
                            wire, sizeof(wire)));
    wire_length = dp104_wire_frame_size(wire, sizeof(wire));
    CHECK(wire_length == DP104_WIRE_BYTES);
    __dp104_plc_state.rx_sr_before_dr = UINT32_C(0xa1b2c3d4);
    __dp104_plc_state.rx_cndtr_after_dr = 9;
    __dp104_plc_state.rx_sample_count = 0x101;
    dp104_plc_uart_rx(wire, 2);
    legacy = 0;
    dp104_plc_queue_hook(&legacy);
    CHECK(legacy == 0 && __dp104_plc_state.uart_stream.used == 2);
    __dp104_plc_state.rx_sr_before_dr = UINT32_C(0x55667788);
    __dp104_plc_state.rx_cndtr_after_dr = 4;
    __dp104_plc_state.rx_sample_count = 0x102;
    dp104_plc_uart_rx(wire + 2, (uint32_t)(wire_length - 2u));
    legacy = 0;
    dp104_plc_queue_hook(&legacy);
    CHECK(legacy == 0 && __dp104_plc_state.uart_stream.used == 0);
    CHECK(tx_count > 0);
    CHECK(dp104_wire_decode(DP104_WIRE_RESPONSE, tx_frames[tx_count - 1],
                            tx_lengths[tx_count - 1], response,
                            sizeof(response)));
    CHECK(response[1] == DP104_OP_DATA && response[2] == 0x42 &&
          response[3] == DP104_BAD_STATE);

    /* A full decoded COMMIT attempt gets its own status/count marker; this
     * receiver has no staging transaction, so the real reply is BAD_STATE.
     * First inject only 39 of its 43 six-bit symbols after a legacy prefix in
     * a 72-byte callback; the later complete envelope must record 43. */
    make_request(request, DP104_OP_COMMIT, 0x53);
    CHECK(dp104_wire_encode(DP104_WIRE_REQUEST, request, sizeof(request),
                            wire, sizeof(wire)));
    memset(raw72, 0xff, sizeof(raw72));
    memcpy(raw72 + 30, wire, 30u + 12u); /* Header plus 39 symbols. */
    __dp104_plc_state.rx_sr_before_dr = UINT32_C(0xdeadbeef);
    __dp104_plc_state.rx_cndtr_after_dr = 0;
    __dp104_plc_state.rx_sample_count = 0x303;
    dp104_plc_uart_rx(raw72, (uint32_t)sizeof(raw72));
    CHECK(__dp104_plc_state.diagnostic.rx_dma_full_callbacks == 2u);
    __dp104_plc_state.rx_sr_before_dr = UINT32_C(0x76543210);
    __dp104_plc_state.rx_cndtr_after_dr = 3;
    __dp104_plc_state.rx_sample_count = 0x202;
    make_request(request, DP104_OP_COMMIT, 0x53);
    CHECK(send_status_request(request, DP104_BAD_STATE));

    /* F5 page zero freezes the 64-byte header and ring without a bulk copy.
     * F3 remains the real receiver snapshot and is fetched promptly after it. */
    CHECK(send_diagnostic_request(0x37, freeze_nonce, 0, response));
    CHECK(response[3] == DP104_OK && memcmp(response + 4, freeze_nonce, 4) == 0);
    CHECK(memcmp(response + 8, "P5RX", 4) == 0);
    CHECK(dp104_read16(response + 12) == 0 &&
          dp104_read16(response + 14) == DP104_PLC_DIAGNOSTIC_TOTAL_BYTES);
    CHECK(__dp104_plc_state.diagnostic.frozen);
    memcpy(blob, response + 16, DP104_PLC_DIAGNOSTIC_PAGE_BYTES);
    memcpy(records_before, __dp104_plc_state.diagnostic.records,
           sizeof(records_before));
    CHECK(send_receiver_snapshot(f3_nonce, 0x38, f3_response));
    CHECK(f3_response[0] == DP104_COMMAND &&
          f3_response[1] == RECEIVER_SNAPSHOT_OP && f3_response[2] == 0x38 &&
          f3_response[3] == DP104_OK);
    for(offset = DP104_PLC_DIAGNOSTIC_PAGE_BYTES;
        offset < DP104_PLC_DIAGNOSTIC_TOTAL_BYTES;
        offset = (uint16_t)(offset + DP104_PLC_DIAGNOSTIC_PAGE_BYTES))
    {
        CHECK(send_diagnostic_request(0x39, freeze_nonce, offset, response));
        CHECK(response[3] == DP104_OK &&
              dp104_read16(response + 12) == offset &&
              dp104_read16(response + 14) == DP104_PLC_DIAGNOSTIC_TOTAL_BYTES);
        memcpy(blob + offset, response + 16,
               DP104_PLC_DIAGNOSTIC_PAGE_BYTES);
    }
    CHECK(memcmp(__dp104_plc_state.diagnostic.records, records_before,
                 sizeof(records_before)) == 0);
    CHECK(memcmp(blob, "P5LG", 4) == 0);
    CHECK(dp104_read16(blob + 4) == 1 &&
          dp104_read16(blob + 6) == DP104_PLC_DIAGNOSTIC_HEADER_BYTES &&
          dp104_read16(blob + 8) == DP104_PLC_DIAGNOSTIC_TOTAL_BYTES &&
          dp104_read16(blob + 10) == DP104_PLC_DIAGNOSTIC_RECORD_BYTES &&
          dp104_read16(blob + 12) == DP104_PLC_DIAGNOSTIC_RECORD_CAPACITY);
    CHECK((dp104_read16(blob + 16) & 3u) == 3u);
    CHECK(dp104_read32(blob + 20) == 9u);
    /* The rejected callback is capacity+1: increasing 72 to 128 adds 56
     * observed bytes, while the historical 72-byte trace stays unchanged. */
    CHECK(dp104_read32(blob + 24) == 355u + DP104_PLC_RX_DMA_BYTES - PRE_FIX_RX_DMA_BYTES);
    CHECK(dp104_read32(blob + 28) == 2u &&
          dp104_read32(blob + 32) == 1u &&
          dp104_read32(blob + 36) == 3u &&
          dp104_read32(blob + 40) == 144u);
    CHECK(dp104_read32(blob + 44) == 2u);
    CHECK(dp104_read32(blob + 48) == UINT32_C(0x01005303));
    CHECK(dp104_read32(blob + 52) == UINT32_C(0x01000253));
    CHECK(dp104_read32(blob + 56) == 2u && dp104_read32(blob + 60) == 2u);
    record = find_diagnostic_record(blob, DP104_PLC_DIAG_RX_CANDIDATE,
                                    DP104_OP_DATA, 0x42);
    CHECK(record != NULL);
    if(record)
    {
        CHECK(dp104_read32(record + 0) == 4u);
        CHECK(dp104_read32(record + 4) == 0x102u);
        CHECK(dp104_read32(record + 8) == UINT32_C(0x55667788));
        CHECK(dp104_read32(record + 12) == 4u);
        CHECK(dp104_read16(record + 16) == 44u);
        CHECK(dp104_read16(record + 18) == 2u &&
              dp104_read16(record + 20) == 46u &&
              dp104_read16(record + 22) == 8u);
        CHECK((record[25] & DP104_PLC_DIAG_RECORD_AMBIGUOUS) != 0);
        CHECK(record[26] == DP104_COMMAND && record[27] == DP104_OP_DATA &&
              record[28] == 0x42 && record[29] == 8 && record[30] == 7 &&
              record[31] == DP104_WIRE_ENCODED_BYTES);
    }
    for(i = 0; i < dp104_read16(blob + 14); ++i)
    {
        const uint8_t *candidate = blob + DP104_PLC_DIAGNOSTIC_HEADER_BYTES +
            (size_t)i * DP104_PLC_DIAGNOSTIC_RECORD_BYTES;
        if(candidate[24] == DP104_PLC_DIAG_RX_CANDIDATE &&
           candidate[27] == DP104_OP_COMMIT && candidate[28] == 0x53)
        {
            if(dp104_read16(candidate + 16) == PRE_FIX_RX_DMA_BYTES &&
               candidate[31] == 39u)
                partial_commit_candidate = 1;
            if(candidate[31] == DP104_WIRE_ENCODED_BYTES)
                complete_commit_candidate = 1;
        }
    }
    CHECK(partial_commit_candidate && complete_commit_candidate);
    record = find_diagnostic_record(blob, DP104_PLC_DIAG_EXTENSION_CONSUMED,
                                    DP104_OP_DATA, 0x42);
    CHECK(record != NULL);
    if(record)
        CHECK(dp104_read16(record + 22) == DP104_WIRE_BYTES &&
              dp104_read16(record + 18) == 46u &&
              dp104_read16(record + 20) == 0u);
    record = find_diagnostic_record(blob, DP104_PLC_DIAG_COMMIT_RESULT,
                                    DP104_OP_COMMIT, 0x53);
    CHECK(record != NULL);
    if(record)
        CHECK(dp104_read16(record + 22) == 0u && record[29] == DP104_BAD_STATE);
    CHECK(find_diagnostic_record(blob, DP104_PLC_DIAG_RX_PUSH_FAILURE,
                                 0xffu, 0xffu) != NULL);
    CHECK(find_diagnostic_record(blob, DP104_PLC_DIAG_RX_DMA_FULL,
                                 0xffu, 0xffu) != NULL);
    CHECK(find_diagnostic_record(blob, DP104_PLC_DIAG_PARSER_DISCARD,
                                 0xffu, 0xffu) != NULL);

    /* Frozen state survives all later RX, F5 preflight/page reads, F3 and a
     * normal real CAPS transaction. Bad page nonce/alignment/range is rejected. */
    make_request(request, DP104_OP_CAPS, 0x70);
    CHECK(send_status_request(request, DP104_OK));
    dp104_plc_uart_rx(oversized, (uint32_t)sizeof(oversized));
    CHECK(send_diagnostic_request(0x3a, wrong_nonce, 16, response));
    CHECK(response[3] == DP104_BAD_REQUEST && check_empty_error_tail(response));
    CHECK(send_diagnostic_request(0x3b, freeze_nonce, 1, response));
    CHECK(response[3] == DP104_BAD_REQUEST && check_empty_error_tail(response));
    CHECK(send_diagnostic_request(0x3c, freeze_nonce,
                                  DP104_PLC_DIAGNOSTIC_TOTAL_BYTES, response));
    CHECK(response[3] == DP104_BAD_REQUEST && check_empty_error_tail(response));
    CHECK(send_diagnostic_request(0x3d, wrong_nonce,
                                  DP104_PLC_DIAGNOSTIC_PREFLIGHT, response));
    CHECK(response[3] == DP104_OK &&
          __dp104_plc_state.diagnostic.frozen);
    CHECK(send_diagnostic_request(0x3e, freeze_nonce,
                                  DP104_PLC_DIAGNOSTIC_HEADER_BYTES, response));
    CHECK(response[3] == DP104_OK &&
          memcmp(response + 16, blob + DP104_PLC_DIAGNOSTIC_HEADER_BYTES,
                 DP104_PLC_DIAGNOSTIC_PAGE_BYTES) == 0);
    CHECK(memcmp(__dp104_plc_state.diagnostic.records, records_before,
                 sizeof(records_before)) == 0);
    CHECK(check_irq_balance());

    /* The independent COMMIT marker survives ring overwrite by later DATA
     * traffic even when its event record is no longer present. */
    CHECK(reset_diagnostic_fixture());
    __dp104_plc_state.color_order[0] = 0;
    __dp104_plc_state.color_order[1] = 1;
    __dp104_plc_state.color_order[2] = 2;
    __dp104_plc_state.color_order_verified = 1;
    CHECK(send_diagnostic_request(0x40, arm_nonce,
                                  DP104_PLC_DIAGNOSTIC_ARM, response));
    for(i = 0; i < DP104_LED_COUNT; ++i)
    {
        rgb[i * 3u] = (uint8_t)i;
        rgb[i * 3u + 1u] = (uint8_t)(0x40u + i);
        rgb[i * 3u + 2u] = (uint8_t)(0xe0u - i);
    }
    make_request(request, DP104_OP_BEGIN, 0x60);
    dp104_write16(request + 4, DP104_LED_COUNT);
    dp104_write32(request + 8, dp104_crc32(rgb, DP104_FRAME_BYTES));
    CHECK(send_status_request(request, DP104_OK));
    for(i = 0; i < DP104_LED_COUNT; i += DP104_CHUNK_LEDS)
    {
        make_request(request, DP104_OP_DATA, 0x60);
        dp104_write16(request + 4, (uint16_t)i);
        request[6] = DP104_CHUNK_LEDS;
        memcpy(request + 7, rgb + i * 3u, DP104_CHUNK_LEDS * 3u);
        CHECK(send_status_request(request, DP104_OK));
    }
    make_request(request, DP104_OP_COMMIT, 0x60);
    CHECK(send_status_request(request, DP104_OK));
    CHECK(__dp104_plc_state.receiver.direct_active &&
          __dp104_plc_state.receiver.active_transaction == 0x60);
    CHECK(__dp104_plc_state.diagnostic.last_commit_word ==
          UINT32_C(0x01680060));
    for(i = 0; i < DP104_LED_COUNT; i += DP104_CHUNK_LEDS)
    {
        make_request(request, DP104_OP_DATA, 0x60);
        dp104_write16(request + 4, (uint16_t)i);
        request[6] = DP104_CHUNK_LEDS;
        memcpy(request + 7, rgb + i * 3u, DP104_CHUNK_LEDS * 3u);
        CHECK(send_status_request(request, DP104_BAD_STATE));
    }
    CHECK(__dp104_plc_state.diagnostic.record_count ==
          DP104_PLC_DIAGNOSTIC_RECORD_CAPACITY);
    CHECK(__dp104_plc_state.diagnostic.ring_wrapped);
    for(i = 0; i < DP104_PLC_DIAGNOSTIC_RECORD_CAPACITY; ++i)
        CHECK(__dp104_plc_state.diagnostic.records[i][24] !=
              DP104_PLC_DIAG_COMMIT_RESULT);
    CHECK(send_diagnostic_request(0x41, freeze_nonce, 0, response));
    CHECK(response[3] == DP104_OK);
    CHECK(dp104_read16(response + 14) == DP104_PLC_DIAGNOSTIC_TOTAL_BYTES);
    CHECK(dp104_read16(response + 30) == DP104_PLC_DIAGNOSTIC_RECORD_CAPACITY);
    CHECK(dp104_read16(response + 20) == 1u);
    CHECK(send_diagnostic_request(0x43, freeze_nonce, 16, response));
    CHECK(response[3] == DP104_OK &&
          (dp104_read16(response + 16) & 4u) != 0);
    CHECK(send_diagnostic_request(0x42, freeze_nonce, 48, response));
    CHECK(response[3] == DP104_OK && dp104_read16(response + 12) == 48u);
    CHECK(dp104_read32(response + 20) == UINT32_C(0x01680060));
    CHECK(check_irq_balance());
}

static void test_circular_rx_irq(void)
{
    uint8_t request[DP104_REPORT_BYTES], response[DP104_REPORT_BYTES];
    uint8_t burst[177], wire[DP104_WIRE_BYTES], expected[5][17];
    uint8_t *legacy;
    const uint8_t order[3] = {1, 0, 2}, tag[4] = {'U','S','E','R'};
    unsigned i, j, legacy_count = 0, responses, seen_status, seen_data;
    CHECK(reset_diagnostic_fixture());
    CHECK(queue_set_order(0x70, order, tag, DP104_OK));
    make_request(request, DP104_OP_BEGIN, 0x71);
    dp104_write16(request + 4, DP104_LED_COUNT);
    CHECK(send_status_request(request, DP104_OK));
    make_request(request, DP104_OP_DATA, 0x71);
    dp104_write16(request + 4, 16); request[6] = 8;
    memset(request + 7, 0x33, 24);
    CHECK(dp104_wire_encode(DP104_WIRE_REQUEST, request, sizeof(request), wire, sizeof(wire)));
    memcpy(burst, wire, sizeof(wire));
    for(i = 0; i < 5; ++i) {
        memset(expected[i], 0x80u + i, 17);
        expected[i][0] = 1; expected[i][1] = (uint8_t)(0x40u + i); expected[i][2] = 14;
        memcpy(burst + sizeof(wire) + i * 17u, expected[i], 17);
    }
    dp104_write16(request + 4, 24); memset(request + 7, 0x66, 24);
    CHECK(dp104_wire_encode(DP104_WIRE_REQUEST, request, sizeof(request), wire, sizeof(wire)));
    memcpy(burst + 131, wire, sizeof(wire));
    responses = tx_count;
    seen_status = fake_uart_status_reads; seen_data = fake_uart_data_reads;
    for(i = 0; i < sizeof(burst); ++i) {
        __dp104_plc_state.rx_dma_buffer[i % DP104_PLC_RX_DMA_BYTES] = burst[i];
        if((i + 1u) % 64u == 0u || i + 1u == sizeof(burst)) {
            fake_dma_remaining_value = DP104_PLC_RX_DMA_BYTES - ((i + 1u) % DP104_PLC_RX_DMA_BYTES);
            fake_dma_flags_value = (i + 1u) % 128u == 0u ? 0x00200000u :
                                   (i + 1u) % 64u == 0u ? 0x00400000u : 0;
            if(fake_dma_flags_value) dp104_plc_dma_irq();
            else dp104_plc_usart_irq();
            for(j = 0; j < 16; ++j) {
                unsigned before = __dp104_plc_state.uart_stream.used;
                legacy = 0; dp104_plc_queue_hook(&legacy);
                if(legacy) {
                    CHECK(legacy_count < 5 && legacy[DP104_PLC_QUEUE_DATA_BYTES] == 17);
                    if(legacy_count < 5) CHECK(memcmp(legacy, expected[legacy_count], 17) == 0);
                    ++legacy_count;
                }
                if(__dp104_plc_state.uart_stream.used == before) break;
            }
        }
    }
    CHECK(legacy_count == 5 && tx_count == responses + 2u);
    CHECK(__dp104_plc_state.receiver.received[2] == 0xff &&
          __dp104_plc_state.receiver.received[3] == 0xff);
    CHECK(__dp104_plc_state.uart_stream.used == 0);
    CHECK(fake_uart_status_reads == seen_status + 2u && fake_uart_data_reads == seen_data + 1u);
    CHECK(dp104_wire_decode(DP104_WIRE_RESPONSE, tx_frames[tx_count-1], tx_lengths[tx_count-1], response, sizeof(response)));
    CHECK(response[1] == DP104_OP_DATA && response[2] == 0x71 && response[3] == DP104_OK);
    /* Repeated IDLE must not duplicate buffered bytes or acknowledgements. */
    dp104_plc_usart_irq(); CHECK(__dp104_plc_state.uart_stream.used == 0);
    CHECK(tx_count == responses + 2u);
    fake_uart_control_value = 0;
    seen_status = fake_uart_status_reads; seen_data = fake_uart_data_reads;
    dp104_plc_usart_irq();
    CHECK(fake_uart_status_reads == seen_status && fake_uart_data_reads == seen_data);
    fake_uart_control_value = 0x10u;
    __dp104_plc_state.diagnostic.armed = 1;
    fake_dma_flags_value = 0x00600000u; fake_dma_remaining_value = 100;
    dp104_plc_dma_irq();
    CHECK(__dp104_plc_state.diagnostic.reserved0 & 2u);
    CHECK(__dp104_plc_state.uart_stream.used == 0 && __dp104_plc_state.uart_stream.dropping);
    CHECK(tx_count == responses + 2u && fake_dma_flags_value == 0);
    /* A fresh boot may receive bytes after BSS but before app entry. */
    memset(&__dp104_plc_state, 0, sizeof(__dp104_plc_state));
    __dp104_plc_state.uart_stream.used = 2;
    __dp104_plc_state.uart_stream.data[0] = 1;
    __dp104_plc_state.uart_stream.data[1] = 0x44;
    __dp104_plc_state.rx_dma_cursor.consumed = 28;
    CHECK(dp104_plc_target_init());
    CHECK(__dp104_plc_state.uart_stream.used == 2 &&
          __dp104_plc_state.uart_stream.data[1] == 0x44 &&
          __dp104_plc_state.rx_dma_cursor.consumed == 28);
    CHECK(__dp104_plc_state.diagnostic.reserved0 == 1u && fake_dma_irq_enables > 0);
}

static void test_rx128_mixed_burst(void)
{
    uint8_t request[DP104_REPORT_BYTES], response[DP104_REPORT_BYTES];
    uint8_t wire[DP104_WIRE_BYTES], batch[80];
    uint8_t first[17], last[17], full[DP104_PLC_RX_DMA_BYTES];
    uint8_t oversized[DP104_PLC_RX_DMA_BYTES + 1u];
    const uint8_t order[3] = {1, 0, 2};
    const uint8_t tag[4] = {'U', 'S', 'E', 'R'};
    uint8_t *legacy = 0;
    unsigned before, i;

    CHECK(reset_diagnostic_fixture());
    CHECK(DP104_PLC_RX_DMA_BYTES == 128u);
    CHECK(queue_set_order(0x60, order, tag, DP104_OK));
    make_request(request, DP104_OP_BEGIN, 0x61);
    dp104_write16(request + 4, DP104_LED_COUNT);
    CHECK(send_status_request(request, DP104_OK));
    make_request(request, DP104_OP_DATA, 0x61);
    dp104_write16(request + 4, 72);
    request[6] = 8;
    for(i = 0; i < 24; ++i) request[7 + i] = (uint8_t)(i + 11u);
    CHECK(dp104_wire_encode(DP104_WIRE_REQUEST, request, sizeof(request), wire, sizeof(wire)));
    memset(first, 0xa1, sizeof(first));
    memset(last, 0xb2, sizeof(last));
    first[0] = last[0] = 1;
    first[1] = 0x31; last[1] = 0x32;
    first[2] = last[2] = 14;
    memcpy(batch, first, 17);
    memcpy(batch + 17, wire, DP104_WIRE_BYTES);
    memcpy(batch + 17 + DP104_WIRE_BYTES, last, 17);
    CHECK(sizeof(batch) > PRE_FIX_RX_DMA_BYTES && sizeof(batch) <= DP104_PLC_RX_DMA_BYTES);
    memcpy(__dp104_plc_state.rx_dma_buffer, batch, sizeof(batch));
    __dp104_plc_state.rx_sr_before_dr = 0xd0;
    __dp104_plc_state.rx_cndtr_after_dr = DP104_PLC_RX_DMA_BYTES - sizeof(batch);
    __dp104_plc_state.rx_sample_count = 1;
    before = tx_count;
    dp104_plc_uart_rx(__dp104_plc_state.rx_dma_buffer, sizeof(batch));
    dp104_plc_queue_hook(&legacy);
    CHECK(legacy && legacy[DP104_PLC_QUEUE_DATA_BYTES] == 17 && memcmp(legacy, first, 17) == 0);
    CHECK(tx_count == before);
    legacy = 0;
    dp104_plc_queue_hook(&legacy);
    CHECK(legacy == 0 && tx_count == before + 1u);
    CHECK(dp104_wire_decode(DP104_WIRE_RESPONSE, tx_frames[before], tx_lengths[before], response, sizeof(response)));
    CHECK(response[1] == DP104_OP_DATA && response[2] == 0x61 && response[3] == DP104_OK);
    CHECK(__dp104_plc_state.receiver.received[72 / 8] == 0xff);
    CHECK(memcmp(__dp104_plc_state.receiver.staging + 72 * 3, request + 7, 24) == 0);
    dp104_plc_queue_hook(&legacy);
    CHECK(legacy && legacy[DP104_PLC_QUEUE_DATA_BYTES] == 17 && memcmp(legacy, last, 17) == 0);
    CHECK(__dp104_plc_state.uart_stream.used == 0);

    memset(full, 0xff, sizeof(full));
    __dp104_plc_state.diagnostic.armed = 1;
    __dp104_plc_state.rx_cndtr_after_dr = 0;
    __dp104_plc_state.rx_sample_count = 2;
    dp104_plc_uart_rx(full, sizeof(full));
    CHECK(__dp104_plc_state.uart_stream.used == DP104_PLC_RX_DMA_BYTES);
    CHECK(__dp104_plc_state.diagnostic.rx_dma_full_callbacks == 1);
    dp104_wire_stream_reset(&__dp104_plc_state.uart_stream);
    memset(oversized, 0xff, sizeof(oversized));
    __dp104_plc_state.rx_cndtr_after_dr = 7;
    dp104_plc_uart_rx(oversized, sizeof(oversized));
    CHECK(__dp104_plc_state.uart_stream.used == 0 && __dp104_plc_state.uart_stream.dropping);
    CHECK(__dp104_plc_state.diagnostic.rx_push_failures == 1);
    CHECK(check_irq_balance());
}

int main(void)
{
    uint8_t request[DP104_REPORT_BYTES], response[DP104_REPORT_BYTES];
    uint8_t legacy_batch[] = {1, 0x20, 2, 0xaa, 0xbb,
                              1, 0x30, 1, 0xcc, 0xff, 0x99};
    uint8_t expected_order[3] = {1, 0, 2};
    const uint8_t user_tag[4] = {'U', 'S', 'E', 'R'};
    const uint8_t bad_tag[4] = {'A', 'U', 'T', 'O'};
    const uint8_t app_nonce[16] = {
        0x91, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
        0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xf0, 0x01
    };
    const uint8_t snapshot_nonce_initial[4] = {0x51, 0x62, 0x73, 0x84};
    const uint8_t snapshot_nonce_partial[4] = {0xa1, 0xb2, 0xc3, 0xd4};
    const uint8_t snapshot_nonce_active[4] = {0xe1, 0xf2, 0x93, 0x84};
    uint8_t rgb[DP104_FRAME_BYTES];
    uint8_t partial_staging[DP104_FRAME_BYTES];
    uint8_t partial_received[(DP104_LED_COUNT + 7u) / 8u];
    uint8_t output_before[DP104_FRAME_BYTES];
    uint8_t bad_nonce[16] = {0};
    uint8_t f1_response[DP104_REPORT_BYTES];
    uint8_t f2_response[DP104_REPORT_BYTES];
    uint8_t f1_nonce[16];
    uint8_t master_stream_bytes[DP104_WIRE_BYTES * 2u];
    unsigned i, c;
    unsigned f1_tx_index, f2_tx_index, status_tx_index;
    size_t f1_wire_length, f2_wire_length, status_wire_length;
    size_t master_stream_length, split;
    unsigned before_saves, before_restores;
    uint8_t *legacy;
    uint32_t committed_before;
    uint8_t dirty_before;

    memset((void *)dp104_plc_test_key_buffer, 0, sizeof(dp104_plc_test_key_buffer));
    dp104_plc_test_dirty = 0;
    memset(__dp104_plc_state.rx_dma_buffer, 0xa7, sizeof(__dp104_plc_state.rx_dma_buffer));
    CHECK(dp104_plc_target_init());
    for(i = 0; i < sizeof(__dp104_plc_state.rx_dma_buffer); ++i)
        CHECK(__dp104_plc_state.rx_dma_buffer[i] == 0xa7);
    CHECK(__dp104_plc_state.initialized == DP104_PLC_TARGET_MAGIC);
    CHECK(__dp104_plc_state.color_order_verified == 0);
    CHECK(__dp104_plc_state.twiddle[0] == UINT32_C(0x3f800000));
    CHECK(__dp104_plc_state.twiddle[1] == 0);
    CHECK(__dp104_plc_state.twiddle[1024] == UINT32_C(0xbf800000));
    CHECK(__dp104_plc_state.twiddle[1025] == 0);
    CHECK(!dp104_plc_should_skip_setter());

    /* App info is available before calibration, but cannot claim readiness
     * or change the key buffer, setter ownership, or receiver lease state. */
    memcpy(output_before, (const void *)dp104_plc_test_key_buffer,
           sizeof(output_before));
    dirty_before = dp104_plc_test_dirty;
    committed_before = __dp104_plc_state.receiver.committed_at;
    CHECK(send_app_info(app_nonce, 0x42, response));
    CHECK(check_app_info(app_nonce, 0x42, 0, response));
    CHECK(memcmp(output_before, (const void *)dp104_plc_test_key_buffer,
                 sizeof(output_before)) == 0);
    CHECK(dp104_plc_test_dirty == dirty_before);
    CHECK(__dp104_plc_state.override_active == 0 &&
          __dp104_plc_state.diagnostic_active == 0);
    CHECK(__dp104_plc_state.receiver.committed_at == committed_before);

    /* F3 is available before calibration and only snapshots receiver state. */
    CHECK(send_receiver_snapshot(snapshot_nonce_initial, 0x50, response));
    CHECK(response[0] == DP104_COMMAND &&
          response[1] == RECEIVER_SNAPSHOT_OP && response[2] == 0x50 &&
          response[3] == DP104_OK);
    CHECK(memcmp(response + 4, snapshot_nonce_initial, 4) == 0);
    CHECK(response[8] == 0 && response[9] == 0 && response[10] == 0);
    for(i = 11; i < 24; ++i) CHECK(response[i] == 0);
    CHECK(dp104_read32(response + 24) == 0);
    CHECK(dp104_read32(response + 28) ==
          dp104_crc32(__dp104_plc_state.receiver.staging, DP104_FRAME_BYTES));
    CHECK(__dp104_plc_state.receiver.committed_at == committed_before &&
          __dp104_plc_state.receiver.staging_active == 0 &&
          __dp104_plc_state.receiver.direct_active == 0);
    CHECK(memcmp(output_before, (const void *)dp104_plc_test_key_buffer,
                 sizeof(output_before)) == 0);
    CHECK(dp104_plc_test_dirty == dirty_before);

    /* F3 rejects zero transaction, zero nonce, bad status, and reserved data. */
    make_request(request, RECEIVER_SNAPSHOT_OP, 0);
    memcpy(request + 4, snapshot_nonce_initial, 4);
    CHECK(send_request(request, response));
    CHECK(response[3] == DP104_BAD_REQUEST && check_empty_error_tail(response));
    make_request(request, RECEIVER_SNAPSHOT_OP, 0x51);
    CHECK(send_request(request, response));
    CHECK(response[3] == DP104_BAD_REQUEST && check_empty_error_tail(response));
    make_request(request, RECEIVER_SNAPSHOT_OP, 0x52);
    memcpy(request + 4, snapshot_nonce_initial, 4);
    request[8] = 1;
    CHECK(send_request(request, response));
    CHECK(response[3] == DP104_BAD_REQUEST && check_empty_error_tail(response));
    make_request(request, RECEIVER_SNAPSHOT_OP, 0x53);
    memcpy(request + 4, snapshot_nonce_initial, 4);
    request[3] = 1;
    CHECK(send_request(request, response));
    CHECK(response[3] == DP104_BAD_REQUEST && check_empty_error_tail(response));

    /* Malformed app-info requests get BAD_REQUEST with an empty payload. */
    make_request(request, APP_INFO_OP, 0x43);
    CHECK(send_request(request, response));
    CHECK(response[3] == DP104_BAD_REQUEST && check_empty_error_tail(response));
    make_request(request, APP_INFO_OP, 0x44);
    request[3] = 1;
    memcpy(request + 4, app_nonce, sizeof(app_nonce));
    CHECK(send_request(request, response));
    CHECK(response[3] == DP104_BAD_REQUEST && check_empty_error_tail(response));
    make_request(request, APP_INFO_OP, 0x45);
    memcpy(request + 4, bad_nonce, sizeof(bad_nonce));
    CHECK(send_request(request, response));
    CHECK(response[3] == DP104_BAD_REQUEST && check_empty_error_tail(response));
    make_request(request, APP_INFO_OP, 0x46);
    memcpy(request + 4, app_nonce, sizeof(app_nonce));
    request[20] = 1;
    CHECK(send_request(request, response));
    CHECK(response[3] == DP104_BAD_REQUEST && check_empty_error_tail(response));

    /* The worker's popped slot is overwritten as soon as IRQs are restored. */
    before_saves = irq_saves;
    before_restores = irq_restores;
    enqueue_bytes(legacy_batch, sizeof(legacy_batch), 1);
    legacy = 0;
    dp104_plc_queue_hook(&legacy);
    CHECK(legacy == __dp104_plc_state.legacy_slot);
    CHECK(legacy && legacy[DP104_PLC_QUEUE_DATA_BYTES] == sizeof(legacy_batch));
    CHECK(legacy && memcmp(legacy, legacy_batch, sizeof(legacy_batch)) == 0);
    CHECK(fake_queue_slot[0] == 0xee);
    CHECK(irq_saves == before_saves + 1 && irq_restores == before_restores + 1);

    /* A valid CAPS is BUSY until the caller explicitly verifies channel order. */
    make_request(request, DP104_OP_CAPS, 0); /* Core protocol allows txn zero here. */
    CHECK(send_request(request, response));
    CHECK(response[3] == DP104_BUSY && response[4] == 0);
    CHECK(memcmp(response + 4, DP104_CAPS_MAGIC, 8) != 0);

    /* Malformed CAPS keeps the existing receiver's validation/ready priority. */
    request[4] = 1;
    CHECK(send_request(request, response));
    CHECK(response[3] == DP104_UNSUPPORTED);

    /* Raw pulse uses an unverified raw channel and expires across millis wrap. */
    fake_ms = UINT32_C(0xfffffff0);
    CHECK(queue_pulse(1, 1, 64, DP104_OK));
    CHECK(__dp104_plc_state.override_active == 1);
    CHECK(__dp104_plc_state.diagnostic_active == 1);
    CHECK(__dp104_plc_state.color_order_verified == 0);
    CHECK(__dp104_plc_state.diagnostic_started == UINT32_C(0xfffffff0));
    CHECK(dp104_plc_should_skip_setter());
    for(i = 0; i < DP104_LED_COUNT; ++i)
        CHECK(dp104_plc_test_key_buffer[i * 3u] == 0 &&
              dp104_plc_test_key_buffer[i * 3u + 1u] == 64 &&
              dp104_plc_test_key_buffer[i * 3u + 2u] == 0);

    fake_ms = UINT32_C(0x000003d7); /* 999 ms after 0xfffffff0. */
    CHECK(send_empty_queue());
    CHECK(__dp104_plc_state.override_active == 1 &&
          __dp104_plc_state.diagnostic_active == 1);
    dp104_plc_test_dirty = 0;
    fake_ms = UINT32_C(0x000003d8); /* Exact 1000 ms timeout. */
    CHECK(send_empty_queue());
    CHECK(__dp104_plc_state.override_active == 0 &&
          __dp104_plc_state.diagnostic_active == 0);
    CHECK(dp104_plc_test_dirty == 1 && !dp104_plc_should_skip_setter());

    CHECK(queue_set_order(2, (const uint8_t[3]){0, 0, 2}, user_tag,
                          DP104_BAD_REQUEST));
    CHECK(queue_set_order(3, expected_order, bad_tag, DP104_BAD_REQUEST));
    CHECK(__dp104_plc_state.color_order_verified == 0);
    make_request(request, DP104_PLC_CAL_SET_ORDER, 4);
    memcpy(request + 4, expected_order, 3);
    memcpy(request + 7, user_tag, 4);
    CHECK(send_status_request(request, DP104_BAD_REQUEST)); /* Missing nonce. */
    memset(request + 11, 0x41, 16);
    request[31] = 1;
    CHECK(send_status_request(request, DP104_BAD_REQUEST)); /* Nonzero tail. */
    CHECK(__dp104_plc_state.color_order_verified == 0);
    CHECK(queue_set_order(4, expected_order, user_tag, DP104_OK));
    CHECK(__dp104_plc_state.color_order_verified == 1);
    CHECK(memcmp(__dp104_plc_state.color_order, expected_order, 3) == 0);
    CHECK(__dp104_plc_state.override_active == 0 &&
          __dp104_plc_state.diagnostic_active == 0);
    memcpy(output_before, (const void *)dp104_plc_test_key_buffer,
           sizeof(output_before));
    dirty_before = dp104_plc_test_dirty;
    CHECK(send_app_info(app_nonce, 0x47, response));
    CHECK(check_app_info(app_nonce, 0x47, 1, response));
    CHECK(memcmp(output_before, (const void *)dp104_plc_test_key_buffer,
                 sizeof(output_before)) == 0);
    CHECK(dp104_plc_test_dirty == dirty_before);

    /* The fixed wire envelope keeps this GRB F1 reply at 46 bytes regardless
     * of nonce contents, with no tail padding before the following F2. */
    make_request(request, DP104_PLC_CAL_SET_ORDER, 0xe1);
    memcpy(request + 4, expected_order, 3);
    memcpy(request + 7, user_tag, 4);
    for(i = 0; i < sizeof(f1_nonce); ++i)
        f1_nonce[i] = (uint8_t)(0x20u + i);
    f1_nonce[0] = 1;
    memcpy(request + 11, f1_nonce, sizeof(f1_nonce));
    f1_tx_index = tx_count;
    CHECK(send_request(request, f1_response));
    CHECK(f1_response[3] == DP104_OK &&
          memcmp(f1_response + 4, expected_order, 3) == 0 &&
          memcmp(f1_response + 7, f1_nonce, 16) == 0);
    f1_wire_length = accepted_uart_extension_size(tx_frames[f1_tx_index],
                                                   tx_lengths[f1_tx_index]);
    CHECK(f1_wire_length == DP104_WIRE_BYTES);
    CHECK(tx_lengths[f1_tx_index] == DP104_WIRE_BYTES);

    f2_tx_index = tx_count;
    CHECK(send_app_info(app_nonce, 0x49, f2_response));
    CHECK(check_app_info(app_nonce, 0x49, 1, f2_response));
    f2_wire_length = accepted_uart_extension_size(tx_frames[f2_tx_index],
                                                   tx_lengths[f2_tx_index]);
    CHECK(f2_wire_length != 0 && tx_lengths[f2_tx_index] == f2_wire_length);

    /* Exercise the master stream parser at every possible split point across
     * adjacent fixed F1/F2 envelopes. No frame may leak as legacy. */
    master_stream_length = tx_lengths[f1_tx_index] + tx_lengths[f2_tx_index];
    CHECK(master_stream_length <= sizeof(master_stream_bytes));
    memcpy(master_stream_bytes, tx_frames[f1_tx_index], tx_lengths[f1_tx_index]);
    memcpy(master_stream_bytes + tx_lengths[f1_tx_index],
           tx_frames[f2_tx_index], tx_lengths[f2_tx_index]);
    for(split = 0; split <= master_stream_length; ++split)
    {
        dp104_wire_stream stream;
        master_stream_capture capture;
        memset(&capture, 0, sizeof(capture));
        dp104_wire_stream_init(&stream);
        CHECK(dp104_wire_stream_push(&stream, master_stream_bytes, split));
        CHECK(drain_master_stream(&stream, &capture));
        CHECK(dp104_wire_stream_push(&stream, master_stream_bytes + split,
                                     master_stream_length - split));
        CHECK(drain_master_stream(&stream, &capture));
        CHECK(capture.count == 2 && capture.legacy_count == 0);
        CHECK(memcmp(capture.responses[0], f1_response,
                     DP104_REPORT_BYTES) == 0);
        CHECK(memcmp(capture.responses[1], f2_response,
                     DP104_REPORT_BYTES) == 0);
        CHECK(stream.used == 0);
    }

    CHECK(queue_set_order(5, expected_order, user_tag, DP104_OK)); /* idempotent */
    CHECK(queue_set_order(6, (const uint8_t[3]){0, 1, 2}, user_tag,
                          DP104_BAD_STATE));
    CHECK(memcmp(__dp104_plc_state.color_order, expected_order, 3) == 0);
    CHECK(queue_pulse(7, 0, 20, DP104_BAD_STATE)); /* pulses stop after verification */

    make_request(request, DP104_OP_CAPS, 0);
    CHECK(send_request(request, response));
    CHECK(response[3] == DP104_OK &&
          memcmp(response + 4, DP104_CAPS_MAGIC, 8) == 0);
    CHECK(response[17] == 1 && dp104_read16(response + 14) == DP104_LED_COUNT);

    /* Full frame goes through the production receiver, CRC, wire ACK, and mapping. */
    fake_ms = 0x1000;
    for(i = 0; i < DP104_LED_COUNT; ++i)
    {
        rgb[i * 3u] = (uint8_t)i;
        rgb[i * 3u + 1u] = (uint8_t)(100u + i);
        rgb[i * 3u + 2u] = (uint8_t)(255u - i);
    }
    make_request(request, DP104_OP_BEGIN, 77);
    dp104_write16(request + 4, DP104_LED_COUNT);
    dp104_write32(request + 8, dp104_crc32(rgb, DP104_FRAME_BYTES));
    CHECK(send_status_request(request, DP104_OK));
    memset(partial_staging, 0, sizeof(partial_staging));
    memset(partial_received, 0, sizeof(partial_received));

    for(i = 0; i < DP104_LED_COUNT; i += DP104_CHUNK_LEDS)
    {
        const unsigned count = DP104_LED_COUNT - i < DP104_CHUNK_LEDS ?
                               DP104_LED_COUNT - i : DP104_CHUNK_LEDS;
        make_request(request, DP104_OP_DATA, 77);
        dp104_write16(request + 4, (uint16_t)i);
        request[6] = (uint8_t)count;
        memcpy(request + 7, rgb + i * 3u, count * 3u);
        CHECK(send_status_request(request, DP104_OK));
        if(i == DP104_CHUNK_LEDS)
        {
            memcpy(partial_staging, rgb,
                   (i + count) * 3u);
            partial_received[0] = 0xff;
            partial_received[1] = 0xff;
            memcpy(output_before, (const void *)dp104_plc_test_key_buffer,
                   sizeof(output_before));
            dirty_before = dp104_plc_test_dirty;
            committed_before = __dp104_plc_state.receiver.committed_at;
            CHECK(send_receiver_snapshot(snapshot_nonce_partial, 0x54,
                                         response));
            CHECK(response[0] == DP104_COMMAND &&
                  response[1] == RECEIVER_SNAPSHOT_OP &&
                  response[2] == 0x54 && response[3] == DP104_OK);
            CHECK(memcmp(response + 4, snapshot_nonce_partial, 4) == 0);
            CHECK(response[8] == 2 && response[9] == 77 && response[10] == 0);
            CHECK(memcmp(response + 11, partial_received,
                         sizeof(partial_received)) == 0);
            CHECK(dp104_read32(response + 24) ==
                  dp104_crc32(rgb, DP104_FRAME_BYTES));
            CHECK(dp104_read32(response + 28) ==
                  dp104_crc32(partial_staging, DP104_FRAME_BYTES));
            CHECK(__dp104_plc_state.receiver.staging_active == 1 &&
                  __dp104_plc_state.receiver.direct_active == 0 &&
                  __dp104_plc_state.receiver.transaction == 77 &&
                  __dp104_plc_state.receiver.began_at == fake_ms &&
                  __dp104_plc_state.receiver.committed_at == committed_before);
            CHECK(memcmp(__dp104_plc_state.receiver.staging, partial_staging,
                         sizeof(partial_staging)) == 0);
            CHECK(memcmp(__dp104_plc_state.receiver.received, partial_received,
                         sizeof(partial_received)) == 0);
            CHECK(memcmp(output_before,
                         (const void *)dp104_plc_test_key_buffer,
                         sizeof(output_before)) == 0);
            CHECK(dp104_plc_test_dirty == dirty_before);
        }
    }
    make_request(request, DP104_OP_COMMIT, 77);
    CHECK(send_status_request(request, DP104_OK));
    CHECK(__dp104_plc_state.receiver.direct_active == 1);
    CHECK(__dp104_plc_state.receiver.active_transaction == 77);
    CHECK(__dp104_plc_state.override_active == 1 && dp104_plc_should_skip_setter());
    make_request(request, DP104_OP_STATUS, 0xa3);
    status_tx_index = tx_count;
    CHECK(send_request(request, response));
    CHECK(response[3] == DP104_OK && response[4] == 1 &&
          response[5] == 0 && response[6] == 77);
    status_wire_length = accepted_uart_extension_size(
        tx_frames[status_tx_index], tx_lengths[status_tx_index]);
    CHECK(status_wire_length == DP104_WIRE_BYTES);
    CHECK(tx_lengths[status_tx_index] == DP104_WIRE_BYTES);
    committed_before = __dp104_plc_state.receiver.committed_at;
    memcpy(output_before, (const void *)dp104_plc_test_key_buffer,
           sizeof(output_before));
    fake_ms = 0x1000u + 400u;
    CHECK(send_app_info(app_nonce, 0x48, response));
    CHECK(check_app_info(app_nonce, 0x48, 1, response));
    CHECK(__dp104_plc_state.receiver.committed_at == committed_before);
    CHECK(__dp104_plc_state.receiver.direct_active == 1 &&
          __dp104_plc_state.override_active == 1);
    CHECK(memcmp(output_before, (const void *)dp104_plc_test_key_buffer,
                 sizeof(output_before)) == 0);
    for(i = 0; i < DP104_LED_COUNT; ++i)
        for(c = 0; c < 3; ++c)
            CHECK(dp104_plc_test_key_buffer[i * 3u + expected_order[c]] ==
                  rgb[i * 3u + c]);

    dirty_before = dp104_plc_test_dirty;
    CHECK(send_receiver_snapshot(snapshot_nonce_active, 0x55, response));
    CHECK(response[0] == DP104_COMMAND &&
          response[1] == RECEIVER_SNAPSHOT_OP && response[2] == 0x55 &&
          response[3] == DP104_OK);
    CHECK(memcmp(response + 4, snapshot_nonce_active, 4) == 0);
    CHECK(response[8] == 1 && response[9] == 0 && response[10] == 77);
    for(i = 0; i < sizeof(partial_received); ++i)
        CHECK(response[11u + i] == 0xff);
    CHECK(dp104_read32(response + 24) == dp104_crc32(rgb, DP104_FRAME_BYTES));
    CHECK(dp104_read32(response + 28) == dp104_crc32(rgb, DP104_FRAME_BYTES));
    CHECK(__dp104_plc_state.receiver.direct_active == 1 &&
          __dp104_plc_state.receiver.staging_active == 0 &&
          __dp104_plc_state.receiver.active_transaction == 77 &&
          __dp104_plc_state.receiver.committed_at == committed_before);
    CHECK(memcmp(output_before, (const void *)dp104_plc_test_key_buffer,
                 sizeof(output_before)) == 0);
    CHECK(dp104_plc_test_dirty == dirty_before);

    fake_ms = 0x1000u + DP104_LEASE_MS - 1u;
    CHECK(send_empty_queue());
    CHECK(__dp104_plc_state.receiver.direct_active == 1 &&
          __dp104_plc_state.override_active == 1);
    fake_ms += 1u;
    CHECK(send_empty_queue());
    CHECK(__dp104_plc_state.receiver.direct_active == 0 &&
          __dp104_plc_state.override_active == 0);
    CHECK(!dp104_plc_should_skip_setter());

    test_plc_diagnostic();
    test_rx128_mixed_burst();
    test_circular_rx_irq();

    CHECK(queue_pops > 0);
    CHECK(check_irq_balance());
    for(i = 0; i < tx_count; ++i)
    {
        const size_t extension_length =
            accepted_uart_extension_size(tx_frames[i], tx_lengths[i]);
        CHECK(extension_length == DP104_WIRE_BYTES &&
              tx_lengths[i] <= DP104_WIRE_BYTES && extension_length != 0 &&
              tx_modes[i] == 1 && tx_frames[i][0] == DP104_WIRE_RESPONSE &&
              tx_frames[i][1] == DP104_WIRE_TYPE &&
              tx_frames[i][2] == extension_length - DP104_WIRE_HEADER_BYTES);
    }
    if(failures)
    {
        fprintf(stderr, "plc target binding: %d checks failed\n", failures);
        return 1;
    }
    puts("plc target binding: state init, queue lifetime, calibration, CAPS gate, 104 RGB publish, ACK, lease, setter guard and IRQ balance PASS (synthetic twiddle basis)");
    return 0;
}
