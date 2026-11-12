#ifndef DP104_DMA_RX_H
#define DP104_DMA_RX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum dp104_dma_rx_event {
    DP104_DMA_RX_EVENT_HALF = 1u << 0,
    DP104_DMA_RX_EVENT_COMPLETE = 1u << 1
};

typedef enum dp104_dma_rx_result {
    DP104_DMA_RX_ERROR = -1,
    DP104_DMA_RX_OK = 0,
    DP104_DMA_RX_OVERRUN = 1
} dp104_dma_rx_result;

typedef struct dp104_dma_rx_cursor {
    uint16_t consumed;
    /* 0=normal, 1=remaining==0 consumed before TC, 2=TC boundary consumed. */
    uint8_t pending_complete;
} dp104_dma_rx_cursor;

typedef void (*dp104_dma_rx_emit_fn)(void *context, const uint8_t *data,
                                     size_t length);

void dp104_dma_rx_cursor_init(dp104_dma_rx_cursor *cursor);

/*
 * Consume bytes from a circular DMA buffer using one coherent snapshot.
 * Capacity must be in [1, UINT16_MAX], remaining must be <= capacity, and
 * events may contain only HALF and COMPLETE. A zero event mask is a
 * position-only observation (for example, an IDLE callback).
 *
 * Emits only newly observed spans. A wrap emits the tail and head separately.
 * An exact remaining==0 tail is normalized to cursor position 0. If its TC
 * flag is delayed, new head bytes can still be consumed once before that flag
 * arrives; the flag then acknowledges the already-consumed boundary. A TC
 * boundary already consumed also tolerates repeated position-only IDLE at
 * position 0 until remaining advances. These boundary markers cannot reveal
 * an unobserved full lap; callers must provide coherent flags and service
 * often enough that multiple wraps cannot occur between snapshots.
 *
 * HALF and COMPLETE together, or a COMPLETE snapshot whose normalized
 * position equals consumed without a pending boundary acknowledgement, is an
 * ambiguous lap: cursor advances to the current position, nothing is emitted,
 * and OVERRUN is returned. Invalid arguments/counters return ERROR without
 * advancing the cursor. The caller owns interrupt serialization, flag clears,
 * and DMA snapshot coherence.
 */
dp104_dma_rx_result dp104_dma_rx_consume(
    dp104_dma_rx_cursor *cursor, const uint8_t *buffer,
    size_t capacity, size_t remaining, uint8_t events,
    dp104_dma_rx_emit_fn emit, void *context);

#ifdef __cplusplus
}
#endif

#endif
