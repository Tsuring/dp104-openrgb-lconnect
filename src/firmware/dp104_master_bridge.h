#ifndef DP104_MASTER_BRIDGE_H
#define DP104_MASTER_BRIDGE_H

#include <stddef.h>
#include <stdint.h>
#include "dp104_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The interrupt-facing entry points only copy bounded reports. pump() runs in
 * the normal task context; it performs UART/USB transmission. The platform
 * must serialize queue state with interrupt save/restore, not a busy-wait lock.
 * send_plc handles the verified physical framing; it must not fabricate ACKs.
 * No stock peripheral addresses or assumed packet types are embedded here.
 * send_usb must consume/copy its input before returning: pump's report is
 * stack-local. APIs that retain a DMA pointer need persistent staging plus a
 * real completion/busy check, such as dp104_usb_stage_send().
 */
typedef struct dp104_bridge_hooks {
    void *context;
    uint32_t (*enter)(void *context);
    void (*leave)(void *context, uint32_t saved);
    int (*send_plc)(void *context, const uint8_t *payload, unsigned length);
    int (*send_usb)(void *context, uintptr_t port, const uint8_t *payload, unsigned length);
} dp104_bridge_hooks;

typedef struct dp104_master_bridge {
    dp104_bridge_hooks hooks;
    uint8_t request[DP104_REPORT_BYTES];
    uint8_t response[DP104_REPORT_BYTES];
    uintptr_t port;
    uint32_t generation;
    uint8_t expected_op;
    uint8_t expected_transaction;
    uint8_t request_ready;
    uint8_t response_ready;
    uint8_t waiting;
} dp104_master_bridge;

void dp104_bridge_init(dp104_master_bridge *bridge, dp104_bridge_hooks hooks);
/* Returns 0 for stock reports so the original handler runs unchanged.
 * Returns 1 for D2, including malformed/unsupported D2. These are never echoed
 * as a successful local ACK. A new request supersedes a timed-out one. */
int dp104_bridge_on_host(dp104_master_bridge *bridge, uintptr_t port,
                         const uint8_t *payload, size_t length);
/* Called only after the platform has validated its PLC frame envelope. */
void dp104_bridge_on_plc(dp104_master_bridge *bridge,
                          const uint8_t *payload, size_t length);
/* Non-reentrant; call from one regular firmware task, never an ISR. */
void dp104_bridge_pump(dp104_master_bridge *bridge);

#ifdef __cplusplus
}
#endif
#endif
