#ifndef DP104_RECEIVER_H
#define DP104_RECEIVER_H

#include <stddef.h>
#include <stdint.h>
#include "dp104_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Hardware binding contract:
 * ready: true only after both master/PLC transport and output hooks work.
 * publish: atomically acquire direct ownership and publish ALL 104 RGB triples;
 *          false must leave output and ownership unchanged. Consume/copy the
 *          data synchronously; the receiver reuses its staging buffer.
 * release: restore the pre-existing effect producer without changing settings.
 * Poll on the normal task tick, including when the host is disconnected.
 * Call this API from one task, or serialize it externally. No ISR-side use.
 */
typedef struct dp104_output_hooks {
    void *context;
    int (*ready)(void *context);
    int (*publish)(void *context, const uint8_t *rgb, unsigned leds);
    void (*release)(void *context);
} dp104_output_hooks;

typedef struct dp104_receiver {
    dp104_output_hooks hooks;
    uint8_t staging[DP104_FRAME_BYTES];
    uint8_t received[(DP104_LED_COUNT + 7u) / 8u];
    uint32_t expected_crc;
    uint32_t began_at;
    uint32_t committed_at;
    uint8_t transaction;
    uint8_t active_transaction;
    uint8_t staging_active;
    uint8_t direct_active;
} dp104_receiver;

void dp104_receiver_init(dp104_receiver *receiver, dp104_output_hooks hooks);
void dp104_receiver_poll(dp104_receiver *receiver, uint32_t now_ms);
void dp104_receiver_handle(dp104_receiver *receiver,
                           const uint8_t *request, size_t request_length,
                           uint8_t response[DP104_REPORT_BYTES], uint32_t now_ms);

#ifdef __cplusplus
}
#endif
#endif
