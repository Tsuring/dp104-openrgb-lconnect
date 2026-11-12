#include "dp104_master_target.h"

#include "dp104_patch_state.h"
#include "dp104_wire.h"
#include "dp104_wire_stream.h"

#include <string.h>

#define DP104_MASTER_STATE_MAGIC UINT32_C(0x44504d31) /* "DPM1" */
#define DP104_MASTER_CSR_MASK UINT32_C(0x00000088)
#define DP104_MASTER_UPDATE_COMMAND 0xd1u
#define DP104_MASTER_SNAPSHOT_OP 0xf4u

#define DP104_MASTER_RAW_REPORT_ADDRESS ((uintptr_t)0x200104c0u)
#define DP104_MASTER_UART_STATUS_ADDRESS ((uintptr_t)0x40003c0bu)
#define DP104_MASTER_UART_DATA_ADDRESS ((uintptr_t)0x40003c08u)
#define DP104_MASTER_UART_RFC_ADDRESS ((uintptr_t)0x40003c0au)
#define DP104_MASTER_RX_ACCUMULATOR_ADDRESS ((uintptr_t)0x2000df2au)
#define DP104_MASTER_RX_BUFFER_ADDRESS ((uintptr_t)0x20011fb0u)
#define DP104_MASTER_UART_LEGACY_ADDRESS ((uintptr_t)0x0001d722u)
#define DP104_MASTER_USB_IN_ADDRESS ((uintptr_t)0x2000355eu)
#define DP104_MASTER_USB_OUT_REARM_ADDRESS ((uintptr_t)0x20003736u)
#define DP104_MASTER_USB_RECORDS_ADDRESS ((uintptr_t)0x20011f04u)

extern dp104_master_patch_state __dp104_master_state;

typedef void (*dp104_master_usb_fn)(uintptr_t port, const uint8_t *bytes,
                                    unsigned length);
typedef void (*dp104_master_uart_legacy)(const uint8_t *bytes,
                                         unsigned length);

typedef struct master_usb_attempt {
    uint32_t generation;
    uint8_t expected_op;
    uint8_t expected_transaction;
} master_usb_attempt;

static int master_send_plc(void *context, const uint8_t *payload,
                           unsigned length);
static int master_send_usb(void *context, uintptr_t port,
                           const uint8_t *payload, unsigned length);

static uint32_t master_irq_enter(void *context)
{
    (void)context;
#if defined(DP104_MASTER_TARGET_HOST_TEST)
    {
        uint32_t saved = dp104_master_test_csr800 & DP104_MASTER_CSR_MASK;
        dp104_master_test_csr800 &= ~DP104_MASTER_CSR_MASK;
        ++dp104_master_test_irq_enters;
        ++dp104_master_test_irq_depth;
        return saved;
    }
#else
    {
        uint32_t saved;
        const uint32_t mask = DP104_MASTER_CSR_MASK;
        __asm__ volatile(
            "csrrc %0, 0x800, %1\n\t"
            "fence rw, rw"
            : "=&r"(saved)
            : "r"(mask)
            : "memory");
        return saved & DP104_MASTER_CSR_MASK;
    }
#endif
}

static void master_irq_leave(void *context, uint32_t saved)
{
    (void)context;
    saved &= DP104_MASTER_CSR_MASK;
#if defined(DP104_MASTER_TARGET_HOST_TEST)
    dp104_master_test_csr800 |= saved;
    ++dp104_master_test_irq_leaves;
    if(dp104_master_test_irq_depth <= 0)
        dp104_master_test_irq_fault = 1;
    else
        --dp104_master_test_irq_depth;
#else
    __asm__ volatile(
        "fence rw, rw\n\t"
        "csrrs x0, 0x800, %0"
        :
        : "r"(saved)
        : "memory");
#endif
}

static int master_state_initialized(void)
{
    const volatile uint32_t *marker =
        (const volatile uint32_t *)&__dp104_master_state.initialized;
    return *marker == DP104_MASTER_STATE_MAGIC;
}

static const uint8_t *master_static_report(void)
{
#if defined(DP104_MASTER_TARGET_HOST_TEST)
    return dp104_master_test_report;
#else
    return (const uint8_t *)DP104_MASTER_RAW_REPORT_ADDRESS;
#endif
}

static uint8_t master_uart_rfc_read(void)
{
#if defined(DP104_MASTER_TARGET_HOST_TEST)
    return dp104_master_test_uart_rfc();
#else
    return *(volatile const uint8_t *)DP104_MASTER_UART_RFC_ADDRESS;
#endif
}

static uint16_t master_rx_accumulator_read(void)
{
#if defined(DP104_MASTER_TARGET_HOST_TEST)
    return dp104_master_test_rx_accumulator();
#else
    return *(volatile const uint16_t *)DP104_MASTER_RX_ACCUMULATOR_ADDRESS;
#endif
}

static uint8_t master_uart_rx_fifo_count(void)
{
#if defined(DP104_MASTER_TARGET_HOST_TEST)
    return dp104_master_test_uart_rx_rfc();
#else
    return *(volatile const uint8_t *)DP104_MASTER_UART_RFC_ADDRESS;
#endif
}

static uint8_t master_uart_rx_data_read(void)
{
#if defined(DP104_MASTER_TARGET_HOST_TEST)
    return dp104_master_test_uart_rx_data();
#else
    return *(volatile const uint8_t *)DP104_MASTER_UART_DATA_ADDRESS;
#endif
}

static volatile uint16_t *master_rx_accumulator_pointer(void)
{
#if defined(DP104_MASTER_TARGET_HOST_TEST)
    return dp104_master_test_rx_accumulator_pointer();
#else
    return (volatile uint16_t *)DP104_MASTER_RX_ACCUMULATOR_ADDRESS;
#endif
}

static volatile uint8_t *master_rx_buffer_pointer(void)
{
#if defined(DP104_MASTER_TARGET_HOST_TEST)
    return dp104_master_test_rx_buffer_pointer();
#else
    return (volatile uint8_t *)DP104_MASTER_RX_BUFFER_ADDRESS;
#endif
}

static dp104_bridge_hooks master_bridge_hooks(void)
{
    dp104_bridge_hooks hooks;
    hooks.context = &__dp104_master_state;
    hooks.enter = master_irq_enter;
    hooks.leave = master_irq_leave;
    hooks.send_plc = master_send_plc;
    hooks.send_usb = master_send_usb;
    return hooks;
}

static int master_valid_endpoint(uintptr_t port)
{
    uintptr_t endpoint = port & (uintptr_t)0x7f;
    return endpoint != 0 && endpoint <= 4;
}

static void master_initialize_locked(void)
{
    unsigned i;
    dp104_bridge_init(&__dp104_master_state.bridge, master_bridge_hooks());
    for(i = 0; i < DP104_REPORT_BYTES / sizeof(uint32_t); ++i)
        __dp104_master_state.usb.words[i] = 0;
    __dp104_master_state.usb.port = 0;
    __dp104_master_state.usb.pending = 0;
    dp104_wire_stream_init(&__dp104_master_state.uart_stream);
    __dp104_master_state.uart_stream_active = 0;
    __dp104_master_state.last_host_frame_word = 0;
    __dp104_master_state.last_tx_frame_word = 0;
    __dp104_master_state.rx_callback_count = 0;
    __dp104_master_state.rx_callback_total_bytes = 0;
    __dp104_master_state.rx_last_callback_length = 0;
    __dp104_master_state.uart_line_status_events = 0;
    __dp104_master_state.uart_lsr_bit1_events = 0;
    __dp104_master_state.last_uart_lsr = 0;
    __dp104_master_state.rx_snapshot_valid = 0;
    memset(__dp104_master_state.rx_snapshot_nonce, 0,
           sizeof(__dp104_master_state.rx_snapshot_nonce));
    ((volatile uint32_t *)&__dp104_master_state.initialized)[0] =
        DP104_MASTER_STATE_MAGIC;
}

static void master_legacy_uart_rx(const uint8_t *bytes, unsigned length)
{
    if(bytes == NULL)
        return;
#if defined(DP104_MASTER_TARGET_HOST_TEST)
    dp104_master_test_legacy_uart_rx(bytes, length);
#else
    ((dp104_master_uart_legacy)DP104_MASTER_UART_LEGACY_ADDRESS)(bytes,
                                                                  length);
#endif
}

static void master_usb_out_rearm(uintptr_t port)
{
#if defined(DP104_MASTER_TARGET_HOST_TEST)
    dp104_master_test_usb_out_rearm(port, master_static_report(),
                                    DP104_REPORT_BYTES);
#else
    ((dp104_master_usb_fn)DP104_MASTER_USB_OUT_REARM_ADDRESS)(
        port, master_static_report(), DP104_REPORT_BYTES);
#endif
}

static int master_tx_request_current_locked(const uint8_t *payload)
{
    const dp104_master_bridge *bridge = &__dp104_master_state.bridge;
    return bridge->waiting && !bridge->request_ready && payload != NULL &&
           payload[0] == DP104_COMMAND &&
           bridge->expected_op == payload[1] &&
           bridge->expected_transaction == payload[2];
}

static int master_uart_fifo_full(void)
{
#if defined(DP104_MASTER_TARGET_HOST_TEST)
    return dp104_master_test_uart_status() == 8u;
#else
    volatile const uint8_t *status =
        (volatile const uint8_t *)DP104_MASTER_UART_STATUS_ADDRESS;
    return *status == 8u;
#endif
}

static void master_uart_write_byte(uint8_t byte)
{
#if defined(DP104_MASTER_TARGET_HOST_TEST)
    dp104_master_test_uart_write(byte);
#else
    volatile uint8_t *data = (volatile uint8_t *)DP104_MASTER_UART_DATA_ADDRESS;
    *data = byte;
#endif
}

static int master_send_plc(void *context, const uint8_t *payload,
                           unsigned length)
{
    uint8_t frame[DP104_WIRE_BYTES];
    size_t frame_length;
    uint32_t generation;
    uint32_t saved;
    size_t i;
    int tracked;

    (void)context;
    if(!dp104_wire_encode(DP104_WIRE_REQUEST, payload, length, frame,
                          sizeof(frame)))
        return 0;
    frame_length = dp104_wire_frame_size(frame, sizeof(frame));
    if(frame_length == 0 || frame_length > sizeof(frame))
        return 0;

    saved = master_irq_enter(NULL);
    if(!master_tx_request_current_locked(payload))
    {
        master_irq_leave(NULL, saved);
        return 0;
    }
    generation = __dp104_master_state.bridge.generation;
    tracked = payload[1] <= DP104_OP_COMMIT;
    if(tracked)
        __dp104_master_state.last_tx_frame_word =
            (uint32_t)payload[1] | ((uint32_t)payload[2] << 8) |
            ((uint32_t)(payload[1] == DP104_OP_DATA ? payload[4] : 0xffu) << 16) |
            (master_uart_fifo_full() ? UINT32_C(0x40000000) : 0);
    master_irq_leave(NULL, saved);

    for(i = 0; i < frame_length; ++i)
    {
        for(;;)
        {
            int current;

            saved = master_irq_enter(NULL);
            current = __dp104_master_state.bridge.generation == generation &&
                      master_tx_request_current_locked(payload);
            master_irq_leave(NULL, saved);
            if(!current)
                return 0;

            /* Leave interrupts enabled while the hardware FIFO is full. */
            if(master_uart_fifo_full())
                continue;

            /* Recheck the generation and FIFO under a short critical section
             * that contains only one final status read and one byte store. */
            saved = master_irq_enter(NULL);
            current = __dp104_master_state.bridge.generation == generation &&
                      master_tx_request_current_locked(payload);
            if(!current)
            {
                master_irq_leave(NULL, saved);
                return 0;
            }
            if(master_uart_fifo_full())
            {
                master_irq_leave(NULL, saved);
                continue;
            }
            master_uart_write_byte(frame[i]);
            if(tracked)
                __dp104_master_state.last_tx_frame_word =
                    (__dp104_master_state.last_tx_frame_word & UINT32_C(0x40ffffff)) |
                    ((uint32_t)(i + 1u) << 24) |
                    (i + 1u == frame_length ? UINT32_C(0x80000000) : 0);
            master_irq_leave(NULL, saved);
            break;
        }
    }
    return 1;
}

static int master_usb_busy(void *context, uintptr_t port)
{
    uintptr_t endpoint;

    (void)context;
    if(!master_valid_endpoint(port))
        return 1;
    endpoint = port & (uintptr_t)0x7f;

#if defined(DP104_MASTER_TARGET_HOST_TEST)
    return dp104_master_test_usb_active[endpoint] == 0 ||
           dp104_master_test_usb_remaining[endpoint] != 0;
#else
    {
        uintptr_t record_address = DP104_MASTER_USB_RECORDS_ADDRESS +
                                   (endpoint << 4);
        volatile const uint8_t *active =
            (volatile const uint8_t *)(record_address + 6u);
        volatile const uint32_t *remaining =
            (volatile const uint32_t *)(record_address + 12u);
        return *active == 0 || *remaining != 0;
    }
#endif
}

static int master_usb_in_queue(void *context, uintptr_t port,
                               const uint8_t *payload, unsigned length)
{
    const master_usb_attempt *attempt = (const master_usb_attempt *)context;
    const dp104_master_bridge *bridge = &__dp104_master_state.bridge;

    if(payload == NULL || length != DP104_REPORT_BYTES ||
       !master_valid_endpoint(port) || ((uintptr_t)payload & 3u) != 0 ||
       attempt == NULL || !master_state_initialized() ||
       !__dp104_master_state.uart_stream_active || bridge->request_ready ||
       bridge->generation != attempt->generation ||
       bridge->expected_op != attempt->expected_op ||
       bridge->expected_transaction != attempt->expected_transaction ||
       payload[0] != DP104_COMMAND || payload[1] != bridge->expected_op ||
       payload[2] != bridge->expected_transaction)
        return 0;

#if defined(DP104_MASTER_TARGET_HOST_TEST)
    dp104_master_test_usb_in_writer(port, payload, length);
#else
    ((dp104_master_usb_fn)DP104_MASTER_USB_IN_ADDRESS)(port, payload, length);
#endif
    /* The IN helper retains the aligned stage pointer and returns void. */
    return 1;
}

static int master_send_usb(void *context, uintptr_t port,
                           const uint8_t *payload, unsigned length)
{
    master_usb_attempt attempt;
    dp104_usb_stage_hooks hooks;
    uint32_t saved;

    (void)context;
    if(payload == NULL || length != DP104_REPORT_BYTES)
        return 0;

    saved = master_irq_enter(NULL);
    if(!master_state_initialized() ||
       !__dp104_master_state.uart_stream_active ||
       __dp104_master_state.bridge.request_ready ||
       payload[0] != DP104_COMMAND ||
       payload[1] != __dp104_master_state.bridge.expected_op ||
       payload[2] != __dp104_master_state.bridge.expected_transaction)
    {
        master_irq_leave(NULL, saved);
        return 0;
    }
    attempt.generation = __dp104_master_state.bridge.generation;
    attempt.expected_op = __dp104_master_state.bridge.expected_op;
    attempt.expected_transaction =
        __dp104_master_state.bridge.expected_transaction;
    master_irq_leave(NULL, saved);

#if defined(DP104_MASTER_TARGET_HOST_TEST)
    dp104_master_test_before_usb_stage();
#endif

    hooks.context = &attempt;
    hooks.enter = master_irq_enter;
    hooks.leave = master_irq_leave;
    hooks.busy = master_usb_busy;
    hooks.queue = master_usb_in_queue;
    return dp104_usb_stage_send(&__dp104_master_state.usb, hooks, port,
                                payload, length);
}

/* This response describes the Master itself, not a PLC acknowledgement.
 * Caller holds the IRQ guard. Capture the previous slot before superseding it;
 * use the ordinary persistent response/pump path so USB busy retry and D1
 * cancellation retain exactly the same ownership rules. */
static int master_nonzero_bytes(const uint8_t *bytes, unsigned length)
{
    unsigned i;
    for(i = 0; i < length; ++i)
        if(bytes[i] != 0u) return 1;
    return 0;
}

static int master_zero_bytes(const uint8_t *bytes, unsigned length)
{
    unsigned i;
    for(i = 0; i < length; ++i)
        if(bytes[i] != 0u) return 0;
    return 1;
}

/* Called only for F4 while an original BEGIN/DATA/COMMIT is still pending.
 * All fields and the stream prefix are frozen before F4 supersedes that slot.
 * LSR comes from the existing IRQ-side one-read hook; never read the
 * clear-on-read register here. */
static void master_freeze_rx_snapshot_locked(const uint8_t *request)
{
    dp104_master_patch_state *state = &__dp104_master_state;
    dp104_master_bridge *bridge = &state->bridge;
    uint8_t *blob = state->rx_snapshot_blob;
    uint32_t flags;
    size_t copy_bytes;

    if(state->rx_snapshot_valid || !bridge->waiting ||
       bridge->expected_op < DP104_OP_BEGIN ||
       bridge->expected_op > DP104_OP_COMMIT ||
       !master_nonzero_bytes(request + 4u, 4u))
        return;

    memset(blob, 0, DP104_MASTER_RX_SNAPSHOT_BYTES);
    memcpy(blob, "M6RX", 4u);
    dp104_write16(blob + 4u, DP104_MASTER_RX_SNAPSHOT_SCHEMA);
    dp104_write16(blob + 6u, DP104_MASTER_RX_SNAPSHOT_HEADER_BYTES);
    dp104_write16(blob + 8u, DP104_MASTER_RX_SNAPSHOT_BYTES);
    dp104_write16(blob + 10u, DP104_MASTER_RX_SNAPSHOT_DATA_BYTES);
    blob[12] = master_uart_rfc_read();
    blob[13] = state->uart_stream.dropping;
    blob[14] = state->uart_stream_active;
    dp104_write16(blob + 16u, master_rx_accumulator_read());
    blob[18] = state->last_uart_lsr;
    dp104_write32(blob + 20u, state->uart_stream.used);
    dp104_write32(blob + 24u, state->rx_callback_count);
    dp104_write32(blob + 28u, state->rx_callback_total_bytes);
    dp104_write32(blob + 32u, state->rx_last_callback_length);
    dp104_write32(blob + 36u, state->uart_line_status_events);
    dp104_write32(blob + 40u, state->uart_lsr_bit1_events);
    dp104_write32(blob + 44u, bridge->generation);
    dp104_write32(blob + 48u, state->last_host_frame_word);
    dp104_write32(blob + 52u, state->last_tx_frame_word);
    flags = (uint32_t)bridge->request_ready |
            ((uint32_t)bridge->waiting << 1) |
            ((uint32_t)bridge->response_ready << 2);
    dp104_write32(blob + 56u, flags);
    blob[60] = bridge->expected_op;
    blob[61] = bridge->expected_transaction;

    copy_bytes = state->uart_stream.used;
    if(copy_bytes > DP104_MASTER_RX_SNAPSHOT_DATA_BYTES)
        copy_bytes = DP104_MASTER_RX_SNAPSHOT_DATA_BYTES;
    if(copy_bytes)
        memcpy(blob + DP104_MASTER_RX_SNAPSHOT_HEADER_BYTES,
               state->uart_stream.data, copy_bytes);
    memcpy(state->rx_snapshot_nonce, request + 4u,
           sizeof(state->rx_snapshot_nonce));
    state->rx_snapshot_valid = 1u;
}

/* F6 is strictly local metadata paging. Its response uses the bridge's
 * persistent USB response slot and is never submitted to send_plc. */
static void master_rx_snapshot_locked(uintptr_t port, const uint8_t *report,
                                      unsigned length)
{
    dp104_master_bridge *bridge = &__dp104_master_state.bridge;
    const uint16_t offset = length >= 10u ? dp104_read16(report + 8u) : 0u;
    uint8_t status = DP104_BAD_REQUEST;
    uint8_t *response = bridge->response;
    unsigned i;

    if(length != DP104_REPORT_BYTES)
        status = DP104_BAD_LENGTH;
    else if(report[0] != DP104_COMMAND ||
            report[1] != DP104_MASTER_RX_SNAPSHOT_OP || report[2] == 0u ||
            report[3] != 0u || !master_nonzero_bytes(report + 4u, 4u) ||
            !master_zero_bytes(report + 10u, DP104_REPORT_BYTES - 10u) ||
            offset >= DP104_MASTER_RX_SNAPSHOT_BYTES || (offset % 16u) != 0u)
        status = DP104_BAD_REQUEST;
    else if(!__dp104_master_state.rx_snapshot_valid ||
            report[4] != __dp104_master_state.rx_snapshot_nonce[0] ||
            report[5] != __dp104_master_state.rx_snapshot_nonce[1] ||
            report[6] != __dp104_master_state.rx_snapshot_nonce[2] ||
            report[7] != __dp104_master_state.rx_snapshot_nonce[3])
        status = DP104_BAD_STATE;
    else
        status = DP104_OK;

    for(i = 0; i < DP104_REPORT_BYTES; ++i) response[i] = 0u;
    response[0] = DP104_COMMAND;
    response[1] = DP104_MASTER_RX_SNAPSHOT_OP;
    response[2] = length >= 3u ? report[2] : 0u;
    response[3] = status;
    if(length >= 8u) memcpy(response + 4u, report + 4u, 4u);
    memcpy(response + 8u, "M6RX", 4u);
    dp104_write16(response + 12u, offset);
    dp104_write16(response + 14u, DP104_MASTER_RX_SNAPSHOT_BYTES);
    if(status == DP104_OK)
        memcpy(response + 16u, __dp104_master_state.rx_snapshot_blob + offset, 16u);

    bridge->port = port;
    ++bridge->generation;
    bridge->expected_op = DP104_MASTER_RX_SNAPSHOT_OP;
    bridge->expected_transaction = response[2];
    bridge->request_ready = 0;
    bridge->waiting = 0;
    bridge->response_ready = 1;
}

static void master_snapshot_locked(uintptr_t port, const uint8_t *report,
                                   unsigned length)
{
    static const uint8_t magic[8] = {'D', 'P', '1', '0', '4', 'M', '4', 0};
    dp104_master_bridge *bridge = &__dp104_master_state.bridge;
    uint32_t flags, generation;
    unsigned i;
    if(length != DP104_REPORT_BYTES || report[2] == 0 || report[3] != 0)
        return;
    for(i = 8; i < DP104_REPORT_BYTES; ++i)
        if(report[i] != 0) return;
    master_freeze_rx_snapshot_locked(report);
    flags = (uint32_t)bridge->request_ready |
            ((uint32_t)bridge->waiting << 1) |
            ((uint32_t)bridge->response_ready << 2) |
            ((uint32_t)__dp104_master_state.uart_stream.used << 16);
    generation = bridge->generation;
    for(i = 0; i < 8; ++i) bridge->response[i] = report[i];
    for(i = 0; i < 8; ++i) bridge->response[8 + i] = magic[i];
    dp104_write32(bridge->response + 16, __dp104_master_state.last_host_frame_word);
    dp104_write32(bridge->response + 20, __dp104_master_state.last_tx_frame_word);
    dp104_write32(bridge->response + 24, flags);
    dp104_write32(bridge->response + 28, generation);
    bridge->port = port;
    ++bridge->generation;
    bridge->expected_op = DP104_MASTER_SNAPSHOT_OP;
    bridge->expected_transaction = report[2];
    bridge->request_ready = 0;
    bridge->waiting = 0;
    bridge->response_ready = 1;
}

int dp104_master_host_intercept(uintptr_t opaque_a0, uintptr_t port,
                                unsigned length)
{
    const uint8_t *report;
    uint32_t saved;

    (void)opaque_a0;
    if(length == 0 || (port & (uintptr_t)0x80) != 0 ||
       !master_valid_endpoint(port))
        return 0;

    report = master_static_report();
    if(report[0] == DP104_MASTER_UPDATE_COMMAND)
    {
        saved = master_irq_enter(NULL);
        if(master_state_initialized())
        {
            dp104_master_bridge *bridge = &__dp104_master_state.bridge;
            ++bridge->generation;
            bridge->request_ready = 0;
            bridge->response_ready = 0;
            bridge->waiting = 0;
            dp104_wire_stream_reset(&__dp104_master_state.uart_stream);
            __dp104_master_state.uart_stream_active = 0;
        }
        master_irq_leave(NULL, saved);
        return 0;
    }
    if(report[0] != DP104_COMMAND)
        return 0;

    /* Serialize first initialization, stream activation, and request copying
     * against a D1 updater boundary. bridge_on_host nests the same save/restore
     * primitive and copies the static report before OUT is rearmed. */
    saved = master_irq_enter(NULL);
    if(!master_state_initialized())
        master_initialize_locked();
    if(!__dp104_master_state.uart_stream_active)
    {
        dp104_wire_stream_reset(&__dp104_master_state.uart_stream);
        __dp104_master_state.uart_stream_active = 1;
    }
    if(length >= 2 && report[1] == DP104_MASTER_SNAPSHOT_OP)
        master_snapshot_locked(port, report, length);
    else if(length >= 2 && report[1] == DP104_MASTER_RX_SNAPSHOT_OP)
        master_rx_snapshot_locked(port, report, length);
    else
    {
        if(length == DP104_REPORT_BYTES && report[3] == 0 &&
           report[1] <= DP104_OP_COMMIT)
            __dp104_master_state.last_host_frame_word =
                (uint32_t)report[1] | ((uint32_t)report[2] << 8) |
                ((uint32_t)(report[1] == DP104_OP_DATA ? report[4] : 0xffu) << 16);
        (void)dp104_bridge_on_host(&__dp104_master_state.bridge, port, report,
                                   (size_t)length);
    }
    master_irq_leave(NULL, saved);

    master_usb_out_rearm(port);
    return 1;
}

void dp104_master_uart_rda_drain(void)
{
    unsigned drained = 0;
    volatile uint16_t *accumulator = master_rx_accumulator_pointer();
    volatile uint8_t *buffer = master_rx_buffer_pointer();

    while(drained < 8u)
    {
        uint16_t cursor;
        uint8_t byte;

        /* Leave one byte for the stock UART character-timeout path. With the
         * FIFO trigger lowered to two, draining the final byte here would
         * clear the receiver before its existing timeout callback can run. */
        if(master_uart_rx_fifo_count() <= 1u)
            break;
        cursor = *accumulator;
        byte = master_uart_rx_data_read();
        buffer[cursor] = byte;
        *accumulator = (uint16_t)(cursor + 1u);
        ++drained;
    }
}

static int master_stream_next(uint8_t *record, size_t capacity,
                              size_t *record_length, uint8_t *is_extension,
                              uint32_t *generation)
{
    uint32_t saved = master_irq_enter(NULL);
    int result;

    if(!master_state_initialized() ||
       !__dp104_master_state.uart_stream_active)
    {
        master_irq_leave(NULL, saved);
        return -2;
    }
    result = dp104_wire_stream_next(&__dp104_master_state.uart_stream,
                                    record, capacity, record_length,
                                    is_extension);
    *generation = __dp104_master_state.bridge.generation;
    if(result < 0)
    {
        dp104_wire_stream_reset(&__dp104_master_state.uart_stream);
        __dp104_master_state.uart_stream.dropping = 1;
    }
    master_irq_leave(NULL, saved);
    return result;
}

static int master_stream_session_current(uint32_t generation)
{
    uint32_t saved = master_irq_enter(NULL);
    int current = master_state_initialized() &&
                  __dp104_master_state.uart_stream_active &&
                  __dp104_master_state.bridge.generation == generation;
    master_irq_leave(NULL, saved);
    return current;
}

void dp104_master_uart_rx(const uint8_t *bytes, unsigned length)
{
    uint8_t record[DP104_WIRE_STREAM_FRAME_MAX];
    size_t record_length;
    uint8_t is_extension;
    uint32_t generation;
    uint32_t saved;
    int result;

    if(bytes == NULL)
        return;

    if(!master_state_initialized())
    {
        master_legacy_uart_rx(bytes, length);
        return;
    }

    saved = master_irq_enter(NULL);
    if(!master_state_initialized() ||
       !__dp104_master_state.uart_stream_active)
    {
        master_irq_leave(NULL, saved);
        master_legacy_uart_rx(bytes, length);
        return;
    }
    ++__dp104_master_state.rx_callback_count;
    __dp104_master_state.rx_callback_total_bytes += (uint32_t)length;
    __dp104_master_state.rx_last_callback_length = (uint32_t)length;
    /* push() checks length before reading bytes. Oversize input clears the
     * bounded FIFO and enters its drop/resynchronization mode. */
    result = dp104_wire_stream_push(&__dp104_master_state.uart_stream,
                                   bytes, (size_t)length);
    master_irq_leave(NULL, saved);
    if(!result)
        return;

    for(;;)
    {
        result = master_stream_next(record, sizeof(record), &record_length,
                                    &is_extension, &generation);
        if(result <= 0)
            return;
        if(!master_stream_session_current(generation))
            continue;

        if(is_extension)
        {
            uint8_t report[DP104_REPORT_BYTES];
            if(record[0] == DP104_WIRE_RESPONSE &&
               dp104_wire_decode(DP104_WIRE_RESPONSE, record, record_length,
                                 report, sizeof(report)))
                dp104_bridge_on_plc(&__dp104_master_state.bridge, report,
                                    sizeof(report));
        }
        else
            master_legacy_uart_rx(record, (unsigned)record_length);
    }
}

void dp104_master_uart_line_status(uint8_t raw_lsr)
{
    if(!master_state_initialized()) return;
    ++__dp104_master_state.uart_line_status_events;
    __dp104_master_state.last_uart_lsr = raw_lsr;
    if(raw_lsr & 0x02u)
        ++__dp104_master_state.uart_lsr_bit1_events;
}

void dp104_master_task_pump(void)
{
    if(master_state_initialized())
        dp104_bridge_pump(&__dp104_master_state.bridge);
}
