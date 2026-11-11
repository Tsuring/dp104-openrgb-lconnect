#include "dp104_master_bridge.h"
#include "dp104_receiver.h"
#include "dp104_wire.h"
#include "dp104_lz.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct cpu_test_state {
    dp104_receiver receiver;
    dp104_master_bridge bridge;
    uint8_t visible[DP104_FRAME_BYTES];
    uint8_t plc_wire[DP104_WIRE_BYTES];
    uint8_t plc_payload[DP104_REPORT_BYTES];
    uint8_t plc_response[DP104_REPORT_BYTES];
    uint8_t usb_response[DP104_REPORT_BYTES];
    unsigned published;
    unsigned released;
    unsigned plc_sends;
    unsigned usb_sends;
    uintptr_t usb_port;
    uint32_t now;
} cpu_test_state;

static cpu_test_state state;

static int output_ready(void *context)
{
    (void)context;
    return 1;
}

static int publish_frame(void *context, const uint8_t *rgb, unsigned leds)
{
    cpu_test_state *s = (cpu_test_state *)context;
    if(!rgb || leds != DP104_LED_COUNT)
    {
        return 0;
    }
    memcpy(s->visible, rgb, DP104_FRAME_BYTES);
    ++s->published;
    return 1;
}

static void release_frame(void *context)
{
    cpu_test_state *s = (cpu_test_state *)context;
    ++s->released;
}

static uint32_t enter_bridge(void *context)
{
    (void)context;
    return 0;
}

static void leave_bridge(void *context, uint32_t saved)
{
    (void)context;
    (void)saved;
}

static int send_to_plc(void *context, const uint8_t *payload, unsigned length)
{
    cpu_test_state *s = (cpu_test_state *)context;
    if(!payload || length != DP104_REPORT_BYTES ||
       !dp104_wire_encode(DP104_WIRE_REQUEST, payload, length,
                          s->plc_wire, sizeof(s->plc_wire)))
    {
        return 0;
    }
    ++s->plc_sends;
    return 1;
}

static int send_to_usb(void *context, uintptr_t port,
                       const uint8_t *payload, unsigned length)
{
    cpu_test_state *s = (cpu_test_state *)context;
    if(!payload || length != DP104_REPORT_BYTES)
    {
        return 0;
    }
    memcpy(s->usb_response, payload, DP104_REPORT_BYTES);
    s->usb_port = port;
    ++s->usb_sends;
    return 1;
}

static void init_core(void)
{
    dp104_output_hooks output = {
        &state, output_ready, publish_frame, release_frame
    };
    dp104_bridge_hooks bridge = {
        &state, enter_bridge, leave_bridge, send_to_plc, send_to_usb
    };
    memset(&state, 0, sizeof(state));
    dp104_receiver_init(&state.receiver, output);
    dp104_bridge_init(&state.bridge, bridge);
}

static int test_wire_and_deferred_bridge(void)
{
    static const uint8_t magic[8] = {'D', 'P', '1', '0', '4', 'R', 'G', 'B'};
    static const uint8_t stock_report[11] = {0x0b, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t request[DP104_REPORT_BYTES] = {0};
    uint8_t response[DP104_REPORT_BYTES] = {0};
    uint8_t wrong_response[DP104_REPORT_BYTES] = {0};
    uintptr_t const port = (uintptr_t)0x13579u;

    request[0] = DP104_COMMAND;
    request[1] = DP104_OP_CAPS;
    request[2] = 0x51;

    if(dp104_wire_encode(DP104_WIRE_REQUEST, request, sizeof(request),
                         state.plc_wire, sizeof(state.plc_wire)) != 1 ||
       state.plc_wire[0] != DP104_WIRE_REQUEST ||
       state.plc_wire[1] != DP104_WIRE_TYPE ||
       state.plc_wire[2] < DP104_REPORT_BYTES ||
       dp104_wire_decode(DP104_WIRE_REQUEST, state.plc_wire, dp104_wire_frame_size(state.plc_wire, sizeof(state.plc_wire)),
                         state.plc_payload, sizeof(state.plc_payload)) != 1 ||
       memcmp(state.plc_payload, request, sizeof(request)) != 0)
    {
        return 10;
    }
    if(dp104_wire_decode(DP104_WIRE_RESPONSE, state.plc_wire, dp104_wire_frame_size(state.plc_wire, sizeof(state.plc_wire)),
                         state.plc_payload, sizeof(state.plc_payload)) != 0)
    {
        return 11;
    }

    if(dp104_bridge_on_host(&state.bridge, port, stock_report, sizeof(stock_report)) != 0 ||
       state.plc_sends != 0 || state.usb_sends != 0)
    {
        return 12;
    }
    if(dp104_bridge_on_host(&state.bridge, port, request, sizeof(request)) != 1 ||
       state.plc_sends != 0 || state.usb_sends != 0)
    {
        return 13;
    }

    /* The bridge queues in the host callback and transmits only in pump(). */
    dp104_bridge_pump(&state.bridge);
    if(state.plc_sends != 1 || state.usb_sends != 0 ||
       dp104_wire_decode(DP104_WIRE_REQUEST, state.plc_wire, dp104_wire_frame_size(state.plc_wire, sizeof(state.plc_wire)),
                         state.plc_payload, sizeof(state.plc_payload)) != 1)
    {
        return 14;
    }

    /* The receiver, not the bridge, produces the protocol response. */
    dp104_receiver_handle(&state.receiver, state.plc_payload,
                          sizeof(state.plc_payload), response, state.now++);
    if(response[0] != DP104_COMMAND || response[1] != DP104_OP_CAPS ||
       response[2] != request[2] || response[3] != DP104_OK ||
       memcmp(&response[4], magic, sizeof(magic)) != 0)
    {
        return 15;
    }
    if(dp104_wire_encode(DP104_WIRE_RESPONSE, response, sizeof(response),
                         state.plc_wire, sizeof(state.plc_wire)) != 1 ||
       dp104_wire_decode(DP104_WIRE_RESPONSE, state.plc_wire, dp104_wire_frame_size(state.plc_wire, sizeof(state.plc_wire)),
                         state.plc_response, sizeof(state.plc_response)) != 1)
    {
        return 16;
    }

    memcpy(wrong_response, state.plc_response, sizeof(wrong_response));
    wrong_response[2] = (uint8_t)(wrong_response[2] + 1u);
    dp104_bridge_on_plc(&state.bridge, wrong_response, sizeof(wrong_response));
    dp104_bridge_pump(&state.bridge);
    if(state.usb_sends != 0)
    {
        return 17;
    }

    dp104_bridge_on_plc(&state.bridge, state.plc_response,
                        sizeof(state.plc_response));
    if(state.usb_sends != 0)
    {
        return 18;
    }
    dp104_bridge_pump(&state.bridge);
    if(state.usb_sends != 1 || state.usb_port != port ||
       memcmp(state.usb_response, state.plc_response, DP104_REPORT_BYTES) != 0)
    {
        return 19;
    }
    return 0;
}

static int test_receiver_frame_and_lease(void)
{
    uint8_t request[DP104_REPORT_BYTES];
    uint8_t response[DP104_REPORT_BYTES];
    uint8_t frame[DP104_FRAME_BYTES];
    uint32_t committed_at;
    unsigned led_index;

    state.now = 0xfffffff0u;
    for(led_index = 0; led_index < DP104_LED_COUNT; ++led_index)
    {
        frame[led_index * 3u] = (uint8_t)(led_index * 13u + 7u);
        frame[led_index * 3u + 1u] = (uint8_t)(led_index * 5u + 11u);
        frame[led_index * 3u + 2u] = (uint8_t)(led_index * 3u + 19u);
    }

    memset(request, 0, sizeof(request));
    request[0] = DP104_COMMAND;
    request[1] = DP104_OP_BEGIN;
    request[2] = 0xa5;
    dp104_write16(&request[4], DP104_LED_COUNT);
    dp104_write32(&request[8], dp104_crc32(frame, sizeof(frame)));
    dp104_receiver_handle(&state.receiver, request, sizeof(request), response, state.now++);
    if(response[3] != DP104_OK || !state.receiver.staging_active)
    {
        return 20;
    }

    for(led_index = 0; led_index < DP104_LED_COUNT; led_index += DP104_CHUNK_LEDS)
    {
        memset(request, 0, sizeof(request));
        request[0] = DP104_COMMAND;
        request[1] = DP104_OP_DATA;
        request[2] = 0xa5;
        dp104_write16(&request[4], (uint16_t)led_index);
        request[6] = DP104_CHUNK_LEDS;
        memcpy(&request[7], &frame[led_index * 3u], DP104_CHUNK_LEDS * 3u);
        dp104_receiver_handle(&state.receiver, request, sizeof(request), response, state.now++);
        if(response[3] != DP104_OK)
        {
            return 21;
        }
    }

    memset(request, 0, sizeof(request));
    request[0] = DP104_COMMAND;
    request[1] = DP104_OP_COMMIT;
    request[2] = 0xa5;
    dp104_receiver_handle(&state.receiver, request, sizeof(request), response, state.now++);
    committed_at = state.receiver.committed_at;
    if(response[3] != DP104_OK || !state.receiver.direct_active ||
       state.published != 1 || state.released != 0 ||
       memcmp(state.visible, frame, sizeof(frame)) != 0)
    {
        return 22;
    }

    dp104_receiver_poll(&state.receiver, committed_at + DP104_LEASE_MS - 1u);
    if(!state.receiver.direct_active || state.released != 0)
    {
        return 23;
    }
    dp104_receiver_poll(&state.receiver, committed_at + DP104_LEASE_MS);
    if(state.receiver.direct_active || state.released != 1)
    {
        return 24;
    }
    return 0;
}

static int run_tests(void)
{
    static const uint8_t crc_input[] = "123456789";
    int result;
    static const uint8_t packed[] = {2, 'A', 15, 0};
    static const uint8_t malformed[] = {1, 0, 0};
    uint8_t decoded[21];
    unsigned i;

    if(sizeof(void *) != 4u)
    {
        return 1;
    }
    if(dp104_crc32(crc_input, sizeof(crc_input) - 1u) != 0xcbf43926u)
    {
        return 2;
    }
    memset(decoded, 0xa5, sizeof(decoded));
    if(!dp104_lz_decode(packed, sizeof(packed), decoded + 1, 19) ||
       decoded[0] != 0xa5 || decoded[20] != 0xa5) return 40;
    for(i = 1; i < 20; ++i) if(decoded[i] != 'A') return 41;
    if(dp104_lz_decode(malformed, sizeof(malformed), decoded + 1, 19)) return 42;
    if(decoded[0] != 0xa5 || decoded[20] != 0xa5) return 43;

    init_core();
    result = test_wire_and_deferred_bridge();
    if(result != 0)
    {
        return result;
    }
    if(state.published != 0 || state.released != 0)
    {
        return 3;
    }
    return test_receiver_frame_and_lease();
}

static __attribute__((noreturn)) void linux_exit(int status)
{
#if defined(__riscv)
    register long argument __asm__("a0") = status;
    register long syscall_number __asm__("a7") = 93;
    __asm__ volatile("ecall" : : "r"(argument), "r"(syscall_number) : "memory");
#elif defined(__arm__)
    register long argument __asm__("r0") = status;
    register long syscall_number __asm__("r7") = 1;
    __asm__ volatile("svc #0" : : "r"(argument), "r"(syscall_number) : "memory");
#else
#error This harness supports only RV32 and ARM32.
#endif
    __builtin_unreachable();
}

__attribute__((noreturn, used)) void _start(void)
{
    linux_exit(run_tests());
}
