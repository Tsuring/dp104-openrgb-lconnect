#include "dp104_master_bridge.h"
#include <string.h>

static int configured(const dp104_master_bridge *b)
{
    return b->hooks.enter && b->hooks.leave && b->hooks.send_plc && b->hooks.send_usb;
}

void dp104_bridge_init(dp104_master_bridge *b, dp104_bridge_hooks hooks)
{
    memset(b, 0, sizeof(*b));
    b->hooks = hooks;
}

int dp104_bridge_on_host(dp104_master_bridge *b, uintptr_t port,
                         const uint8_t *payload, size_t length)
{
    uint32_t saved;
    if(!payload || !length || payload[0] != DP104_COMMAND) return 0;
    if(length != DP104_REPORT_BYTES || payload[3] || !configured(b)) return 1;
    saved = b->hooks.enter(b->hooks.context);
    memcpy(b->request, payload, DP104_REPORT_BYTES);
    b->port = port;
    ++b->generation;
    b->expected_op = payload[1];
    b->expected_transaction = payload[2];
    b->request_ready = 1;
    b->response_ready = 0;
    b->waiting = 0;
    b->hooks.leave(b->hooks.context, saved);
    return 1;
}

void dp104_bridge_on_plc(dp104_master_bridge *b, const uint8_t *payload, size_t length)
{
    uint32_t saved;
    if(!payload || length != DP104_REPORT_BYTES || !configured(b)) return;
    saved = b->hooks.enter(b->hooks.context);
    if(b->waiting && payload[0] == DP104_COMMAND &&
       payload[1] == b->expected_op && payload[2] == b->expected_transaction)
    {
        memcpy(b->response, payload, DP104_REPORT_BYTES);
        b->response_ready = 1;
        b->waiting = 0;
    }
    b->hooks.leave(b->hooks.context, saved);
}

void dp104_bridge_pump(dp104_master_bridge *b)
{
    uint8_t report[DP104_REPORT_BYTES];
    uintptr_t port = 0;
    uint32_t saved, generation = 0;
    int outgoing = 0, incoming = 0;
    if(!configured(b)) return;

    saved = b->hooks.enter(b->hooks.context);
    if(b->request_ready)
    {
        memcpy(report, b->request, sizeof(report));
        generation = b->generation;
        b->request_ready = 0;
        b->waiting = 1;
        outgoing = 1;
    }
    else if(b->response_ready)
    {
        memcpy(report, b->response, sizeof(report));
        generation = b->generation;
        port = b->port;
        b->response_ready = 0;
        incoming = 1;
    }
    b->hooks.leave(b->hooks.context, saved);

    if(outgoing && !b->hooks.send_plc(b->hooks.context, report, sizeof(report)))
    {
        saved = b->hooks.enter(b->hooks.context);
        if(b->generation == generation)
        {
            b->waiting = 0;
            b->response_ready = 0;
        }
        b->hooks.leave(b->hooks.context, saved);
    }
    if(incoming && !b->hooks.send_usb(b->hooks.context, port, report, sizeof(report)))
    {
        saved = b->hooks.enter(b->hooks.context);
        if(b->generation == generation)
        {
            memcpy(b->response, report, sizeof(report));
            b->response_ready = 1;
        }
        b->hooks.leave(b->hooks.context, saved);
    }
}
