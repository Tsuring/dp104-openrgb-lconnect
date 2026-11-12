#include "dp104_plc_target.h"
#include "dp104_version.h"
#include "dp104_wire.h"

#include <stddef.h>
#include <string.h>

#define DP104_PLC_UART_QUEUE_POP_ADDR UINT32_C(0x08018f25)
#define DP104_PLC_UART_SEND_ADDR      UINT32_C(0x08015c19)
#define DP104_PLC_MILLIS_LOW_ADDR     UINT32_C(0x20001d38)
#define DP104_PLC_KEY_BUFFER_ADDR     UINT32_C(0x20002348)
#define DP104_PLC_DIRTY_ADDR          UINT32_C(0x2000252c)
#define DP104_PLC_UART_DMA_BYTES      DP104_PLC_RX_DMA_BYTES
#define DP104_PLC_RECEIVER_SNAPSHOT_OP 0xf3u

#define DP104_PLC_STATIC_ASSERT(name, expression) \
    typedef char name[(expression) ? 1 : -1]

DP104_PLC_STATIC_ASSERT(dp104_plc_rx_sr_after_stream,
    offsetof(dp104_plc_patch_state, rx_sr_before_dr) ==
    offsetof(dp104_plc_patch_state, uart_stream) + sizeof(dp104_wire_stream));
DP104_PLC_STATIC_ASSERT(dp104_plc_rx_cndtr_after_sr,
    offsetof(dp104_plc_patch_state, rx_cndtr_after_dr) ==
    offsetof(dp104_plc_patch_state, rx_sr_before_dr) + sizeof(uint32_t));
DP104_PLC_STATIC_ASSERT(dp104_plc_rx_count_after_cndtr,
    offsetof(dp104_plc_patch_state, rx_sample_count) ==
    offsetof(dp104_plc_patch_state, rx_cndtr_after_dr) + sizeof(uint32_t));
DP104_PLC_STATIC_ASSERT(dp104_plc_fixed_reply_idle_tail,
    DP104_WIRE_BYTES == 46u && DP104_WIRE_BYTES % 7u != 0u);
DP104_PLC_STATIC_ASSERT(dp104_plc_rx_buffer_capacity,
    sizeof(((dp104_plc_patch_state *)0)->rx_dma_buffer) == DP104_PLC_UART_DMA_BYTES);
DP104_PLC_STATIC_ASSERT(dp104_plc_rx_buffer_alignment,
    offsetof(dp104_plc_patch_state, rx_dma_buffer) % 4u == 0);
#ifndef DP104_PLC_HOST_TEST
DP104_PLC_STATIC_ASSERT(dp104_plc_rx_sr_target_offset,
    offsetof(dp104_plc_patch_state, rx_sr_before_dr) == 8896u);
DP104_PLC_STATIC_ASSERT(dp104_plc_rx_cndtr_target_offset,
    offsetof(dp104_plc_patch_state, rx_cndtr_after_dr) == 8900u);
DP104_PLC_STATIC_ASSERT(dp104_plc_rx_count_target_offset,
    offsetof(dp104_plc_patch_state, rx_sample_count) == 8904u);
DP104_PLC_STATIC_ASSERT(dp104_plc_rx_buffer_target_offset,
    offsetof(dp104_plc_patch_state, rx_dma_buffer) == 11112u);
DP104_PLC_STATIC_ASSERT(dp104_plc_rx_owned_target_bytes,
    sizeof(dp104_plc_patch_state) == 11244u);
DP104_PLC_STATIC_ASSERT(dp104_plc_rx_cursor_target_offset,
    offsetof(dp104_plc_patch_state, rx_dma_cursor) == 11240u);
#endif

extern dp104_plc_patch_state __dp104_plc_state;

static uint32_t irq_save(void)
{
#ifdef DP104_PLC_HOST_TEST
    return dp104_plc_test_irq_save();
#else
    uint32_t primask;
    __asm volatile ("mrs %0, primask\n\tcpsid i"
                    : "=r"(primask) : : "memory");
    return primask;
#endif
}

static void irq_restore(uint32_t primask)
{
#ifdef DP104_PLC_HOST_TEST
    dp104_plc_test_irq_restore(primask);
#else
    __asm volatile ("msr primask, %0" : : "r"(primask) : "memory");
#endif
}

#define DP104_DMA_RX_HALF_FLAG UINT32_C(0x00400000)
#define DP104_DMA_RX_COMPLETE_FLAG UINT32_C(0x00200000)
#define DP104_DMA_RX_FLAGS (DP104_DMA_RX_HALF_FLAG | DP104_DMA_RX_COMPLETE_FLAG)

static uint32_t dma_flags(void)
{
#ifdef DP104_PLC_HOST_TEST
    return dp104_plc_test_dma_flags();
#else
    return *(const volatile uint32_t *)(uintptr_t)UINT32_C(0x40020000);
#endif
}

static uint32_t dma_remaining(void)
{
#ifdef DP104_PLC_HOST_TEST
    return dp104_plc_test_dma_remaining();
#else
    return *(const volatile uint32_t *)(uintptr_t)UINT32_C(0x40020070);
#endif
}

static void dma_clear(uint32_t flags)
{
#ifdef DP104_PLC_HOST_TEST
    dp104_plc_test_dma_clear(flags);
#else
    /* W1C only the observed RX channel HT/TC flags, never TX channel 7. */
    *(volatile uint32_t *)(uintptr_t)UINT32_C(0x40020004) = flags;
#endif
}

static void dma_enable_irq(void)
{
#ifdef DP104_PLC_HOST_TEST
    dp104_plc_test_dma_enable_irq();
#else
    volatile uint8_t *priority = (volatile uint8_t *)(uintptr_t)UINT32_C(0xe000e400);
    priority[16] = priority[38];
    /* Preserve a pending receive event; the handler validates/clears flags. */
    *(volatile uint32_t *)(uintptr_t)UINT32_C(0xe000e100) = UINT32_C(1) << 16;
#endif
}

static void uart_rx_locked(const uint8_t *data, uint32_t length);

static void dma_emit(void *context, const uint8_t *bytes, size_t length)
{
    (void)context;
    uart_rx_locked(bytes, (uint32_t)length);
}

/* Caller serializes DMA/USART handlers. The channel stays running in circular
 * mode: no disable/reload window is introduced while bytes arrive. */
static void dma_drain_locked(void)
{
    dp104_plc_patch_state *state = &__dp104_plc_state;
    uint32_t before, after, remaining;
    unsigned attempts = 3;
    uint8_t events;
    dp104_dma_rx_result result;

    do {
        before = dma_flags() & DP104_DMA_RX_FLAGS;
        remaining = dma_remaining();
        after = dma_flags() & DP104_DMA_RX_FLAGS;
    } while(before != after && --attempts);
    dma_clear(after);
    state->rx_cndtr_after_dr = remaining;
    events = (uint8_t)((after & DP104_DMA_RX_HALF_FLAG ?
                         DP104_DMA_RX_EVENT_HALF : 0u) |
                      (after & DP104_DMA_RX_COMPLETE_FLAG ?
                         DP104_DMA_RX_EVENT_COMPLETE : 0u));
    if(before != after) events = DP104_DMA_RX_EVENT_HALF | DP104_DMA_RX_EVENT_COMPLETE;
    result = dp104_dma_rx_consume(&state->rx_dma_cursor, state->rx_dma_buffer,
        DP104_PLC_RX_DMA_BYTES, remaining, events, dma_emit, state);
    if(result != DP104_DMA_RX_OK)
    {
        state->diagnostic.reserved0 |= 2u;
        state->uart_stream.used = 0;
        state->uart_stream.dropping = 1;
    }
    if(events || result != DP104_DMA_RX_OK)
        dp104_plc_diagnostic_note_parser(&state->diagnostic,
            result != DP104_DMA_RX_OK ? DP104_PLC_DIAG_DMA_OVERRUN :
            events & DP104_DMA_RX_EVENT_COMPLETE ? DP104_PLC_DIAG_DMA_COMPLETE :
                                                 DP104_PLC_DIAG_DMA_HALF,
            (uint16_t)remaining, state->uart_stream.used,
            state->uart_stream.used, state->uart_stream.dropping,
            state->uart_stream.dropping);
}

void dp104_plc_dma_irq(void)
{
    const uint32_t primask = irq_save();
    /* DMA interrupts do not read USART SR/DR. Zero means no fresh SR sample. */
    __dp104_plc_state.rx_sr_before_dr = 0;
    dma_drain_locked();
    irq_restore(primask);
}

void dp104_plc_usart_irq(void)
{
    const uint32_t primask = irq_save();
    uint32_t status, data;
#ifdef DP104_PLC_HOST_TEST
    if(!(dp104_plc_test_uart_control() & 0x10u) ||
       !(dp104_plc_test_uart_status() & 0x10u))
    {
        irq_restore(primask);
        return;
    }
    status = dp104_plc_test_uart_status();
    data = dp104_plc_test_uart_data();
#else
    const volatile uint32_t *uart =
        (const volatile uint32_t *)(uintptr_t)UINT32_C(0x40004400);
    if(!(uart[3] & 0x10u) || !(uart[0] & 0x10u))
    {
        irq_restore(primask);
        return;
    }
    /* Keep the original adjacent SR/DR read-to-clear sequence exactly once. */
    status = uart[0];
    data = uart[1];
#endif
    (void)data;
    __dp104_plc_state.rx_sr_before_dr = status;
    ++__dp104_plc_state.rx_sample_count;
    dma_drain_locked();
    irq_restore(primask);
}

static uint32_t target_millis(void)
{
#ifdef DP104_PLC_HOST_TEST
    return dp104_plc_test_millis();
#else
    /* The low word is naturally aligned and read atomically on Cortex-M3. */
    const volatile uint32_t *ticks =
        (const volatile uint32_t *)(uintptr_t)DP104_PLC_MILLIS_LOW_ADDR;
    return *ticks;
#endif
}

static void legacy_queue_pop(uint8_t **out)
{
#ifdef DP104_PLC_HOST_TEST
    dp104_plc_test_legacy_pop(out);
#else
    typedef void (*queue_pop_fn)(uint8_t **);
    ((queue_pop_fn)(uintptr_t)DP104_PLC_UART_QUEUE_POP_ADDR)(out);
#endif
}

static void uart_send(uint8_t *bytes, uint32_t length, uint32_t mode)
{
#ifdef DP104_PLC_HOST_TEST
    dp104_plc_test_uart_send(bytes, length, mode);
#else
    typedef void (*uart_send_fn)(uint8_t *, uint32_t, uint32_t);
    ((uart_send_fn)(uintptr_t)DP104_PLC_UART_SEND_ADDR)(bytes, length, mode);
#endif
}

static volatile uint8_t *key_buffer(void)
{
#ifdef DP104_PLC_HOST_TEST
    return dp104_plc_test_key_buffer;
#else
    return (volatile uint8_t *)(uintptr_t)DP104_PLC_KEY_BUFFER_ADDR;
#endif
}

static volatile uint8_t *dirty_flag(void)
{
#ifdef DP104_PLC_HOST_TEST
    return &dp104_plc_test_dirty;
#else
    return (volatile uint8_t *)(uintptr_t)DP104_PLC_DIRTY_ADDR;
#endif
}

static int valid_color_order(const uint8_t order[3])
{
    uint8_t seen = 0;
    unsigned i;
    for(i = 0; i < 3; ++i)
    {
        if(order[i] > 2u || (seen & (uint8_t)(1u << order[i]))) return 0;
        seen |= (uint8_t)(1u << order[i]);
    }
    return seen == 7u;
}

static int target_ready(void *context)
{
    const dp104_plc_patch_state *state = (const dp104_plc_patch_state *)context;
    return state && state->initialized == DP104_PLC_TARGET_MAGIC &&
           state->color_order_verified && valid_color_order(state->color_order);
}

static int publish_frame(void *context, const uint8_t *rgb, unsigned leds)
{
    dp104_plc_patch_state *state = (dp104_plc_patch_state *)context;
    volatile uint8_t *output;
    volatile uint8_t *dirty;
    uint32_t primask;
    unsigned led, channel;

    if(!state || !rgb || leds != DP104_LED_COUNT || !target_ready(state)) return 0;
    primask = irq_save();
    if(!target_ready(state))
    {
        irq_restore(primask);
        return 0;
    }

    output = key_buffer();
    dirty = dirty_flag();
    /* The setter gate becomes visible before any host color reaches the buffer. */
    state->override_active = 1;
    for(led = 0; led < DP104_LED_COUNT; ++led)
        for(channel = 0; channel < 3; ++channel)
            output[led * 3u + state->color_order[channel]] = rgb[led * 3u + channel];
    *dirty = 1;
    irq_restore(primask);
    return 1;
}

static void release_output(void *context)
{
    dp104_plc_patch_state *state = (dp104_plc_patch_state *)context;
    uint32_t primask;
    if(!state) return;
    primask = irq_save();
    state->override_active = 0;
    state->diagnostic_active = 0;
    state->diagnostic_started = 0;
    *dirty_flag() = 1;
    irq_restore(primask);
}

int dp104_plc_target_init(void)
{
    dp104_output_hooks hooks;
    dp104_plc_patch_state *state = &__dp104_plc_state;

    /* Startup BSS clearing covers this entire measured reservation. Do not
     * clear it again after peripherals start: that would erase early ingress
     * or block half-transfer servicing. This entry is called once after BSS. */
    state->diagnostic.reserved0 |= 1u;
    if(!dp104_twiddle_rebuild(state->twiddle, DP104_TWIDDLE_WORDS,
                              __dp104_twiddle_basis, DP104_TWIDDLE_WORDS))
        return 0;

    hooks.context = state;
    hooks.ready = target_ready;
    hooks.publish = publish_frame;
    hooks.release = release_output;
    dp104_receiver_init(&state->receiver, hooks);
    dma_enable_irq();
    state->initialized = DP104_PLC_TARGET_MAGIC;
    return 1;
}

static void poll_at(uint32_t now_ms)
{
    dp104_plc_patch_state *state = &__dp104_plc_state;
    uint32_t primask;

    if(state->initialized != DP104_PLC_TARGET_MAGIC) return;
    if(state->diagnostic_active &&
       (uint32_t)(now_ms - state->diagnostic_started) >= DP104_PLC_DIAGNOSTIC_MS)
    {
        primask = irq_save();
        if(state->diagnostic_active &&
           (uint32_t)(now_ms - state->diagnostic_started) >= DP104_PLC_DIAGNOSTIC_MS)
        {
            state->diagnostic_active = 0;
            state->diagnostic_started = 0;
            state->override_active = 0;
            *dirty_flag() = 1;
        }
        irq_restore(primask);
    }
    dp104_receiver_poll(&state->receiver, now_ms);
}

void dp104_plc_target_poll(void)
{
    poll_at(target_millis());
}

static void uart_rx_locked(const uint8_t *data, uint32_t length)
{
    dp104_plc_patch_state *state = &__dp104_plc_state;
    uint16_t stream_used_before = state->uart_stream.used;
    uint8_t push_failed = 0;

    /* The owned DMA buffer is 128 bytes. On an invalid callback, drop the partial
     * stream and wait for the next unambiguous direction prefix. */
    if(length > DP104_PLC_UART_DMA_BYTES || (length && !data))
    {
        state->uart_stream.used = 0;
        state->uart_stream.dropping = 1;
        push_failed = 1;
    }
    else
        push_failed = !dp104_wire_stream_push(&state->uart_stream, data, length);

    /* Circular DMA may overwrite its old span after the copy. Scan the stable
     * IRQ-owned FIFO copy rather than the live peripheral destination. */
    dp104_plc_diagnostic_on_rx(&state->diagnostic,
        !push_failed && length ? state->uart_stream.data + stream_used_before : data, length,
        state->rx_sr_before_dr, state->rx_cndtr_after_dr,
        state->rx_sample_count, stream_used_before,
        state->uart_stream.used, push_failed);

}

void dp104_plc_uart_rx(const uint8_t *data, uint32_t length)
{
    const uint32_t primask = irq_save();
    uart_rx_locked(data, length);
    irq_restore(primask);
}

int dp104_plc_should_skip_setter(void)
{
    /* Called from the Thumb veneer; deliberately no polling or side effects. */
    return __dp104_plc_state.override_active != 0;
}

static int all_zero(const uint8_t *bytes, unsigned start, unsigned end)
{
    unsigned i;
    for(i = start; i < end; ++i)
        if(bytes[i]) return 0;
    return 1;
}

static int valid_caps_request(const uint8_t request[DP104_REPORT_BYTES])
{
    return request[0] == DP104_COMMAND && request[1] == DP104_OP_CAPS &&
           request[3] == 0 && all_zero(request, 4, DP104_REPORT_BYTES);
}

static void response_header(uint8_t response[DP104_REPORT_BYTES],
                            const uint8_t request[DP104_REPORT_BYTES], uint8_t status)
{
    memset(response, 0, DP104_REPORT_BYTES);
    response[0] = request[0];
    response[1] = request[1];
    response[2] = request[2];
    response[3] = status;
}

static void handle_app_info(dp104_plc_patch_state *state,
                            const uint8_t request[DP104_REPORT_BYTES],
                            uint8_t response[DP104_REPORT_BYTES])
{
    if(!request[2] || request[3] != 0 ||
       all_zero(request, 4, 20) || !all_zero(request, 20, DP104_REPORT_BYTES))
    {
        response[3] = DP104_BAD_REQUEST;
        return;
    }

    response[3] = DP104_OK;
    memcpy(response + 4, DP104_CAPS_MAGIC, 8);
    response[12] = COMPAT_VERSION_MAJOR;
    response[13] = COMPAT_VERSION_MINOR;
    response[14] = COMPAT_VERSION_PATCH;
    response[15] = target_ready(state) ? 1u : 0u;
    memcpy(response + 16, request + 4, 16);
}

static void handle_receiver_snapshot(
    const uint8_t request[DP104_REPORT_BYTES],
    uint8_t response[DP104_REPORT_BYTES], const dp104_receiver *receiver)
{
    if(!request[2] || request[3] != 0 || all_zero(request, 4, 8) ||
       !all_zero(request, 8, DP104_REPORT_BYTES))
    {
        response[3] = DP104_BAD_REQUEST;
        return;
    }

    /* Read-only task-context snapshot. The receiver is owned by the queue
     * worker; this handler does not poll, commit, release, or renew its lease. */
    memcpy(response + 4, request + 4, 4);
    response[8] = (uint8_t)((receiver->direct_active ? 1u : 0u) |
                            (receiver->staging_active ? 2u : 0u));
    response[9] = receiver->staging_active ? receiver->transaction : 0u;
    response[10] = receiver->active_transaction;
    memcpy(response + 11, receiver->received, sizeof(receiver->received));
    dp104_write32(response + 24, receiver->expected_crc);
    dp104_write32(response + 28,
                  dp104_crc32(receiver->staging, DP104_FRAME_BYTES));
}

static uint8_t handle_calibration(dp104_plc_patch_state *state,
                                  const uint8_t request[DP104_REPORT_BYTES],
                                  uint32_t now_ms)
{
    uint32_t primask;
    unsigned led, channel;
    volatile uint8_t *output;

    if(!request[2] || request[3] != 0) return DP104_BAD_REQUEST;
    if(request[1] == DP104_PLC_CAL_RAW_PULSE)
    {
        const uint8_t raw_channel = request[4];
        const uint8_t intensity = request[5];
        if(raw_channel > 2u || intensity > 64u ||
           !all_zero(request, 6, DP104_REPORT_BYTES))
            return DP104_BAD_REQUEST;

        primask = irq_save();
        if(state->color_order_verified)
        {
            irq_restore(primask);
            return DP104_BAD_STATE;
        }
        output = key_buffer();
        state->override_active = 1;
        for(led = 0; led < DP104_LED_COUNT; ++led)
            for(channel = 0; channel < 3; ++channel)
                output[led * 3u + channel] = channel == raw_channel ? intensity : 0;
        state->diagnostic_active = 1;
        state->diagnostic_started = now_ms;
        *dirty_flag() = 1;
        irq_restore(primask);
        return DP104_OK;
    }

    if(request[1] == DP104_PLC_CAL_SET_ORDER)
    {
        const uint8_t order[3] = {request[4], request[5], request[6]};
        if(!valid_color_order(order) || request[7] != 'U' || request[8] != 'S' ||
           request[9] != 'E' || request[10] != 'R' ||
           all_zero(request, 11, 27) || !all_zero(request, 27, DP104_REPORT_BYTES))
            return DP104_BAD_REQUEST;

        primask = irq_save();
        if(state->color_order_verified)
        {
            const int same = state->color_order[0] == order[0] &&
                             state->color_order[1] == order[1] &&
                             state->color_order[2] == order[2];
            irq_restore(primask);
            return same ? DP104_OK : DP104_BAD_STATE;
        }
        state->color_order[0] = order[0];
        state->color_order[1] = order[1];
        state->color_order[2] = order[2];
        state->color_order_verified = 1;
        state->diagnostic_active = 0;
        state->diagnostic_started = 0;
        state->override_active = 0;
        *dirty_flag() = 1;
        irq_restore(primask);
        return DP104_OK;
    }
    return DP104_BAD_REQUEST;
}

static void process_request(dp104_plc_patch_state *state,
                            const uint8_t request[DP104_REPORT_BYTES],
                            uint8_t response[DP104_REPORT_BYTES], uint32_t now_ms)
{
    uint8_t status;
    response_header(response, request, DP104_OK);
    if(state->initialized != DP104_PLC_TARGET_MAGIC)
    {
        response[3] = DP104_BUSY;
        return;
    }
    if(request[0] == DP104_COMMAND && request[1] == APP_INFO_OP)
    {
        handle_app_info(state, request, response);
        return;
    }
    if(request[0] == DP104_COMMAND &&
       request[1] == DP104_PLC_RECEIVER_SNAPSHOT_OP)
    {
        handle_receiver_snapshot(request, response, &state->receiver);
        return;
    }
    if(request[0] == DP104_COMMAND &&
       request[1] == DP104_PLC_DIAGNOSTIC_OP)
    {
        const uint32_t primask = irq_save();
        dp104_plc_diagnostic_handle(&state->diagnostic, request, response);
        irq_restore(primask);
        return;
    }
    if(request[0] == DP104_COMMAND &&
       (request[1] == DP104_PLC_CAL_RAW_PULSE ||
        request[1] == DP104_PLC_CAL_SET_ORDER))
    {
        status = handle_calibration(state, request, now_ms);
        response[3] = status;
        if(status == DP104_OK && request[1] == DP104_PLC_CAL_SET_ORDER)
        {
            memcpy(response + 4, state->color_order, 3);
            memcpy(response + 7, request + 11, 16);
        }
        return;
    }
    /* Only a well-formed CAPS request is special-cased. Malformed CAPS retains
     * the receiver core's existing validation/error priority. */
    if(request[1] == DP104_OP_CAPS && valid_caps_request(request) &&
       !target_ready(state))
    {
        response[3] = DP104_BUSY;
        return;
    }
    dp104_receiver_handle(&state->receiver, request, DP104_REPORT_BYTES,
                          response, now_ms);
}

typedef struct dp104_route_context {
    dp104_plc_patch_state *state;
    uint32_t now_ms;
    unsigned legacy_length;
    int legacy_overflow;
    size_t extension_bytes;
    size_t input_bytes;
    uint16_t stream_used_before;
    uint16_t stream_used_after;
    uint8_t dropping_before;
    uint8_t dropping_after;
} dp104_route_context;

static uint16_t receiver_received_led_count(const dp104_receiver *receiver)
{
    unsigned i, bit;
    uint16_t count = 0;
    for(i = 0; i < sizeof(receiver->received); ++i)
        for(bit = 0; bit < 8u; ++bit)
            if(receiver->received[i] & (uint8_t)(1u << bit)) ++count;
    return count;
}

static void note_diagnostic_extension(dp104_plc_patch_state *state,
                                     const uint8_t request[DP104_REPORT_BYTES],
                                     uint16_t wire_bytes,
                                     uint16_t stream_used_before,
                                     uint16_t stream_used_after,
                                     uint8_t dropping_before,
                                     uint8_t dropping_after)
{
    uint32_t primask = irq_save();
    dp104_plc_diagnostic_note_extension(&state->diagnostic, request,
        wire_bytes, stream_used_before, stream_used_after,
        dropping_before, dropping_after);
    dp104_plc_diagnostic_note_decoded(&state->diagnostic, request);
    irq_restore(primask);
}

static void note_diagnostic_parser(dp104_plc_patch_state *state, uint8_t event,
                                  uint16_t bytes,
                                  uint16_t stream_used_before,
                                  uint16_t stream_used_after,
                                  uint8_t dropping_before,
                                  uint8_t dropping_after)
{
    uint32_t primask = irq_save();
    dp104_plc_diagnostic_note_parser(&state->diagnostic, event, bytes,
        stream_used_before, stream_used_after, dropping_before,
        dropping_after);
    irq_restore(primask);
}

static void extension_sink(void *context, const uint8_t *frame, size_t length)
{
    dp104_route_context *route = (dp104_route_context *)context;
    uint8_t request[DP104_REPORT_BYTES];
    uint8_t response[DP104_REPORT_BYTES];
    uint8_t reply[DP104_WIRE_BYTES];
    size_t reply_length;
    size_t encoded_length = 0;
    uint16_t commit_received_leds = 0;
    int encoded;
    if(!dp104_wire_decode(DP104_WIRE_REQUEST, frame, length,
                          request, sizeof(request)))
        return;
    route->extension_bytes += length;
    if(request[1] <= DP104_OP_COMMIT)
        note_diagnostic_extension(route->state, request,
            (uint16_t)length, (uint16_t)route->stream_used_before,
            (uint16_t)route->stream_used_after, route->dropping_before,
            route->dropping_after);
    if(request[1] == DP104_OP_COMMIT)
        commit_received_leds =
            receiver_received_led_count(&route->state->receiver);
    process_request(route->state, request, response, route->now_ms);
    if(request[1] == DP104_OP_COMMIT)
    {
        const uint32_t primask = irq_save();
        dp104_plc_diagnostic_note_commit(&route->state->diagnostic, request,
            response[3], commit_received_leds);
        irq_restore(primask);
    }
    encoded = dp104_wire_encode(DP104_WIRE_RESPONSE, response,
                                sizeof(response), reply, sizeof(reply));
    /* Success means the fixed encoder wrote exactly DP104_WIRE_BYTES.
     * This 46-byte reply never needs the old divisible-by-seven tail padding. */
    encoded_length = encoded ? sizeof(reply) : 0u;
    if(request[1] <= DP104_OP_COMMIT)
    {
        const uint32_t primask = irq_save();
        dp104_plc_diagnostic_note_reply(&route->state->diagnostic, request,
            response[3], encoded, (uint16_t)encoded_length);
        irq_restore(primask);
    }
    if(encoded && (reply_length = encoded_length) != 0)
    {
        /* Use the stock asynchronous contract. The helper first copies into
         * its shared DMA buffer, then records busy=1 so the next caller drains
         * completion and disables the channel before reusing it. Mixing a
         * synchronous completion with stock asynchronous sends skips that
         * cleanup because mode 0 leaves EN set but no busy marker. */
        uart_send(reply, (uint32_t)reply_length, 1);
        if(request[1] <= DP104_OP_COMMIT)
        {
            const uint32_t primask = irq_save();
            dp104_plc_diagnostic_note_uart_send(&route->state->diagnostic,
                request, response[3], (uint32_t)reply_length, 1u);
            irq_restore(primask);
        }
    }
}

static void legacy_sink(void *context, const uint8_t *bytes, size_t length)
{
    dp104_route_context *route = (dp104_route_context *)context;
    if(length > DP104_PLC_QUEUE_DATA_BYTES - route->legacy_length)
    {
        route->legacy_overflow = 1;
        return;
    }
    memcpy(route->state->legacy_slot + route->legacy_length, bytes, length);
    route->legacy_length += (unsigned)length;
}

static void copy_raw_to_legacy(dp104_plc_patch_state *state,
                               const uint8_t *bytes, unsigned length,
                               uint8_t **out)
{
    if(length > DP104_PLC_QUEUE_DATA_BYTES) length = DP104_PLC_QUEUE_DATA_BYTES;
    memcpy(state->legacy_slot, bytes, length);
    state->legacy_slot[DP104_PLC_QUEUE_DATA_BYTES] = (uint8_t)length;
    *out = length ? state->legacy_slot : 0;
}

void dp104_plc_queue_hook(uint8_t **out)
{
    uint8_t local_batch[DP104_PLC_QUEUE_SLOT_BYTES];
    uint8_t *source = 0;
    uint32_t primask, now_ms;
    unsigned length, i;
    size_t stream_length = 0;
    uint8_t stream_is_extension = 0;
    uint16_t stream_used_before, stream_used_after;
    uint8_t dropping_before, dropping_after;
    int stream_result;
    dp104_plc_patch_state *state = &__dp104_plc_state;

    now_ms = target_millis();
    if(!out)
    {
        poll_at(now_ms);
        return;
    }
    *out = 0;

    /* Parse/copy one raw-stream record while IRQs are excluded. If it is
     * incomplete, retain it and consult the original queue as a fallback. */
    primask = irq_save();
    stream_used_before = state->uart_stream.used;
    dropping_before = state->uart_stream.dropping;
    stream_result = dp104_wire_stream_next(&state->uart_stream, local_batch,
                                           DP104_PLC_QUEUE_DATA_BYTES,
                                           &stream_length, &stream_is_extension);
    stream_used_after = state->uart_stream.used;
    dropping_after = state->uart_stream.dropping;
    if(stream_result <= 0 &&
       (stream_used_before != stream_used_after ||
        dropping_before != dropping_after))
    {
        const uint16_t discarded = stream_used_before > stream_used_after ?
            (uint16_t)(stream_used_before - stream_used_after) : 0;
        dp104_plc_diagnostic_note_parser(&state->diagnostic,
            DP104_PLC_DIAG_PARSER_DISCARD, discarded,
            stream_used_before, stream_used_after, dropping_before,
            dropping_after);
    }
    if(stream_result <= 0)
    {
        /* The stock queue slot can be reused immediately after IRQ restore. */
        legacy_queue_pop(&source);
        if(source)
            for(i = 0; i < DP104_PLC_QUEUE_SLOT_BYTES; ++i)
                local_batch[i] = source[i];
    }
    irq_restore(primask);
    *out = 0;

    if(stream_result > 0)
    {
        poll_at(now_ms);
        if(stream_is_extension)
        {
            dp104_route_context route;
            route.state = state;
            route.now_ms = now_ms;
            route.legacy_length = 0;
            route.legacy_overflow = 0;
            route.extension_bytes = 0;
            route.input_bytes = stream_length;
            route.stream_used_before = stream_used_before;
            route.stream_used_after = stream_used_after;
            route.dropping_before = dropping_before;
            route.dropping_after = dropping_after;
            extension_sink(&route, local_batch, stream_length);
        }
        else
            copy_raw_to_legacy(state, local_batch, (unsigned)stream_length, out);
        return;
    }

    if(!source)
    {
        poll_at(now_ms);
        return;
    }

    length = local_batch[DP104_PLC_QUEUE_DATA_BYTES];
    if(length > DP104_PLC_QUEUE_DATA_BYTES)
        length = DP104_PLC_QUEUE_DATA_BYTES; /* Stock callback already caps at 67. */
    if(state->initialized != DP104_PLC_TARGET_MAGIC)
    {
        copy_raw_to_legacy(state, local_batch, length, out);
        return;
    }

    poll_at(now_ms);
    memset(state->legacy_slot, 0, sizeof(state->legacy_slot));
    {
        dp104_route_context route;
        route.state = state;
        route.now_ms = now_ms;
        route.legacy_length = 0;
        route.legacy_overflow = 0;
        route.extension_bytes = 0;
        route.input_bytes = length;
        route.stream_used_before = 0;
        route.stream_used_after = 0;
        route.dropping_before = 0;
        route.dropping_after = 0;
        (void)dp104_wire_route_batch(DP104_WIRE_REQUEST, local_batch, length,
                                     extension_sink, legacy_sink, &route);
        if(route.input_bytes > route.legacy_length + route.extension_bytes)
        {
            const size_t discarded = route.input_bytes -
                                     route.legacy_length -
                                     route.extension_bytes;
            note_diagnostic_parser(state,
                DP104_PLC_DIAG_PARSER_BATCH_DISCARD,
                (uint16_t)discarded, 0, 0, 0, 0);
        }
        if(route.legacy_overflow)
        {
            /* This cannot occur for a 67-byte input batch; preserve a bounded
             * raw copy rather than returning a pointer into the stack. */
            copy_raw_to_legacy(state, local_batch, length, out);
            return;
        }
        if(route.legacy_length)
        {
            state->legacy_slot[DP104_PLC_QUEUE_DATA_BYTES] =
                (uint8_t)route.legacy_length;
            *out = state->legacy_slot;
        }
    }
}
