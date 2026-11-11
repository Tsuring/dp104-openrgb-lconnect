#ifndef DP104_USB_STAGE_H
#define DP104_USB_STAGE_H
#include <stddef.h>
#include <stdint.h>
#include "dp104_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Zero-initialize before first use; never reinitialize during a transfer.
 * Persistent, four-byte-aligned storage for a USB API that retains its input.
 * It must outlive every queued transfer. busy() must observe real completion,
 * and queue() must set that busy condition before returning success. A failed
 * queue must not retain the pointer. enter/leave serialize the entire check,
 * copy and queue operation with the endpoint completion ISR and other writers.
 */
typedef struct dp104_usb_stage {
    uint32_t words[DP104_REPORT_BYTES / sizeof(uint32_t)];
    uintptr_t port;
    uint8_t pending;
} dp104_usb_stage;

typedef struct dp104_usb_stage_hooks {
    void *context;
    uint32_t (*enter)(void *context);
    void (*leave)(void *context, uint32_t saved);
    int (*busy)(void *context, uintptr_t port);
    int (*queue)(void *context, uintptr_t port, const uint8_t *payload, unsigned length);
} dp104_usb_stage_hooks;

int dp104_usb_stage_send(dp104_usb_stage *stage, dp104_usb_stage_hooks hooks,
                         uintptr_t port, const uint8_t *payload, size_t length);

#ifdef __cplusplus
}
#endif
#endif
