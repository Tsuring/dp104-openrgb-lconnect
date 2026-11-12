#include "dp104_dma_rx.h"

#include <limits.h>

#define DP104_DMA_RX_EVENT_MASK \
    (DP104_DMA_RX_EVENT_HALF | DP104_DMA_RX_EVENT_COMPLETE)

enum dp104_dma_rx_boundary_state {
    DP104_DMA_RX_BOUNDARY_NORMAL = 0,
    DP104_DMA_RX_BOUNDARY_AWAIT_COMPLETE = 1,
    DP104_DMA_RX_BOUNDARY_COMPLETE_SEEN = 2
};

void dp104_dma_rx_cursor_init(dp104_dma_rx_cursor *cursor)
{
    if(!cursor) return;
    cursor->consumed = 0;
    cursor->pending_complete = 0;
}

static void emit_span(dp104_dma_rx_emit_fn emit, void *context,
                      const uint8_t *data, size_t length)
{
    if(length) emit(context, data, length);
}

static dp104_dma_rx_result overrun(dp104_dma_rx_cursor *cursor,
                                   uint16_t position)
{
    cursor->consumed = position;
    cursor->pending_complete = 0;
    return DP104_DMA_RX_OVERRUN;
}

#ifdef DP104_PLC_UNITY_CURSOR
__attribute__((always_inline))
#endif
dp104_dma_rx_result dp104_dma_rx_consume(
    dp104_dma_rx_cursor *cursor, const uint8_t *buffer,
    size_t capacity, size_t remaining, uint8_t events,
    dp104_dma_rx_emit_fn emit, void *context)
{
    size_t raw_position;
    uint16_t position;
    uint16_t consumed;

    if(!cursor || !buffer || !emit || capacity == 0u ||
       capacity > UINT16_MAX || remaining > capacity ||
       (events & (uint8_t)~DP104_DMA_RX_EVENT_MASK) != 0u ||
       cursor->consumed >= capacity ||
       cursor->pending_complete > DP104_DMA_RX_BOUNDARY_COMPLETE_SEEN)
        return DP104_DMA_RX_ERROR;

    raw_position = capacity - remaining;
    position = (uint16_t)(raw_position == capacity ? 0u : raw_position);
    consumed = cursor->consumed;

    if(events == DP104_DMA_RX_EVENT_MASK)
        return overrun(cursor, position);

    /* A completed boundary remains a no-data endpoint only until DMA moves. */
    if(cursor->pending_complete == DP104_DMA_RX_BOUNDARY_COMPLETE_SEEN &&
       position != 0u)
        cursor->pending_complete = DP104_DMA_RX_BOUNDARY_NORMAL;

    if(events & DP104_DMA_RX_EVENT_COMPLETE)
    {
        if(cursor->pending_complete == DP104_DMA_RX_BOUNDARY_AWAIT_COMPLETE)
        {
            if(position < consumed)
                return overrun(cursor, position);
            cursor->consumed = position;
            cursor->pending_complete = position == 0u ?
                DP104_DMA_RX_BOUNDARY_COMPLETE_SEEN :
                DP104_DMA_RX_BOUNDARY_NORMAL;
            emit_span(emit, context, buffer + consumed, position - consumed);
            return DP104_DMA_RX_OK;
        }
        if(position == consumed)
            return overrun(cursor, position);
        if(position > consumed)
            return overrun(cursor, position);

        cursor->consumed = position;
        cursor->pending_complete = position == 0u ?
            DP104_DMA_RX_BOUNDARY_COMPLETE_SEEN :
            DP104_DMA_RX_BOUNDARY_NORMAL;
        emit_span(emit, context, buffer + consumed, capacity - consumed);
        emit_span(emit, context, buffer, position);
        return DP104_DMA_RX_OK;
    }

    if(remaining == 0u)
    {
        if(consumed == 0u)
        {
            if(cursor->pending_complete != DP104_DMA_RX_BOUNDARY_NORMAL)
                return DP104_DMA_RX_OK;
            return overrun(cursor, position);
        }
        if(cursor->pending_complete == DP104_DMA_RX_BOUNDARY_AWAIT_COMPLETE)
            return overrun(cursor, position);

        cursor->consumed = 0;
        cursor->pending_complete = DP104_DMA_RX_BOUNDARY_AWAIT_COMPLETE;
        emit_span(emit, context, buffer + consumed, capacity - consumed);
        return DP104_DMA_RX_OK;
    }

    if(position < consumed)
        return overrun(cursor, position);
    if(position == consumed)
        return DP104_DMA_RX_OK;

    cursor->consumed = position;
    if(cursor->pending_complete != DP104_DMA_RX_BOUNDARY_AWAIT_COMPLETE)
        cursor->pending_complete = DP104_DMA_RX_BOUNDARY_NORMAL;
    emit_span(emit, context, buffer + consumed, position - consumed);
    return DP104_DMA_RX_OK;
}
