#include "dp104_master_target.h"

#include "dp104_patch_state.h"
#include "dp104_wire.h"
#include "dp104_wire_stream.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if(!(condition)) { \
        fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
        return 1; \
    } \
} while(0)

dp104_master_patch_state __dp104_master_state;

volatile uint32_t dp104_master_test_csr800;
volatile uint8_t dp104_master_test_usb_active[5];
volatile uint32_t dp104_master_test_usb_remaining[5];
uint8_t dp104_master_test_report[DP104_REPORT_BYTES];
unsigned dp104_master_test_irq_enters;
unsigned dp104_master_test_irq_leaves;
int dp104_master_test_irq_depth;
int dp104_master_test_irq_fault;

static uint8_t uart_output[DP104_WIRE_BYTES];
static unsigned uart_output_length;
static unsigned uart_full_polls;
static unsigned uart_status_outside_critical;
static unsigned uart_status_inside_critical;
static unsigned uart_write_inside_critical;
static unsigned uart_d1_injected;
static unsigned uart_d1_inject_after_writes;
static int uart_d1_inject_enabled;

static unsigned usb_in_writes;
static uintptr_t usb_in_port;
static const uint8_t *usb_retained_pointer;
static uint8_t usb_retained_copy[DP104_REPORT_BYTES];
static unsigned usb_out_rearms;
static uintptr_t usb_out_port;
static const uint8_t *usb_out_pointer;
static unsigned usb_out_length;
static int usb_out_copy_visible;
static int usb_d1_before_stage;
static int usb_d1_make_endpoint_free;

static unsigned legacy_calls;
static const uint8_t *legacy_last_pointer;
static unsigned legacy_last_length;
static uint8_t legacy_copy[DP104_WIRE_STREAM_CAPACITY];
static uint8_t legacy_first_bytes[16];

static int inject_d1(uintptr_t port)
{
    memset(dp104_master_test_report, 0, sizeof(dp104_master_test_report));
    dp104_master_test_report[0] = 0xd1;
    ++uart_d1_injected;
    return dp104_master_host_intercept((uintptr_t)0xdeadbeefu, port,
                                       DP104_REPORT_BYTES);
}

uint8_t dp104_master_test_uart_status(void)
{
    if(dp104_master_test_irq_depth == 0)
    {
        ++uart_status_outside_critical;
        if(uart_d1_inject_enabled &&
           uart_output_length >= uart_d1_inject_after_writes)
        {
            uart_d1_inject_enabled = 0;
            (void)inject_d1(3u);
        }
    }
    else
        ++uart_status_inside_critical;

    if(uart_full_polls != 0)
    {
        --uart_full_polls;
        return 8u;
    }
    return 0u;
}

void dp104_master_test_uart_write(uint8_t byte)
{
    if(dp104_master_test_irq_depth != 1)
        ++uart_write_inside_critical;
    if(uart_output_length < sizeof(uart_output))
        uart_output[uart_output_length++] = byte;
}

void dp104_master_test_usb_in_writer(uintptr_t port, const uint8_t *bytes,
                                     unsigned length)
{
    uintptr_t endpoint = port & (uintptr_t)0x7f;
    if(bytes == NULL || length != DP104_REPORT_BYTES || endpoint == 0 ||
       endpoint > 4)
        return;
    usb_in_port = port;
    usb_retained_pointer = bytes;
    memcpy(usb_retained_copy, bytes, length);
    dp104_master_test_usb_active[endpoint] = 1;
    dp104_master_test_usb_remaining[endpoint] = 1;
    ++usb_in_writes;
}

void dp104_master_test_usb_out_rearm(uintptr_t port, const uint8_t *bytes,
                                     unsigned length)
{
    usb_out_port = port;
    usb_out_pointer = bytes;
    usb_out_length = length;
    usb_out_copy_visible =
        __dp104_master_state.bridge.request_ready &&
        memcmp(__dp104_master_state.bridge.request, dp104_master_test_report,
               DP104_REPORT_BYTES) == 0;
    ++usb_out_rearms;
}

void dp104_master_test_before_usb_stage(void)
{
    if(usb_d1_before_stage)
    {
        usb_d1_before_stage = 0;
        (void)inject_d1(3u);
        if(usb_d1_make_endpoint_free)
        {
            dp104_master_test_usb_active[3] = 1;
            dp104_master_test_usb_remaining[3] = 0;
        }
    }
}

void dp104_master_test_legacy_uart_rx(const uint8_t *bytes, unsigned length)
{
    legacy_last_pointer = bytes;
    legacy_last_length = length;
    if(length <= sizeof(legacy_copy))
        memcpy(legacy_copy, bytes, length);
    if(legacy_calls < sizeof(legacy_first_bytes))
        legacy_first_bytes[legacy_calls] = length ? bytes[0] : 0;
    ++legacy_calls;
}

static void reset_fixture(void)
{
    memset(&__dp104_master_state, 0, sizeof(__dp104_master_state));
    memset((void *)dp104_master_test_usb_active, 0,
           sizeof(dp104_master_test_usb_active));
    memset((void *)dp104_master_test_usb_remaining, 0,
           sizeof(dp104_master_test_usb_remaining));
    memset(dp104_master_test_report, 0, sizeof(dp104_master_test_report));
    memset(uart_output, 0, sizeof(uart_output));
    memset(usb_retained_copy, 0, sizeof(usb_retained_copy));
    memset(legacy_copy, 0, sizeof(legacy_copy));
    memset(legacy_first_bytes, 0, sizeof(legacy_first_bytes));
    dp104_master_test_csr800 = 0;
    dp104_master_test_irq_enters = 0;
    dp104_master_test_irq_leaves = 0;
    dp104_master_test_irq_depth = 0;
    dp104_master_test_irq_fault = 0;
    uart_output_length = 0;
    uart_full_polls = 0;
    uart_status_outside_critical = 0;
    uart_status_inside_critical = 0;
    uart_write_inside_critical = 0;
    uart_d1_injected = 0;
    uart_d1_inject_after_writes = 0;
    uart_d1_inject_enabled = 0;
    usb_in_writes = 0;
    usb_in_port = 0;
    usb_retained_pointer = NULL;
    usb_out_rearms = 0;
    usb_out_port = 0;
    usb_out_pointer = NULL;
    usb_out_length = 0;
    usb_out_copy_visible = 0;
    usb_d1_before_stage = 0;
    usb_d1_make_endpoint_free = 0;
    legacy_calls = 0;
    legacy_last_pointer = NULL;
    legacy_last_length = 0;
}

static void make_report(uint8_t report[DP104_REPORT_BYTES], uint8_t op,
                        uint8_t transaction, uint8_t fill)
{
    unsigned i;
    memset(report, 0, DP104_REPORT_BYTES);
    report[0] = DP104_COMMAND;
    report[1] = op;
    report[2] = transaction;
    for(i = 4; i < DP104_REPORT_BYTES; ++i)
        report[i] = (uint8_t)(fill + i);
}

static int host_report(const uint8_t report[DP104_REPORT_BYTES], uintptr_t a0,
                       uintptr_t port, unsigned length)
{
    memcpy(dp104_master_test_report, report, DP104_REPORT_BYTES);
    return dp104_master_host_intercept(a0, port, length);
}

static int make_wire(uint8_t direction,
                     const uint8_t report[DP104_REPORT_BYTES],
                     uint8_t frame[DP104_WIRE_BYTES], size_t *length)
{
    if(!dp104_wire_encode(direction, report, DP104_REPORT_BYTES, frame,
                          DP104_WIRE_BYTES))
        return 0;
    *length = dp104_wire_frame_size(frame, DP104_WIRE_BYTES);
    return *length == DP104_WIRE_BYTES &&
           frame[2] == DP104_WIRE_ENCODED_BYTES;
}

static int assert_irq_balanced(void)
{
    CHECK(dp104_master_test_irq_enters == dp104_master_test_irq_leaves);
    CHECK(dp104_master_test_irq_depth == 0);
    CHECK(!dp104_master_test_irq_fault);
    return 0;
}

static int test_host_abi_out_rearm_and_no_local_ack(void)
{
    uint8_t stock[DP104_REPORT_BYTES] = {0};
    uint8_t request[DP104_REPORT_BYTES];
    uint8_t expected[DP104_WIRE_BYTES];
    size_t expected_length;

    reset_fixture();
    stock[0] = 0x44;
    memcpy(dp104_master_test_report, stock, sizeof(stock));
    CHECK(dp104_master_host_intercept(0, 3u, DP104_REPORT_BYTES) == 0);
    CHECK(__dp104_master_state.initialized == 0);
    CHECK(usb_out_rearms == 0 && usb_in_writes == 0);

    make_report(request, DP104_OP_CAPS, 0x31, 0x20);
    CHECK(host_report(request, 0, 3u, DP104_REPORT_BYTES) == 1);
    CHECK(__dp104_master_state.initialized != 0);
    CHECK(__dp104_master_state.uart_stream_active != 0);
    CHECK(__dp104_master_state.bridge.request_ready);
    CHECK(memcmp(__dp104_master_state.bridge.request, request,
                 sizeof(request)) == 0);
    CHECK(usb_out_rearms == 1 && usb_out_pointer == dp104_master_test_report);
    CHECK(usb_out_port == 3u && usb_out_length == DP104_REPORT_BYTES);
    CHECK(usb_out_copy_visible);
    CHECK(uart_output_length == 0 && usb_in_writes == 0);

    uart_full_polls = 2;
    dp104_master_task_pump();
    CHECK(make_wire(DP104_WIRE_REQUEST, request, expected,
                    &expected_length));
    CHECK(uart_output_length == expected_length);
    CHECK(memcmp(uart_output, expected, expected_length) == 0);
    CHECK(__dp104_master_state.bridge.waiting);
    CHECK(usb_in_writes == 0); /* No PLC response, no fabricated CAPS ACK. */
    CHECK(uart_status_outside_critical >= 2);
    CHECK(__dp104_master_state.last_host_frame_word == UINT32_C(0x00ff3100));
    CHECK(__dp104_master_state.last_tx_frame_word == UINT32_C(0xeeff3100));
    CHECK(uart_write_inside_critical == 0);
    CHECK(assert_irq_balanced() == 0);
    return 0;
}

static int test_host_ignores_a0_and_filters_ports(void)
{
    uint8_t request[DP104_REPORT_BYTES];

    reset_fixture();
    make_report(request, DP104_OP_STATUS, 0x32, 0x20);
    memcpy(dp104_master_test_report, request, sizeof(request));

    /* IN-completion port and invalid endpoints must not inspect stale a0/data. */
    CHECK(dp104_master_host_intercept((uintptr_t)0xdeadbeefu, 0x83u,
                                      DP104_REPORT_BYTES) == 0);
    CHECK(dp104_master_host_intercept(0, 0, DP104_REPORT_BYTES) == 0);
    CHECK(dp104_master_host_intercept(0, 5u, DP104_REPORT_BYTES) == 0);
    CHECK(__dp104_master_state.initialized == 0);
    CHECK(usb_out_rearms == 0);

    CHECK(dp104_master_host_intercept((uintptr_t)0xdeadbeefu, 4u,
                                      DP104_REPORT_BYTES) == 1);
    CHECK(__dp104_master_state.bridge.request_ready);
    CHECK(__dp104_master_state.bridge.port == 4u);
    CHECK(memcmp(__dp104_master_state.bridge.request, request,
                 sizeof(request)) == 0);
    CHECK(usb_out_rearms == 1 && usb_out_port == 4u);
    CHECK(usb_out_pointer == dp104_master_test_report);
    CHECK(usb_out_copy_visible);
    CHECK(assert_irq_balanced() == 0);
    return 0;
}

static int test_short_d2_and_stock_passthrough(void)
{
    uint8_t short_d2[DP104_REPORT_BYTES] = {DP104_COMMAND};
    uint8_t early[5] = {0x91, 0x92, 0x93, 0x94, 0x95};

    reset_fixture();
    CHECK(host_report(short_d2, 0, 3u, 1u) == 1);
    CHECK(__dp104_master_state.initialized != 0);
    CHECK(!__dp104_master_state.bridge.request_ready);
    CHECK(usb_out_rearms == 1 && usb_out_length == DP104_REPORT_BYTES);
    CHECK(usb_out_pointer == dp104_master_test_report);
    CHECK(uart_output_length == 0 && usb_in_writes == 0);
    CHECK(dp104_master_host_intercept(0xdeadbeefu, 3u, 0) == 0);
    CHECK(usb_out_rearms == 1);

    reset_fixture();
    dp104_master_uart_rx(early, (unsigned)sizeof(early));
    CHECK(legacy_calls == 1);
    CHECK(legacy_last_pointer == early);
    CHECK(legacy_last_length == sizeof(early));
    CHECK(memcmp(legacy_copy, early, sizeof(early)) == 0);
    CHECK(usb_out_rearms == 0);
    return 0;
}

static int test_fragmented_uart_stream_and_legacy_order(void)
{
    const uintptr_t port = 3u;
    uint8_t request[DP104_REPORT_BYTES];
    uint8_t reply[DP104_REPORT_BYTES];
    uint8_t frame[DP104_WIRE_BYTES];
    uint8_t combined[DP104_WIRE_BYTES + 5];
    uint8_t legacy_a[4] = {0, 0x22, 1, 0xa1};
    uint8_t legacy_b[5] = {1, 0x33, 2, 0xb1, 0xb2};
    size_t frame_length;

    reset_fixture();
    make_report(request, DP104_OP_CAPS, 0x54, 0x10);
    CHECK(host_report(request, 0, port, DP104_REPORT_BYTES) == 1);
    dp104_master_task_pump();
    make_report(reply, DP104_OP_CAPS, 0x54, 0xd0);
    CHECK(make_wire(DP104_WIRE_RESPONSE, reply, frame, &frame_length));

    dp104_master_uart_rx(legacy_a, (unsigned)sizeof(legacy_a));
    CHECK(legacy_calls == 1 && legacy_last_length == sizeof(legacy_a));
    CHECK(memcmp(legacy_copy, legacy_a, sizeof(legacy_a)) == 0);
    CHECK(legacy_first_bytes[0] == legacy_a[0]);

    dp104_master_uart_rx(frame, 2);
    CHECK(legacy_calls == 1);
    CHECK(!__dp104_master_state.bridge.response_ready);

    memcpy(combined, frame + 2, frame_length - 2);
    memcpy(combined + frame_length - 2, legacy_b, sizeof(legacy_b));
    dp104_master_uart_rx(combined,
                         (unsigned)(frame_length - 2 + sizeof(legacy_b)));
    CHECK(__dp104_master_state.bridge.response_ready);
    CHECK(legacy_calls == 2);
    CHECK(legacy_last_length == sizeof(legacy_b));
    CHECK(memcmp(legacy_copy, legacy_b, sizeof(legacy_b)) == 0);
    CHECK(legacy_first_bytes[1] == legacy_b[0]);
    CHECK(usb_in_writes == 0); /* UART ISR only copies/routes; task sends. */

    dp104_master_test_usb_active[port] = 1;
    dp104_master_test_usb_remaining[port] = 0;
    dp104_master_task_pump();
    CHECK(usb_in_writes == 1 && usb_in_port == port);
    CHECK(memcmp(usb_retained_copy, reply, sizeof(reply)) == 0);
    CHECK(assert_irq_balanced() == 0);
    return 0;
}

static int test_stream_overflow_and_d1_reset(void)
{
    uint8_t request[DP104_REPORT_BYTES];
    uint8_t oversize[DP104_WIRE_STREAM_CAPACITY + 1];
    uint8_t updater_bytes[4] = {2, 0x10, 1, 0x77};
    uint32_t generation;

    reset_fixture();
    make_report(request, DP104_OP_CAPS, 0x61, 0x10);
    CHECK(host_report(request, 0, 3u, DP104_REPORT_BYTES) == 1);
    generation = __dp104_master_state.bridge.generation;
    memset(oversize, 0x55, sizeof(oversize));
    dp104_master_uart_rx(oversize, (unsigned)sizeof(oversize));
    CHECK(__dp104_master_state.uart_stream.used == 0);
    CHECK(__dp104_master_state.uart_stream.dropping != 0);
    CHECK(legacy_calls == 0);

    memset(dp104_master_test_report, 0, sizeof(dp104_master_test_report));
    dp104_master_test_report[0] = 0xd1;
    CHECK(dp104_master_host_intercept(0, 3u, DP104_REPORT_BYTES) == 0);
    CHECK(__dp104_master_state.bridge.generation == generation + 1u);
    CHECK(!__dp104_master_state.bridge.request_ready);
    CHECK(!__dp104_master_state.bridge.response_ready);
    CHECK(!__dp104_master_state.bridge.waiting);
    CHECK(!__dp104_master_state.uart_stream_active);
    CHECK(__dp104_master_state.uart_stream.used == 0);
    CHECK(!__dp104_master_state.uart_stream.dropping);
    CHECK(usb_out_rearms == 1); /* D1 belongs to the original handler. */

    dp104_master_uart_rx(updater_bytes, (unsigned)sizeof(updater_bytes));
    CHECK(legacy_calls == 1 && legacy_last_pointer == updater_bytes);
    CHECK(legacy_last_length == sizeof(updater_bytes));
    CHECK(memcmp(legacy_copy, updater_bytes, sizeof(updater_bytes)) == 0);
    CHECK(assert_irq_balanced() == 0);
    return 0;
}

static int test_d1_cancels_bytewise_fifo_write(void)
{
    uint8_t request[DP104_REPORT_BYTES];
    uint8_t query[DP104_REPORT_BYTES] = {DP104_COMMAND, 0xf4, 0x45, 0};
    uint32_t generation;

    reset_fixture();
    make_report(request, DP104_OP_DATA, 0x62, 0x20);
    CHECK(host_report(request, 0, 3u, DP104_REPORT_BYTES) == 1);
    generation = __dp104_master_state.bridge.generation;
    uart_d1_inject_enabled = 1;
    uart_d1_inject_after_writes = 1;

    dp104_master_task_pump();
    CHECK(uart_d1_injected == 1);
    CHECK(uart_output_length == 1);
    CHECK((__dp104_master_state.last_tx_frame_word >> 24) == 1u);
    CHECK(__dp104_master_state.bridge.generation == generation + 1u);
    CHECK(!__dp104_master_state.bridge.request_ready);
    CHECK(!__dp104_master_state.bridge.waiting);
    CHECK(uart_write_inside_critical == 0);
    /* D1 cancels after one FIFO byte; the local snapshot preserves that exact
     * partial-send evidence without resuming the canceled request. */
    CHECK(host_report(query, 0, 3u, DP104_REPORT_BYTES) == 1);
    CHECK(__dp104_master_state.bridge.response_ready);
    CHECK(dp104_read32(__dp104_master_state.bridge.response + 16) ==
          UINT32_C(0x00246202));
    CHECK(dp104_read32(__dp104_master_state.bridge.response + 20) ==
          UINT32_C(0x01246202));
    CHECK(uart_output_length == 1);
    CHECK(assert_irq_balanced() == 0);
    return 0;
}

static int test_usb_retry_retention_and_d1_preserve(void)
{
    const uintptr_t port = 3u;
    uint8_t request[DP104_REPORT_BYTES];
    uint8_t reply[DP104_REPORT_BYTES];
    uint8_t frame[DP104_WIRE_BYTES];
    const uint8_t *old_pointer;
    uint8_t old_contents[DP104_REPORT_BYTES];
    size_t frame_length;

    reset_fixture();
    make_report(request, DP104_OP_CAPS, 0x41, 0x10);
    make_report(reply, DP104_OP_CAPS, 0x41, 0xa0);
    CHECK(host_report(request, 0, port, DP104_REPORT_BYTES) == 1);
    dp104_master_task_pump();
    CHECK(make_wire(DP104_WIRE_RESPONSE, reply, frame, &frame_length));
    dp104_master_uart_rx(frame, (unsigned)frame_length);
    CHECK(__dp104_master_state.bridge.response_ready);

    /* Inactive endpoint is busy; the actual PLC response remains retryable. */
    dp104_master_task_pump();
    CHECK(usb_in_writes == 0 && __dp104_master_state.bridge.response_ready);
    dp104_master_test_usb_active[port] = 1;
    dp104_master_test_usb_remaining[port] = 0;
    dp104_master_task_pump();
    CHECK(usb_in_writes == 1);
    CHECK(usb_retained_pointer ==
          (const uint8_t *)__dp104_master_state.usb.words);
    CHECK((((uintptr_t)usb_retained_pointer) & 3u) == 0);
    CHECK(memcmp(usb_retained_copy, reply, sizeof(reply)) == 0);
    old_pointer = usb_retained_pointer;
    memcpy(old_contents, usb_retained_pointer, sizeof(old_contents));

    /* The previous IN pointer is retained while a newer response waits. */
    make_report(request, DP104_OP_STATUS, 0x42, 0x30);
    CHECK(host_report(request, (uintptr_t)0xdeadbeefu, port,
                      DP104_REPORT_BYTES) == 1);
    dp104_master_task_pump();
    make_report(reply, DP104_OP_STATUS, 0x42, 0xb0);
    CHECK(make_wire(DP104_WIRE_RESPONSE, reply, frame, &frame_length));
    dp104_master_uart_rx(frame, (unsigned)frame_length);
    dp104_master_task_pump();
    CHECK(usb_in_writes == 1 && __dp104_master_state.bridge.response_ready);
    CHECK(usb_retained_pointer == old_pointer);
    CHECK(memcmp(old_pointer, old_contents, sizeof(old_contents)) == 0);

    /* D1 immediately before staging clears the session but preserves the
     * outstanding vendor DMA storage and prevents a stale retry. */
    usb_d1_before_stage = 1;
    dp104_master_task_pump();
    CHECK(uart_d1_injected == 1);
    CHECK(usb_in_writes == 1);
    CHECK(!__dp104_master_state.uart_stream_active);
    CHECK(__dp104_master_state.usb.pending != 0);
    CHECK(usb_retained_pointer == old_pointer);
    CHECK(memcmp(old_pointer, old_contents, sizeof(old_contents)) == 0);
    CHECK(!__dp104_master_state.bridge.response_ready);
    CHECK(assert_irq_balanced() == 0);
    return 0;
}

static int test_usb_queue_rejects_d1_generation(void)
{
    const uintptr_t port = 4u;
    uint8_t request[DP104_REPORT_BYTES];
    uint8_t reply[DP104_REPORT_BYTES];
    uint8_t frame[DP104_WIRE_BYTES];
    size_t frame_length;

    reset_fixture();
    make_report(request, DP104_OP_CAPS, 0x73, 0x10);
    make_report(reply, DP104_OP_CAPS, 0x73, 0xc0);
    CHECK(host_report(request, 0, port, DP104_REPORT_BYTES) == 1);
    dp104_master_task_pump();
    CHECK(make_wire(DP104_WIRE_RESPONSE, reply, frame, &frame_length));
    dp104_master_uart_rx(frame, (unsigned)frame_length);
    dp104_master_test_usb_active[port] = 1;
    dp104_master_test_usb_remaining[port] = 0;

    usb_d1_before_stage = 1;
    usb_d1_make_endpoint_free = 1;
    dp104_master_task_pump();
    CHECK(uart_d1_injected == 1);
    CHECK(usb_in_writes == 0);
    CHECK(!__dp104_master_state.usb.pending);
    CHECK(memcmp(__dp104_master_state.usb.words, reply, sizeof(reply)) == 0);
    CHECK(!__dp104_master_state.bridge.response_ready);
    CHECK(!__dp104_master_state.uart_stream_active);
    CHECK(assert_irq_balanced() == 0);
    return 0;
}

static int test_master_snapshot(void)
{
    uint8_t data[DP104_REPORT_BYTES] = {DP104_COMMAND, DP104_OP_DATA, 0x21, 0, 88};
    uint8_t query[DP104_REPORT_BYTES] = {DP104_COMMAND, 0xf4, 0x45, 0, 1, 2, 3, 4};
    uint8_t report[DP104_REPORT_BYTES];
    uint8_t local_response[DP104_REPORT_BYTES];
    uint8_t reply_frame[DP104_WIRE_BYTES];
    uint32_t generation;
    unsigned sent;
    size_t reply_length;
    unsigned i;
    reset_fixture();
    CHECK(host_report(data, 0, 3u, sizeof(data)) == 1);
    generation = __dp104_master_state.bridge.generation;
    /* Observe a request which is queued but has not reached UART. */
    __dp104_master_state.uart_stream.used = 7;
    CHECK(host_report(query, 0, 3u, sizeof(query)) == 1);
    CHECK(!__dp104_master_state.bridge.request_ready);
    CHECK(!__dp104_master_state.bridge.waiting);
    CHECK(__dp104_master_state.bridge.response_ready);
    CHECK(dp104_read32(__dp104_master_state.bridge.response + 16) == UINT32_C(0x00582102));
    CHECK(dp104_read32(__dp104_master_state.bridge.response + 20) == 0);
    CHECK(dp104_read32(__dp104_master_state.bridge.response + 24) == UINT32_C(0x00070001));
    CHECK(dp104_read32(__dp104_master_state.bridge.response + 28) == generation);
    CHECK(memcmp(__dp104_master_state.bridge.response, query, 8) == 0);
    CHECK(memcmp(__dp104_master_state.bridge.response + 8, "DP104M4\0", 8) == 0);
    dp104_master_task_pump(); /* USB busy retains the local response. */
    CHECK(usb_in_writes == 0 && __dp104_master_state.bridge.response_ready);
    CHECK(uart_output_length == 0);
    dp104_master_test_usb_active[3] = 1;
    dp104_master_task_pump();
    CHECK(usb_in_writes == 1 && uart_output_length == 0);
    CHECK(memcmp(usb_retained_copy, query, 8) == 0);

    reset_fixture();
    CHECK(host_report(data, 0, 3u, sizeof(data)) == 1);
    dp104_master_task_pump();
    CHECK(usb_in_writes == 0); /* DATA requires an actual PLC response. */
    sent = uart_output_length;
    CHECK(sent == DP104_WIRE_BYTES);
    generation = __dp104_master_state.bridge.generation;
    CHECK(host_report(query, 0, 3u, sizeof(query)) == 1);
    CHECK(dp104_read32(__dp104_master_state.bridge.response + 20) == UINT32_C(0xae582102));
    CHECK(dp104_read32(__dp104_master_state.bridge.response + 24) == 2u);
    CHECK(dp104_read32(__dp104_master_state.bridge.response + 28) == generation);
    /* Updater cancels a pending diagnostic response just like a PLC response. */
    CHECK(inject_d1(3u) == 0);
    dp104_master_task_pump();
    CHECK(!__dp104_master_state.bridge.response_ready);
    CHECK(uart_output_length == sent && usb_in_writes == 0);

    reset_fixture();
    CHECK(host_report(data, 0, 3u, sizeof(data)) == 1);
    dp104_master_task_pump();
    CHECK(uart_output_length == DP104_WIRE_BYTES);
    {
        const uint32_t host_word = __dp104_master_state.last_host_frame_word;
        const uint32_t tx_word = __dp104_master_state.last_tx_frame_word;
        const uint8_t non_frame_ops[] = {
            0xf1u, 0xf2u, 0xf3u, DP104_OP_STATUS, DP104_OP_RELEASE
        };
        for(i = 0; i < sizeof(non_frame_ops); ++i)
        {
            make_report(report, non_frame_ops[i], (uint8_t)(0x50u + i), 0x10);
            CHECK(host_report(report, 0, 3u, DP104_REPORT_BYTES) == 1);
            dp104_master_task_pump();
            CHECK(__dp104_master_state.last_host_frame_word == host_word);
            CHECK(__dp104_master_state.last_tx_frame_word == tx_word);
        }
    }
    memset(query, 0, sizeof(query));
    query[0] = DP104_COMMAND;
    query[1] = 0xf4u;
    query[2] = 0x45u;
    CHECK(host_report(query, 0, 3u, sizeof(query)) == 1);
    CHECK(__dp104_master_state.bridge.response_ready);
    memcpy(local_response, __dp104_master_state.bridge.response,
           sizeof(local_response));
    CHECK(dp104_read32(__dp104_master_state.bridge.response + 16) ==
          __dp104_master_state.last_host_frame_word);
    CHECK(dp104_read32(__dp104_master_state.bridge.response + 20) ==
          __dp104_master_state.last_tx_frame_word);
    /* A stale PLC F4-shaped packet cannot replace the local response: F4 is
     * never sent to the PLC and the bridge is no longer waiting for a reply. */
    make_report(report, 0xf4u, query[2], 0);
    CHECK(make_wire(DP104_WIRE_RESPONSE, report, reply_frame, &reply_length));
    CHECK(!__dp104_master_state.bridge.waiting);
    dp104_master_uart_rx(reply_frame, (unsigned)reply_length);
    CHECK(__dp104_master_state.bridge.response_ready);
    CHECK(memcmp(__dp104_master_state.bridge.response, local_response,
                 sizeof(local_response)) == 0);

    reset_fixture();
    CHECK(host_report(data, 0, 3u, sizeof(data)) == 1);
    generation = __dp104_master_state.bridge.generation;
    query[2] = 0;
    CHECK(host_report(query, 0, 3u, sizeof(query)) == 1);
    CHECK(__dp104_master_state.bridge.generation == generation);
    query[2] = 0x45;
    query[31] = 1;
    CHECK(host_report(query, 0, 3u, sizeof(query)) == 1);
    CHECK(__dp104_master_state.bridge.generation == generation);
    query[31] = 0;
    query[3] = 1;
    CHECK(host_report(query, 0, 3u, sizeof(query)) == 1);
    CHECK(__dp104_master_state.bridge.generation == generation);
    query[3] = 0;
    CHECK(host_report(query, 0, 3u, sizeof(query) - 1) == 1);
    CHECK(__dp104_master_state.bridge.generation == generation);
    CHECK(__dp104_master_state.bridge.request_ready);
    CHECK(!__dp104_master_state.bridge.response_ready);
    CHECK(assert_irq_balanced() == 0);
    return 0;
}

static int test_nested_custom_csr_restore(void)
{
    uint8_t request[DP104_REPORT_BYTES];
    uint32_t saved_outer;
    uint32_t saved_inner;

    reset_fixture();
    make_report(request, DP104_OP_CAPS, 0x74, 0x10);
    CHECK(host_report(request, 0, 3u, DP104_REPORT_BYTES) == 1);
    dp104_master_test_csr800 = UINT32_C(0x000000dd);
    saved_outer = __dp104_master_state.bridge.hooks.enter(
        __dp104_master_state.bridge.hooks.context);
    CHECK(saved_outer == UINT32_C(0x88));
    CHECK(dp104_master_test_csr800 == UINT32_C(0x55));
    saved_inner = __dp104_master_state.bridge.hooks.enter(
        __dp104_master_state.bridge.hooks.context);
    CHECK(saved_inner == 0);
    __dp104_master_state.bridge.hooks.leave(
        __dp104_master_state.bridge.hooks.context, saved_inner);
    CHECK(dp104_master_test_csr800 == UINT32_C(0x55));
    __dp104_master_state.bridge.hooks.leave(
        __dp104_master_state.bridge.hooks.context, saved_outer);
    CHECK(dp104_master_test_csr800 == UINT32_C(0xdd));
    CHECK(assert_irq_balanced() == 0);
    return 0;
}

int main(void)
{
    CHECK(test_host_abi_out_rearm_and_no_local_ack() == 0);
    CHECK(test_host_ignores_a0_and_filters_ports() == 0);
    CHECK(test_short_d2_and_stock_passthrough() == 0);
    CHECK(test_fragmented_uart_stream_and_legacy_order() == 0);
    CHECK(test_stream_overflow_and_d1_reset() == 0);
    CHECK(test_d1_cancels_bytewise_fifo_write() == 0);
    CHECK(test_usb_retry_retention_and_d1_preserve() == 0);
    CHECK(test_usb_queue_rejects_d1_generation() == 0);
    CHECK(test_master_snapshot() == 0);
    CHECK(test_nested_custom_csr_restore() == 0);
    puts("master target binding: static RawHID ABI, stream routing, D1 cancellation, and USB lifetime PASS");
    return 0;
}
