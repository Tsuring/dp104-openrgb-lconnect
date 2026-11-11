#ifndef DP104_PATCH_STATE_H
#define DP104_PATCH_STATE_H

#include <stdint.h>
#include "dp104_master_bridge.h"
#include "dp104_receiver.h"
#include "dp104_usb_stage.h"
#include "dp104_twiddle.h"
#include "dp104_wire_stream.h"

/* Prospective target-owned storage. No instance is placed by this header.
 * The target planner measures these types with each target compiler. A magic
 * field alone must never be used as evidence that physical hooks are ready.
 */
typedef struct dp104_master_patch_state {
    uint32_t initialized;
    dp104_master_bridge bridge;
    dp104_usb_stage usb;
    dp104_wire_stream uart_stream;
    uint8_t uart_stream_active;
} dp104_master_patch_state;

typedef struct dp104_plc_patch_state {
    uint32_t initialized;
    dp104_receiver receiver;
    uint32_t twiddle[DP104_TWIDDLE_WORDS];
    uint8_t legacy_slot[68];
    uint8_t color_order[3];
    uint8_t color_order_verified;
    volatile uint8_t override_active;
    uint8_t diagnostic_active;
    uint16_t reserved;
    uint32_t diagnostic_started;
    dp104_wire_stream uart_stream;
} dp104_plc_patch_state;

#endif
