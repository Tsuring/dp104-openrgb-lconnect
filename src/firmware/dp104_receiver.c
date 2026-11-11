#include "dp104_receiver.h"
#include <string.h>

static int is_ready(const dp104_receiver *r)
{
    return r->hooks.ready && r->hooks.publish && r->hooks.release &&
           r->hooks.ready(r->hooks.context);
}

static int zero_bytes(const uint8_t *p, unsigned start, unsigned end)
{
    unsigned i;
    for(i = start; i < end; ++i)
        if(p[i]) return 0;
    return 1;
}

static void release_output(dp104_receiver *r)
{
    if(r->direct_active && r->hooks.release)
        r->hooks.release(r->hooks.context);
    r->direct_active = 0;
    r->active_transaction = 0;
}

void dp104_receiver_init(dp104_receiver *r, dp104_output_hooks hooks)
{
    memset(r, 0, sizeof(*r));
    r->hooks = hooks;
}

void dp104_receiver_poll(dp104_receiver *r, uint32_t now_ms)
{
    if(r->staging_active && (uint32_t)(now_ms - r->began_at) >= DP104_LEASE_MS)
        r->staging_active = 0;
    if(r->direct_active &&
       ((uint32_t)(now_ms - r->committed_at) >= DP104_LEASE_MS || !is_ready(r)))
        release_output(r);
}

void dp104_receiver_handle(dp104_receiver *r, const uint8_t *request,
                          size_t length, uint8_t response[DP104_REPORT_BYTES],
                          uint32_t now_ms)
{
    uint8_t packet[DP104_REPORT_BYTES] = {0};
    uint8_t status = DP104_OK;
    unsigned i;

    /* Support the in-place report buffers commonly used by HID handlers. */
    if(request) memcpy(packet, request, length < sizeof(packet) ? length : sizeof(packet));
    memset(response, 0, DP104_REPORT_BYTES);
    memcpy(response, packet, 3);
    dp104_receiver_poll(r, now_ms);

    if(!request || length != DP104_REPORT_BYTES) status = DP104_BAD_LENGTH;
    else if(packet[0] != DP104_COMMAND) status = DP104_UNSUPPORTED;
    else if(packet[3]) status = DP104_BAD_REQUEST;
    else if(!is_ready(r)) status = DP104_UNSUPPORTED;
    else switch(packet[1])
    {
        case DP104_OP_CAPS:
            if(!zero_bytes(packet, 4, DP104_REPORT_BYTES))
            {
                status = DP104_BAD_REQUEST;
                break;
            }
            memcpy(response + 4, DP104_CAPS_MAGIC, 8);
            response[12] = DP104_PROTOCOL_MAJOR;
            dp104_write16(response + 14, DP104_LED_COUNT);
            response[16] = DP104_CHUNK_LEDS;
            response[17] = 1;
            dp104_write16(response + 18, DP104_LEASE_MS);
            break;
        case DP104_OP_BEGIN:
            if(!packet[2] || dp104_read16(packet + 4) != DP104_LED_COUNT ||
               !zero_bytes(packet, 6, 8) || !zero_bytes(packet, 12, DP104_REPORT_BYTES))
            {
                status = DP104_BAD_REQUEST;
                break;
            }
            memset(r->received, 0, sizeof(r->received));
            r->transaction = packet[2];
            r->expected_crc = dp104_read32(packet + 8);
            r->began_at = now_ms;
            r->staging_active = 1;
            break;
        case DP104_OP_DATA:
        {
            const unsigned start = dp104_read16(packet + 4);
            const unsigned count = packet[6];
            if(!r->staging_active || packet[2] != r->transaction)
            {
                status = DP104_BAD_STATE;
                break;
            }
            if(!count || count > DP104_CHUNK_LEDS || start >= DP104_LED_COUNT ||
               count > DP104_LED_COUNT - start)
            {
                status = DP104_BAD_INDEX;
                break;
            }
            if(!zero_bytes(packet, 7u + count * 3u, DP104_REPORT_BYTES))
            {
                status = DP104_BAD_REQUEST;
                break;
            }
            memcpy(r->staging + start * 3u, packet + 7, count * 3u);
            for(i = start; i < start + count; ++i)
                r->received[i / 8u] |= (uint8_t)(1u << (i % 8u));
            break;
        }
        case DP104_OP_COMMIT:
            if(!zero_bytes(packet, 4, DP104_REPORT_BYTES))
                status = DP104_BAD_REQUEST;
            else if(!r->staging_active || packet[2] != r->transaction)
                status = DP104_BAD_STATE;
            else
            {
                for(i = 0; i < DP104_LED_COUNT; ++i)
                    if(!(r->received[i / 8u] & (1u << (i % 8u)))) break;
                if(i != DP104_LED_COUNT) status = DP104_INCOMPLETE;
                else if(dp104_crc32(r->staging, DP104_FRAME_BYTES) != r->expected_crc)
                    status = DP104_BAD_CRC;
                else if(!r->hooks.publish(r->hooks.context, r->staging, DP104_LED_COUNT))
                    status = DP104_BUSY;
                else
                {
                    r->direct_active = 1;
                    r->active_transaction = r->transaction;
                    r->committed_at = now_ms;
                    r->staging_active = 0;
                }
            }
            break;
        case DP104_OP_RELEASE:
            if(!zero_bytes(packet, 4, DP104_REPORT_BYTES)) status = DP104_BAD_REQUEST;
            else
            {
                release_output(r);
                r->staging_active = 0;
            }
            break;
        case DP104_OP_STATUS:
            if(!zero_bytes(packet, 4, DP104_REPORT_BYTES)) status = DP104_BAD_REQUEST;
            else
            {
                response[4] = r->direct_active;
                response[5] = r->staging_active;
                response[6] = r->active_transaction;
            }
            break;
        default:
            status = DP104_BAD_REQUEST;
            break;
    }
    response[3] = status;
}
