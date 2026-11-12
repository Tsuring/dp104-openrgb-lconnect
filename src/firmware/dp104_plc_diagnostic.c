#include "dp104_plc_diagnostic.h"

#include <stddef.h>
#include <string.h>

#define DIAG_PREFIX_IDLE 0u
#define DIAG_PREFIX_DIRECTION 1u
#define DIAG_PREFIX_TYPE 2u
#define DIAG_PREFIX_SYMBOLS 3u

#define DIAG_HEADER_FLAG_ARMED 0x0001u
#define DIAG_HEADER_FLAG_FROZEN 0x0002u
#define DIAG_HEADER_FLAG_WRAPPED 0x0004u
#define DIAG_HEADER_FLAG_PREFIX_ACTIVE 0x0008u
#define DIAG_COUNTER_COUNT 11u

#if defined(_MSC_VER)
#define DP104_DIAG_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define DP104_DIAG_NOINLINE __attribute__((noinline))
#else
#define DP104_DIAG_NOINLINE
#endif

typedef char dp104_diag_counter_layout_must_be_contiguous[
    offsetof(dp104_plc_diagnostic, uart_send_calls) ==
        offsetof(dp104_plc_diagnostic, rx_sequence) +
            (DIAG_COUNTER_COUNT - 1u) * sizeof(uint32_t) ? 1 : -1];

static void write_le16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static void write_le32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static uint16_t read_le16(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

static int all_zero(const uint8_t *bytes, unsigned start, unsigned end)
{
    unsigned i;
    for(i = start; i < end; ++i)
        if(bytes[i]) return 0;
    return 1;
}

static int nonce_nonzero(const uint8_t nonce[4])
{
    return nonce[0] || nonce[1] || nonce[2] || nonce[3];
}

static void response_header(uint8_t response[DP104_REPORT_BYTES],
                            const uint8_t request[DP104_REPORT_BYTES],
                            uint8_t status)
{
    memset(response, 0, DP104_REPORT_BYTES);
    response[0] = DP104_COMMAND;
    response[1] = DP104_PLC_DIAGNOSTIC_OP;
    response[2] = request[2];
    response[3] = status;
}

static void append_record(dp104_plc_diagnostic *diagnostic, uint8_t event,
                          uint8_t flags, uint16_t detail, uint8_t command,
                          uint8_t operation, uint8_t transaction,
                          uint8_t value, uint8_t prefix_progress,
                          uint16_t stream_used_before,
                          uint16_t stream_used_after)
{
    uint8_t *record;
    uint16_t index;

    index = (uint16_t)(diagnostic->write_index &
                       (DP104_PLC_DIAGNOSTIC_RECORD_CAPACITY - 1u));
    record = diagnostic->records[index];
    memset(record, 0, DP104_PLC_DIAGNOSTIC_RECORD_BYTES);
    write_le32(record + 0, diagnostic->rx_sequence);
    write_le32(record + 4, diagnostic->last_rx_sample_count);
    write_le32(record + 8, diagnostic->last_rx_sr_before_dr);
    write_le32(record + 12, diagnostic->last_rx_cndtr_after_dr);
    write_le16(record + 16, diagnostic->last_rx_length);
    write_le16(record + 18, stream_used_before);
    write_le16(record + 20, stream_used_after);
    write_le16(record + 22, detail);
    record[24] = event;
    record[25] = flags;
    record[26] = command;
    record[27] = operation;
    record[28] = transaction;
    record[29] = value;
    record[30] = prefix_progress;
    record[31] = diagnostic->prefix_symbol_count;
    diagnostic->write_index = (uint16_t)((index + 1u) &
                               (DP104_PLC_DIAGNOSTIC_RECORD_CAPACITY - 1u));
    if(diagnostic->record_count < DP104_PLC_DIAGNOSTIC_RECORD_CAPACITY)
        ++diagnostic->record_count;
    else
        diagnostic->ring_wrapped = 1;
}

static DP104_DIAG_NOINLINE int active_application_request(
    const dp104_plc_diagnostic *diagnostic,
    const uint8_t request[DP104_REPORT_BYTES])
{
    return diagnostic && request && diagnostic->armed && !diagnostic->frozen &&
           request[0] == DP104_COMMAND && request[1] <= DP104_OP_COMMIT;
}

static DP104_DIAG_NOINLINE void append_request_record(
    dp104_plc_diagnostic *diagnostic,
    const uint8_t request[DP104_REPORT_BYTES], uint8_t event, uint8_t flags,
    uint16_t detail, uint8_t value, uint16_t stream_used_before,
    uint16_t stream_used_after)
{
    append_record(diagnostic, event, flags, detail, request[0], request[1],
                  request[2], value, 7, stream_used_before,
                  stream_used_after);
}

static void reset_prefix(dp104_plc_diagnostic *diagnostic)
{
    diagnostic->prefix_state = DIAG_PREFIX_IDLE;
    diagnostic->prefix_symbol_count = 0;
    diagnostic->prefix_bit_count = 0;
    diagnostic->prefix_decoded_count = 0;
    diagnostic->prefix_candidate_counted = 0;
    diagnostic->prefix_accumulator = 0;
    memset(diagnostic->prefix_decoded, 0, sizeof(diagnostic->prefix_decoded));
}

static void append_prefix_record(dp104_plc_diagnostic *diagnostic,
                                 uint16_t stream_used_before,
                                 uint16_t stream_used_after)
{
    const uint8_t decoded = diagnostic->prefix_decoded_count;
    uint8_t command = 0xffu;
    uint8_t operation = 0xffu;
    uint8_t transaction = 0xffu;
    uint8_t value = decoded;
    uint8_t flags = DP104_PLC_DIAG_RECORD_AMBIGUOUS |
                    DP104_PLC_DIAG_RECORD_PREFIX_HEADER_COMPLETE;
    uint16_t detail = UINT16_MAX;

    if(decoded >= 1u)
    {
        command = diagnostic->prefix_decoded[0];
        flags |= DP104_PLC_DIAG_RECORD_D2_COMMAND;
    }
    if(decoded >= 2u) operation = diagnostic->prefix_decoded[1];
    if(decoded >= 3u) transaction = diagnostic->prefix_decoded[2];
    if(decoded >= 7u && operation == DP104_OP_DATA)
    {
        detail = (uint16_t)((uint16_t)diagnostic->prefix_decoded[4] |
                            ((uint16_t)diagnostic->prefix_decoded[5] << 8));
        value = diagnostic->prefix_decoded[6];
        flags |= DP104_PLC_DIAG_RECORD_D2_FIELDS_COMPLETE |
                 DP104_PLC_DIAG_RECORD_DATA_FIELDS;
    }
    else if(decoded >= 7u)
        flags |= DP104_PLC_DIAG_RECORD_D2_FIELDS_COMPLETE;

    /* F5 is the inspection protocol itself. Do not let its requests occupy
     * the ring or replace the application-frame observations. */
    if(command == DP104_COMMAND && operation == DP104_PLC_DIAGNOSTIC_OP)
        return;

    if(command == DP104_COMMAND && operation != 0xffu &&
       !diagnostic->prefix_candidate_counted)
    {
        ++diagnostic->d2_candidates;
        diagnostic->prefix_candidate_counted = 1;
    }
    append_record(diagnostic, command == DP104_COMMAND ?
                  DP104_PLC_DIAG_RX_CANDIDATE : DP104_PLC_DIAG_RX_PREFIX,
                  flags, detail, command, operation, transaction, value,
                  decoded, stream_used_before, stream_used_after);
}

static void finish_candidate_if_ready(dp104_plc_diagnostic *diagnostic,
                                      uint16_t stream_used_before,
                                      uint16_t stream_used_after)
{
    if(diagnostic->prefix_decoded_count >= 7u &&
       diagnostic->prefix_symbol_count >= DP104_WIRE_ENCODED_BYTES)
        append_prefix_record(diagnostic, stream_used_before,
                             stream_used_after);
    if(diagnostic->prefix_symbol_count >= DP104_WIRE_ENCODED_BYTES)
        reset_prefix(diagnostic);
}

static DP104_DIAG_NOINLINE int note_prefix_byte(dp104_plc_diagnostic *diagnostic, uint8_t byte,
                            uint16_t stream_used_before,
                            uint16_t stream_used_after)
{
    uint8_t state = diagnostic->prefix_state;
    if(state == DIAG_PREFIX_IDLE)
    {
        if(byte == DP104_WIRE_REQUEST)
        {
            reset_prefix(diagnostic);
            diagnostic->prefix_state = DIAG_PREFIX_DIRECTION;
        }
        return 0;
    }
    if(state == DIAG_PREFIX_DIRECTION)
    {
        if(byte == DP104_WIRE_TYPE)
            diagnostic->prefix_state = DIAG_PREFIX_TYPE;
        else if(byte != DP104_WIRE_REQUEST)
            reset_prefix(diagnostic);
        return 1;
    }
    if(state == DIAG_PREFIX_TYPE)
    {
        if(byte == DP104_WIRE_ENCODED_BYTES)
            diagnostic->prefix_state = DIAG_PREFIX_SYMBOLS;
        else if(byte == DP104_WIRE_REQUEST)
        {
            reset_prefix(diagnostic);
            diagnostic->prefix_state = DIAG_PREFIX_DIRECTION;
        }
        else
            reset_prefix(diagnostic);
        return 1;
    }

    if(byte < DP104_WIRE_SYMBOL_MIN || byte > DP104_WIRE_SYMBOL_MAX)
    {
        reset_prefix(diagnostic);
        if(byte == DP104_WIRE_REQUEST)
            diagnostic->prefix_state = DIAG_PREFIX_DIRECTION;
        return 1;
    }
    if(diagnostic->prefix_decoded_count >=
       (uint8_t)sizeof(diagnostic->prefix_decoded))
    {
        ++diagnostic->prefix_symbol_count;
        if(diagnostic->prefix_symbol_count >= DP104_WIRE_ENCODED_BYTES)
            finish_candidate_if_ready(diagnostic, stream_used_before,
                                      stream_used_after);
        return 1;
    }
    diagnostic->prefix_accumulator =
        (diagnostic->prefix_accumulator << 6) |
        (uint32_t)(byte - DP104_WIRE_SYMBOL_MIN);
    diagnostic->prefix_bit_count =
        (uint8_t)(diagnostic->prefix_bit_count + 6u);
    ++diagnostic->prefix_symbol_count;
    while(diagnostic->prefix_bit_count >= 8u &&
          diagnostic->prefix_decoded_count <
              (uint8_t)sizeof(diagnostic->prefix_decoded))
    {
        uint8_t decoded;
        diagnostic->prefix_bit_count =
            (uint8_t)(diagnostic->prefix_bit_count - 8u);
        decoded = (uint8_t)(diagnostic->prefix_accumulator >>
                            diagnostic->prefix_bit_count);
        diagnostic->prefix_decoded[diagnostic->prefix_decoded_count++] = decoded;
        diagnostic->prefix_accumulator = diagnostic->prefix_bit_count ?
            diagnostic->prefix_accumulator &
                ((UINT32_C(1) << diagnostic->prefix_bit_count) - 1u) : 0;
        if(diagnostic->prefix_decoded_count == 1u && decoded != DP104_COMMAND)
        {
            reset_prefix(diagnostic);
            return 1;
        }
    }
    return 1;
}

void dp104_plc_diagnostic_on_rx(dp104_plc_diagnostic *diagnostic,
                                const uint8_t *data, uint32_t length,
                                uint32_t sr_before_dr,
                                uint32_t cndtr_after_dr,
                                uint32_t sample_count,
                                uint16_t stream_used_before,
                                uint16_t stream_used_after,
                                int push_failed)
{
    uint32_t i;
    int prefix_observed = 0;

    if(!diagnostic || !diagnostic->armed || diagnostic->frozen) return;
    ++diagnostic->rx_sequence;
    diagnostic->rx_bytes += length;
    diagnostic->last_rx_sr_before_dr = sr_before_dr;
    diagnostic->last_rx_cndtr_after_dr = cndtr_after_dr;
    diagnostic->last_rx_sample_count = sample_count;
    diagnostic->last_rx_length = length > UINT16_MAX ? UINT16_MAX :
                                 (uint16_t)length;
    diagnostic->last_stream_used_before = stream_used_before;
    diagnostic->last_stream_used_after = stream_used_after;
    diagnostic->last_rx_push_failed = push_failed != 0;

    /* Count actual sampled DMA exhaustion, independent of the configured
     * receive capacity. A full buffer alone is not proof of lost bytes. */
    if(length != 0u && cndtr_after_dr == 0u)
    {
        ++diagnostic->rx_dma_full_callbacks;
        append_record(diagnostic, DP104_PLC_DIAG_RX_DMA_FULL,
                      push_failed ? DP104_PLC_DIAG_RECORD_PUSH_FAILED : 0,
                      (uint16_t)length, 0xffu, 0xffu, 0xffu, 0,
                      diagnostic->prefix_decoded_count,
                      stream_used_before, stream_used_after);
    }
    if(push_failed)
    {
        ++diagnostic->rx_push_failures;
        append_record(diagnostic, DP104_PLC_DIAG_RX_PUSH_FAILURE,
                      DP104_PLC_DIAG_RECORD_PUSH_FAILED,
                      length > UINT16_MAX ? UINT16_MAX : (uint16_t)length,
                      0xffu, 0xffu, 0xffu, 0,
                      diagnostic->prefix_decoded_count,
                      stream_used_before, stream_used_after);
        reset_prefix(diagnostic);
        return;
    }
    if(length && !data)
        return;

    for(i = 0; i < length; ++i)
        prefix_observed |= note_prefix_byte(diagnostic, data[i],
                                            stream_used_before,
                                            stream_used_after);

    if(prefix_observed && diagnostic->prefix_state != DIAG_PREFIX_IDLE)
    {
        const uint8_t decoded = diagnostic->prefix_decoded_count;
        const uint8_t operation = decoded >= 2u ?
                                  diagnostic->prefix_decoded[1] : 0xffu;
        if(decoded >= 1u && diagnostic->prefix_decoded[0] == DP104_COMMAND &&
           operation != DP104_PLC_DIAGNOSTIC_OP)
            append_prefix_record(diagnostic, stream_used_before,
                                 stream_used_after);
        else if(decoded == 0u || operation != DP104_PLC_DIAGNOSTIC_OP)
            append_record(diagnostic, DP104_PLC_DIAG_RX_PREFIX,
                          DP104_PLC_DIAG_RECORD_AMBIGUOUS,
                          diagnostic->prefix_state,
                          decoded ? diagnostic->prefix_decoded[0] : 0xffu,
                          operation, decoded >= 3u ?
                          diagnostic->prefix_decoded[2] : 0xffu,
                          decoded, decoded, stream_used_before,
                          stream_used_after);
    }
}

void dp104_plc_diagnostic_note_parser(dp104_plc_diagnostic *diagnostic,
                                      uint8_t event, uint16_t bytes,
                                      uint16_t stream_used_before,
                                      uint16_t stream_used_after,
                                      uint8_t dropping_before,
                                      uint8_t dropping_after)
{
    uint8_t flags = 0;
    if(!diagnostic || !diagnostic->armed || diagnostic->frozen) return;
    if(dropping_before) flags |= DP104_PLC_DIAG_RECORD_DROPPING_BEFORE;
    if(dropping_after) flags |= DP104_PLC_DIAG_RECORD_DROPPING_AFTER;
    if(event == DP104_PLC_DIAG_PARSER_DISCARD ||
       event == DP104_PLC_DIAG_PARSER_BATCH_DISCARD)
        diagnostic->parser_discard_bytes += bytes;
    append_record(diagnostic, event, flags, bytes, 0xffu, 0xffu, 0xffu,
                  0, 0, stream_used_before, stream_used_after);
}

void dp104_plc_diagnostic_note_extension(
    dp104_plc_diagnostic *diagnostic, const uint8_t request[DP104_REPORT_BYTES],
    uint16_t wire_bytes, uint16_t stream_used_before,
    uint16_t stream_used_after, uint8_t dropping_before,
    uint8_t dropping_after)
{
    uint8_t flags = DP104_PLC_DIAG_RECORD_D2_FIELDS_COMPLETE;
    if(!active_application_request(diagnostic, request))
        return;
    if(dropping_before) flags |= DP104_PLC_DIAG_RECORD_DROPPING_BEFORE;
    if(dropping_after) flags |= DP104_PLC_DIAG_RECORD_DROPPING_AFTER;
    append_request_record(diagnostic, request,
        DP104_PLC_DIAG_EXTENSION_CONSUMED, flags, wire_bytes, request[3],
        stream_used_before, stream_used_after);
}

void dp104_plc_diagnostic_note_decoded(
    dp104_plc_diagnostic *diagnostic, const uint8_t request[DP104_REPORT_BYTES])
{
    uint16_t detail = UINT16_MAX;
    uint8_t value;
    uint8_t flags = DP104_PLC_DIAG_RECORD_D2_FIELDS_COMPLETE;
    if(!active_application_request(diagnostic, request))
        return;
    ++diagnostic->decoded_rgb_frames;
    value = request[3];
    if(request[1] == DP104_OP_DATA)
    {
        detail = dp104_read16(request + 4);
        value = request[6];
        flags |= DP104_PLC_DIAG_RECORD_DATA_FIELDS;
    }
    diagnostic->last_decoded_word = UINT32_C(0x01000000) |
                                    (uint32_t)request[1] |
                                    ((uint32_t)request[2] << 8) |
                                    ((uint32_t)request[3] << 16);
    append_request_record(diagnostic, request, DP104_PLC_DIAG_DECODED_FRAME,
        flags, detail, value, diagnostic->last_stream_used_before,
        diagnostic->last_stream_used_after);
}

void dp104_plc_diagnostic_note_commit(
    dp104_plc_diagnostic *diagnostic, const uint8_t request[DP104_REPORT_BYTES],
    uint8_t status, uint16_t received_leds)
{
    if(!active_application_request(diagnostic, request) ||
       request[1] != DP104_OP_COMMIT)
        return;
    diagnostic->last_commit_word = UINT32_C(0x01000000) |
                                   (uint32_t)request[2] |
                                   ((uint32_t)status << 8) |
                                   ((uint32_t)received_leds << 16);
    append_request_record(diagnostic, request, DP104_PLC_DIAG_COMMIT_RESULT,
        DP104_PLC_DIAG_RECORD_D2_FIELDS_COMPLETE, received_leds, status,
        diagnostic->last_stream_used_before,
        diagnostic->last_stream_used_after);
}

void dp104_plc_diagnostic_note_reply(
    dp104_plc_diagnostic *diagnostic, const uint8_t request[DP104_REPORT_BYTES],
    uint8_t status, int encoded, uint16_t wire_bytes)
{
    if(!active_application_request(diagnostic, request))
        return;
    ++diagnostic->reply_encode_attempts;
    append_request_record(diagnostic, request, DP104_PLC_DIAG_REPLY_ENCODE,
        encoded ? 0 : DP104_PLC_DIAG_RECORD_AMBIGUOUS, wire_bytes, status,
        diagnostic->last_stream_used_before,
        diagnostic->last_stream_used_after);
}

void dp104_plc_diagnostic_note_uart_send(
    dp104_plc_diagnostic *diagnostic, const uint8_t request[DP104_REPORT_BYTES],
    uint8_t status, uint32_t length, uint32_t mode)
{
    if(!active_application_request(diagnostic, request))
        return;
    ++diagnostic->uart_send_calls;
    append_request_record(diagnostic, request,
        DP104_PLC_DIAG_UART_SEND_CALL, 0,
        length > UINT16_MAX ? UINT16_MAX : (uint16_t)length,
        (uint8_t)mode, diagnostic->last_stream_used_before,
        diagnostic->last_stream_used_after);
    (void)status;
}

static void arm_logger(dp104_plc_diagnostic *diagnostic)
{
    diagnostic->armed = 1;
    diagnostic->frozen = 0;
    diagnostic->write_index = 0;
    diagnostic->record_count = 0;
    diagnostic->ring_wrapped = 0;
    diagnostic->reserved0 &= 1u;
    memset((uint8_t *)diagnostic + offsetof(dp104_plc_diagnostic, rx_sequence),
           0, offsetof(dp104_plc_diagnostic, records) -
              offsetof(dp104_plc_diagnostic, rx_sequence));
}

static void freeze_snapshot(dp104_plc_diagnostic *diagnostic,
                            const uint8_t nonce[4])
{
    const uint8_t *counter_bytes = (const uint8_t *)diagnostic +
        offsetof(dp104_plc_diagnostic, rx_sequence);
    uint16_t flags = 0;
    uint16_t oldest = diagnostic->record_count ==
                      DP104_PLC_DIAGNOSTIC_RECORD_CAPACITY ?
                      diagnostic->write_index : 0;
    uint8_t *header = diagnostic->snapshot_header;
    unsigned i;

    diagnostic->frozen = 1;
    memcpy(diagnostic->snapshot_nonce, nonce, 4);
    if(diagnostic->armed) flags |= DIAG_HEADER_FLAG_ARMED;
    flags |= DIAG_HEADER_FLAG_FROZEN;
    if(diagnostic->ring_wrapped)
        flags |= DIAG_HEADER_FLAG_WRAPPED;
    if(diagnostic->prefix_state != DIAG_PREFIX_IDLE)
        flags |= DIAG_HEADER_FLAG_PREFIX_ACTIVE;
    flags |= (uint16_t)((diagnostic->reserved0 & 3u) << 4);

    memset(header, 0, DP104_PLC_DIAGNOSTIC_HEADER_BYTES);
    memcpy(header, "P5LG", 4);
    write_le16(header + 4, 1u); /* Schema version. */
    write_le16(header + 6, DP104_PLC_DIAGNOSTIC_HEADER_BYTES);
    write_le16(header + 8, DP104_PLC_DIAGNOSTIC_TOTAL_BYTES);
    write_le16(header + 10, DP104_PLC_DIAGNOSTIC_RECORD_BYTES);
    write_le16(header + 12, DP104_PLC_DIAGNOSTIC_RECORD_CAPACITY);
    write_le16(header + 14, diagnostic->record_count);
    write_le16(header + 16, flags);
    write_le16(header + 18, oldest);
    for(i = 0; i < DIAG_COUNTER_COUNT; ++i)
    {
        uint32_t value;
        memcpy(&value, counter_bytes + i * sizeof(value), sizeof(value));
        write_le32(header + 20u + i * sizeof(value), value);
    }
}

static void copy_blob_page(const dp104_plc_diagnostic *diagnostic,
                           uint16_t offset, uint8_t page[16])
{
    unsigned i;
    const uint16_t oldest = read_le16(diagnostic->snapshot_header + 18);
    for(i = 0; i < DP104_PLC_DIAGNOSTIC_PAGE_BYTES; ++i)
    {
        const uint16_t blob_offset = (uint16_t)(offset + i);
        if(blob_offset < DP104_PLC_DIAGNOSTIC_HEADER_BYTES)
            page[i] = diagnostic->snapshot_header[blob_offset];
        else
        {
            const uint16_t record_offset =
                (uint16_t)(blob_offset - DP104_PLC_DIAGNOSTIC_HEADER_BYTES);
            const uint16_t logical_index =
                (uint16_t)(record_offset / DP104_PLC_DIAGNOSTIC_RECORD_BYTES);
            const uint16_t byte_index =
                (uint16_t)(record_offset % DP104_PLC_DIAGNOSTIC_RECORD_BYTES);
            if(logical_index >= diagnostic->record_count)
                page[i] = 0;
            else
            {
                const uint16_t physical = (uint16_t)((oldest + logical_index) &
                    (DP104_PLC_DIAGNOSTIC_RECORD_CAPACITY - 1u));
                page[i] = diagnostic->records[physical][byte_index];
            }
        }
    }
}

void dp104_plc_diagnostic_handle(
    dp104_plc_diagnostic *diagnostic,
    const uint8_t request[DP104_REPORT_BYTES],
    uint8_t response[DP104_REPORT_BYTES])
{
    static const uint8_t magic[4] = {'P', '5', 'R', 'X'};
    uint16_t offset;

    if(!response) return;
    if(!request)
    {
        memset(response, 0, DP104_REPORT_BYTES);
        response[0] = DP104_COMMAND;
        response[1] = DP104_PLC_DIAGNOSTIC_OP;
        response[3] = DP104_BAD_REQUEST;
        return;
    }
    response_header(response, request, DP104_BAD_REQUEST);
    if(!diagnostic ||
       request[0] != DP104_COMMAND ||
       request[1] != DP104_PLC_DIAGNOSTIC_OP || request[2] == 0 ||
       request[3] != 0 || !nonce_nonzero(request + 4) ||
       !all_zero(request, 10, DP104_REPORT_BYTES))
        return;

    offset = read_le16(request + 8);
    if(offset == DP104_PLC_DIAGNOSTIC_PREFLIGHT)
    {
        response_header(response, request, DP104_OK);
        memcpy(response + 4, request + 4, 4);
        memcpy(response + 8, magic, sizeof(magic));
        write_le16(response + 12, offset);
        write_le16(response + 14, DP104_PLC_DIAGNOSTIC_TOTAL_BYTES);
        write_le32(response + 16, 1u);
        write_le32(response + 20, DP104_PLC_DIAGNOSTIC_RECORD_BYTES);
        write_le32(response + 24, DP104_PLC_DIAGNOSTIC_RECORD_CAPACITY);
        write_le32(response + 28, DP104_PLC_DIAGNOSTIC_TOTAL_BYTES);
        return;
    }
    if(offset == DP104_PLC_DIAGNOSTIC_ARM)
    {
        arm_logger(diagnostic);
        response_header(response, request, DP104_OK);
        memcpy(response + 4, request + 4, 4);
        memcpy(response + 8, magic, sizeof(magic));
        write_le16(response + 12, offset);
        write_le16(response + 14, DP104_PLC_DIAGNOSTIC_TOTAL_BYTES);
        return;
    }

    if(offset >= DP104_PLC_DIAGNOSTIC_TOTAL_BYTES ||
       (offset % DP104_PLC_DIAGNOSTIC_PAGE_BYTES) != 0)
        return;
    if(!diagnostic->frozen)
    {
        if(offset != 0) return;
        freeze_snapshot(diagnostic, request + 4);
    }
    else if(memcmp(diagnostic->snapshot_nonce, request + 4, 4) != 0)
        return;

    response_header(response, request, DP104_OK);
    memcpy(response + 4, request + 4, 4);
    memcpy(response + 8, magic, sizeof(magic));
    write_le16(response + 12, offset);
    write_le16(response + 14, DP104_PLC_DIAGNOSTIC_TOTAL_BYTES);
    copy_blob_page(diagnostic, offset, response + 16);
}
