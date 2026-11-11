#ifndef DP104_PLC_DIAGNOSTIC_H
#define DP104_PLC_DIAGNOSTIC_H

#include <stdint.h>
#include "dp104_protocol.h"
#include "dp104_wire.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DP104_PLC_DIAGNOSTIC_OP 0xf5u
#define DP104_PLC_DIAGNOSTIC_PREFLIGHT UINT16_C(0xffff)
#define DP104_PLC_DIAGNOSTIC_ARM UINT16_C(0xfffe)
#define DP104_PLC_DIAGNOSTIC_PAGE_BYTES 16u
#define DP104_PLC_DIAGNOSTIC_HEADER_BYTES 64u
#define DP104_PLC_DIAGNOSTIC_RECORD_BYTES 32u
#define DP104_PLC_DIAGNOSTIC_RECORD_CAPACITY 64u
#define DP104_PLC_DIAGNOSTIC_TOTAL_BYTES \
    (DP104_PLC_DIAGNOSTIC_HEADER_BYTES + \
     DP104_PLC_DIAGNOSTIC_RECORD_BYTES * DP104_PLC_DIAGNOSTIC_RECORD_CAPACITY)

/* F5 request: D2,F5,txn!=0,status=0,nonce[4],LE16 offset,zero tail.
 * Response: D2,F5,txn,status,nonce[4],"P5RX",LE16 offset,total,DATA[16].
 * FFFF is non-freezing preflight; FFFE resets/arms metadata only. Offset 0
 * freezes and pins the nonce; later aligned 16-byte pages require that nonce.
 * The fixed blob is 2112 bytes: a 64-byte header and 64 32-byte records.
 * Preflight DATA is four LE u32 values: schema, record size, capacity, max blob.
 *
 * Frozen header offsets (all multibyte fields little endian):
 *   0 magic "P5LG"; 4 schema u16; 6 header bytes u16; 8 total bytes u16;
 *   10 record bytes u16; 12 capacity u16; 14 valid record count u16;
 *   16 flags u16 (armed=1, frozen=2, wrapped=4, prefix-active=8);
 *   18 oldest physical ring index u16;
 *   20 RX callbacks u32; 24 RX bytes u32; 28 length-72 callbacks u32;
 *   32 push failures u32; 36 D2 prefix candidates u32;
 *   40 parser-discard bytes u32; 44 decoded RGB request count u32;
 *   48 last decoded word u32 (valid bit24, request status bits16..23,
 *      transaction bits8..15, operation bits0..7);
 *   52 last COMMIT word u32 (valid bit24, received LED count bits16..23,
 *      response status bits8..15, transaction bits0..7);
 *   56 reply-encode attempts u32; 60 stock UART-send calls u32.
 *
 * Record offsets (oldest-to-newest logical order; unused fixed-capacity
 * records are zero): 0 RX sequence u32; 4 sampled RX count u32; 8 raw SR
 * before DR u32; 12 raw CNDTR after DR u32; 16 callback length u16; 18 stream
 * used before push/parser u16; 20 stream used after u16; 22 event detail u16;
 * 24 event code; 25 flags; 26 decoded command; 27 operation; 28 transaction;
 * 29 event value; 30 decoded-prefix byte count; 31 prefix symbols observed
 * at this event (0..43). Unused fixed-capacity records are zero. DATA
 * candidate/decoded records place first LED at detail and chunk count at value.
 * Prefix/candidate events are observations only; they do not prove a complete
 * or valid frame. UART-send events mean the stock async helper was called,
 * not that bytes reached the wire or peer.
 */

enum dp104_plc_diagnostic_event {
    DP104_PLC_DIAG_RX_PREFIX = 1,
    DP104_PLC_DIAG_RX_CANDIDATE = 2,
    DP104_PLC_DIAG_RX_DMA_FULL = 3,
    DP104_PLC_DIAG_RX_PUSH_FAILURE = 4,
    DP104_PLC_DIAG_PARSER_DISCARD = 5,
    DP104_PLC_DIAG_PARSER_BATCH_DISCARD = 6,
    DP104_PLC_DIAG_EXTENSION_CONSUMED = 7,
    DP104_PLC_DIAG_DECODED_FRAME = 8,
    DP104_PLC_DIAG_COMMIT_RESULT = 9,
    DP104_PLC_DIAG_REPLY_ENCODE = 10,
    DP104_PLC_DIAG_UART_SEND_CALL = 11
};

enum dp104_plc_diagnostic_record_flags {
    DP104_PLC_DIAG_RECORD_AMBIGUOUS = 1u << 0,
    DP104_PLC_DIAG_RECORD_PUSH_FAILED = 1u << 1,
    DP104_PLC_DIAG_RECORD_DROPPING_BEFORE = 1u << 2,
    DP104_PLC_DIAG_RECORD_DROPPING_AFTER = 1u << 3,
    DP104_PLC_DIAG_RECORD_PREFIX_HEADER_COMPLETE = 1u << 4,
    DP104_PLC_DIAG_RECORD_D2_COMMAND = 1u << 5,
    DP104_PLC_DIAG_RECORD_D2_FIELDS_COMPLETE = 1u << 6,
    DP104_PLC_DIAG_RECORD_DATA_FIELDS = 1u << 7
};

/* Metadata-only, bounded logger state. Records contain no raw UART payload or
 * LED/key bytes. All mutating note/handle functions require the caller to
 * exclude interrupts; the RX callback and queue hook already do so. */
typedef struct dp104_plc_diagnostic {
    uint8_t armed;
    uint8_t frozen;
    uint16_t write_index;
    uint16_t record_count;
    uint8_t ring_wrapped;
    uint8_t reserved0;

    uint32_t rx_sequence;
    uint32_t rx_bytes;
    uint32_t rx_dma_full_callbacks;
    uint32_t rx_push_failures;
    uint32_t d2_candidates;
    uint32_t parser_discard_bytes;
    uint32_t decoded_rgb_frames;
    uint32_t last_decoded_word;
    uint32_t last_commit_word;
    uint32_t reply_encode_attempts;
    uint32_t uart_send_calls;

    uint32_t last_rx_sr_before_dr;
    uint32_t last_rx_cndtr_after_dr;
    uint32_t last_rx_sample_count;
    uint16_t last_rx_length;
    uint16_t last_stream_used_before;
    uint16_t last_stream_used_after;
    uint8_t last_rx_push_failed;
    uint8_t prefix_state;
    uint8_t prefix_symbol_count;
    uint8_t prefix_bit_count;
    uint8_t prefix_decoded_count;
    uint8_t prefix_candidate_counted;
    uint8_t prefix_decoded[7];
    uint32_t prefix_accumulator;

    uint8_t snapshot_nonce[4];
    uint8_t snapshot_header[DP104_PLC_DIAGNOSTIC_HEADER_BYTES];
    uint8_t records[DP104_PLC_DIAGNOSTIC_RECORD_CAPACITY]
                   [DP104_PLC_DIAGNOSTIC_RECORD_BYTES];
} dp104_plc_diagnostic;

/* Called by dp104_plc_uart_rx after the unchanged wire-stream push attempt. */
void dp104_plc_diagnostic_on_rx(dp104_plc_diagnostic *diagnostic,
                                const uint8_t *data, uint32_t length,
                                uint32_t sr_before_dr,
                                uint32_t cndtr_after_dr,
                                uint32_t sample_count,
                                uint16_t stream_used_before,
                                uint16_t stream_used_after,
                                int push_failed);

/* These event functions are called under the target's existing IRQ guard. */
void dp104_plc_diagnostic_note_parser(dp104_plc_diagnostic *diagnostic,
                                      uint8_t event, uint16_t bytes,
                                      uint16_t stream_used_before,
                                      uint16_t stream_used_after,
                                      uint8_t dropping_before,
                                      uint8_t dropping_after);
void dp104_plc_diagnostic_note_extension(
    dp104_plc_diagnostic *diagnostic, const uint8_t request[DP104_REPORT_BYTES],
    uint16_t wire_bytes, uint16_t stream_used_before,
    uint16_t stream_used_after, uint8_t dropping_before,
    uint8_t dropping_after);
void dp104_plc_diagnostic_note_decoded(
    dp104_plc_diagnostic *diagnostic, const uint8_t request[DP104_REPORT_BYTES]);
void dp104_plc_diagnostic_note_commit(
    dp104_plc_diagnostic *diagnostic, const uint8_t request[DP104_REPORT_BYTES],
    uint8_t status, uint16_t received_leds);
void dp104_plc_diagnostic_note_reply(
    dp104_plc_diagnostic *diagnostic, const uint8_t request[DP104_REPORT_BYTES],
    uint8_t status, int encoded, uint16_t wire_bytes);
void dp104_plc_diagnostic_note_uart_send(
    dp104_plc_diagnostic *diagnostic, const uint8_t request[DP104_REPORT_BYTES],
    uint8_t status, uint32_t length, uint32_t mode);

/* Valid F5 requests are answered locally. Caller holds the IRQ guard. */
void dp104_plc_diagnostic_handle(
    dp104_plc_diagnostic *diagnostic,
    const uint8_t request[DP104_REPORT_BYTES],
    uint8_t response[DP104_REPORT_BYTES]);

#ifdef __cplusplus
}
#endif
#endif
