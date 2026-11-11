# DP104 direct RGB extension, revision 1

This document describes the implemented project-specific D2 extension. Stock DP104 firmware 1.2.0 does **not** implement it. VIA protocol 12 is separate and is not evidence of D2 support or of the QMK OpenRGB protocol.

Compatibility application **1.2.1** uses the **1.2.0 8K master** plus PLC 1.2.0 as pinned inputs. The fixed-wire 1.2.1 images are installed on the tested unit. Relocation addresses from the normal master must not be reused without independent verification.

The path is L-Connect SDK -> OpenRGB controller -> master Raw HID -> PLC receiver -> existing 104-entry LED output. HID uses the existing FF60:0061 interface, VID E560/PID E104, with 32-byte payloads and report ID zero. A hidapi write therefore contains 33 bytes. The protocol header is `include/dp104_protocol.h`.

## Capability gate

Send command D2 / operation CAPS. Stock firmware returns an unsupported response. The native controller must reject that response and must not register a device. A successful response needs the exact DP104RGB magic, major version 1, 104 LEDs, eight LEDs per chunk, and the end-to-end-ready flag. That flag may only be asserted after the master has established that the PLC receiver, renderer arbitration, and output hooks are ready. A locally fabricated master acknowledgement is insufficient.

## Frames

BEGIN declares a transaction, exactly 104 LEDs, and the CRC-32/ISO-HDLC of 312 bytes in RGB order. DATA carries a starting index and one to eight triples. Chunks may arrive out of order or repeat. COMMIT succeeds only when all LEDs have been received and the CRC matches. Partial or rejected frames must not alter visible colors. COMMIT acknowledges the actual PLC acceptance and publication of the whole frame; it must not mean only that bytes were sent to a UART.

The PLC must exclude its built-in effect producer while direct mode owns output, and must retain key scanning, display operation and the existing LED serializer. RELEASE returns ownership to the existing effect producer. A 1000 ms lease expires after the last successful COMMIT; malformed requests, queries and partial frames do not renew it. Partial transactions also expire after 1000 ms from BEGIN. Time comparisons must work across the 32-bit timer rollover.

## Transport and mapping boundaries

The stock master sends an 11-byte type-0B global effect packet. The D2 extension reserves type `0x7D` in directions 0 (PLC reply) and 1 (master request). The internal record is `[direction, 0x7D, 43]` followed by 43 six-bit symbols, for a fixed total of **46 bytes**. Each symbol is its value plus `0x21`, so the alphabet is `0x21..0x60`; 32 report bytes are packed most-significant-bit first, and the final symbol's two unused low bits must be zero. No symbol can equal a direction prefix `0`, `1`, or `2`, or the old escape marker `3`. HID remains exactly 32 bytes.

Both master and PLC must use the fixed-wire image pair. The earlier escaped format is incompatible: a reserved `0x7D` record with a different length is consumed as one malformed reserved record and is never passed to the stock dispatcher. Legacy records remain opaque and are limited to a 64-byte payload (67 bytes including the header), within the current 128-byte PLC RX DMA buffer (the stock buffer was 72 bytes). `dp104_wire_stream` retains fragments across receive callbacks, extracts coalesced records and enters bounded resynchronization on malformed input or FIFO overflow. Tests cover all byte values, all split points, coalescing, legacy payload preservation, old-format rejection, padding-bit validation and overflow; they do not establish physical serial timing.

The earlier escaped format produced zero-heavy variable-length records and had a confirmed missing-DATA failure: an immediate F3 snapshot showed that DATA indices 40..103 had not reached the PLC receiver. The exact loss point within the UART path was not identified. The fixed 46-byte pair passed repeated zero-heavy regressions under slow, bare-marker and fast varied-frame conditions. This removes the variable-length escape path, but does not by itself prove the original loss mechanism.

The PLC retains a conditional reserved empty-record `[0, 0x7D, 0]` path for replies whose total length is divisible by seven. A current 46-byte reply is not divisible by seven, so this branch is not used by fixed-wire extension replies. The old 49-byte F1 and 63-byte STATUS boundaries are historical escaped-format cases, not current frame sizes.

Extension replies use the original PLC UART sender's asynchronous mode 1, matching all three observed stock callers. The sender copies the reply into its own 72-byte DMA buffer before returning; the next call waits for completion and disables the channel before reusing it. The mode-0 path can leave the channel enabled while the software busy flag is clear; mixing that path with a following stock mode-1 send can leave the worker waiting for a transfer that did not start. The implemented sender uses mode 1. Fixed-wire regressions include a 60-second fast varied-frame run (1,223 frames, 19,624 exact exchanges, no retry or foreign report, 16 ms maximum observed reply) and one-second lease expiry without RELEASE. Separate zero-heavy runs also completed. These observations are bounded and do not establish unlimited runtime stability.

The PC transport writes each request once and reads within the existing 100 ms deadline until command, operation and transaction match. Unrelated complete HID reports are discarded; short reads and errors fail closed. A matching endpoint error is returned to the client for rejection, rather than hidden by the filter. The failed request and transaction are never replayed. Native recovery is separate: after a fresh successful CAPS exchange, the worker may send the newest queued frame with a new transaction. Its bounded retry policy is described in [the architecture record](architecture.md).

`dp104_master_bridge` only copies reports in its interrupt-facing callbacks. Its task-side pump transmits and accepts only a matching actual PLC response. USB busy submission retains that response for retry unless a newer host request supersedes it. D1 updater traffic cancels the extension generation; target code must stop extension bytes before handing control back to the stock updater. No successful reply is synthesized for a missing PLC.

## Explicit color calibration

The patched PLC starts without a guessed physical color order and returns BUSY for a valid CAPS request until calibration. Two additional target-only operations use the same D2 header and exact acknowledgement validation:

- `F0 RAW_PULSE`: byte 4 is raw channel 0..2, byte 5 is intensity 0..64, remaining bytes zero. It temporarily lights that raw channel on all 104 outputs for at most 1000 ms, then releases the built-in renderer. It is refused after an order is confirmed.
- `F1 SET_ORDER`: bytes 4..6 are a permutation of raw channels corresponding to RGB, bytes 7..10 are ASCII `USER`, bytes 11..26 contain a fresh nonzero 16-byte nonce, and bytes 27..31 are zero. The success reply echoes the actual order in bytes 4..6 and the nonce in bytes 7..22; bytes 23..31 are zero. The host rejects stale acknowledgements, different orders and different nonces. This asserts an order personally observed by the operator. It changes volatile state only. Repeating the same order is idempotent; a different order is refused until restart.

`tools/calibrate.py` prepares these explicit commands; it is never run by tests or normal discovery. After manual verification it prints a device-path-specific OpenRGB settings fragment, without editing configuration. The native detector can reapply that confirmed permutation from `DP104Settings` and then still requires a full successful CAPS exchange. There is no default mapping, automatic calibration or firmware write in this tool.

The D2 frame carries RGB triples. For the tested keyboard, explicit F1 calibration confirmed raw channel order `[1, 0, 2]` (GRB storage). The original renderer's 104-entry coordinate table was checked against the VIA layout and physical ESC, Space and Enter anchors. In L-Connect compatibility mode, the native adapter selects the linear input sample by horizontal coordinate using `sourceIndex = (x * 103 + 112) / 224`; normal keyboard mode retains identity index order. The user confirmed that the projected Wave follows L-Connect color, brightness, speed and left/right direction options and remains active. VIA labels alone are not treated as proof of physical LED order.

## Application version

`F2 APP_INFO` uses a nonzero transaction, zero request status, a fresh nonzero 16-byte nonce at bytes 4..19, and zeros at bytes 20..31. The reply contains `DP104RGB` at bytes 4..11, semantic major/minor/patch at 12..14, readiness bit 0 at 15, and the echoed nonce at 16..31. Other flags are rejected. This query is available before calibration, changes no renderer ownership and does not renew a frame lease.

### Receiver snapshot diagnostic

The target-only `F3 RECEIVER_SNAPSHOT` is a read-only diagnostic enabled in the patched PLC build. Its request uses a nonzero transaction, zero status, a fresh nonzero four-byte nonce at bytes 4..7, and zeros at bytes 8..31. The reply echoes the nonce at 4..7; byte 8 contains direct-active bit 0 and staging-active bit 1; bytes 9 and 10 identify the staging and active transactions. Bytes 11..23 are the 104-bit received bitmap, 24..27 are the expected frame CRC32, and 28..31 are the CRC32 of the current staging buffer. Integers remain little endian.

The snapshot neither publishes a frame nor changes its lease. Normal task polling can still expire the one-second lease before the query arrives. Query immediately after a failed exchange without resending that exchange: a set bit shows the corresponding DATA was accepted; a complete matching frame with an unchanged active transaction points to COMMIT delivery or rejection. The snapshot does not identify which side of the UART lost an unaccepted request. A successful COMMIT with a lost reply can be distinguished by the active transaction; it must not be assumed from a USB timeout.

### Master forwarding diagnostic

`F4` is a master-local diagnostic, not a PLC acknowledgement. Requests use a nonzero transaction, zero reserved byte, a fresh nonzero four-byte nonce at 4..7 and zeros afterward. Replies echo the header/nonce, contain `DP104M4\0` at 8..15, and four little-endian words at 16..31: last host frame request, last UART FIFO submission, pre-diagnostic bridge flags and pre-diagnostic generation. Request words identify operation, transaction and first DATA LED (or `FF`) in successive bytes. The TX word also stores submitted byte count in bits 24..29, initial FIFO-full observation in bit 30 and complete-envelope submission in bit 31. Only operations 0..3 are tracked. This proves local receipt/submission, not physical UART delivery.

### PLC receive diagnostic

`F5` exports bounded receive metadata without raw legacy/key payloads. Requests carry the standard header, fresh four-byte nonce, a little-endian page offset at 8..9 and zeros afterward. Replies echo header/nonce, contain `P5RX`, echo the offset, declare total size and carry 16 data bytes. Offset `FFFF` describes schema 1, record size 32, ring capacity 64 and total size 2,112; `FFFE` arms/reset metadata without changing RGB state. Reading offset zero freezes the ring and pins its nonce; remaining offsets must be 16-byte aligned and use the same nonce. Malformed, stale and out-of-bounds reads are rejected.

The snapshot consists of a 64-byte `P5LG` header and up to 64 chronological 32-byte records. It records RX sequence, callback length, raw UART status, remaining DMA count, stream lengths, parser events and limited extension-header metadata. Persistent header counters include DMA-full callbacks, software FIFO push failures, discarded parser bytes, decoded frame requests, reply encoding/sending and the last processed COMMIT. RX status sampling preserves the adjacent original SR/DR reads exactly once, and records CNDTR afterward. The raw register values are observations; chip-specific status-bit meanings require independent evidence. See the C structures in `dp104_plc_diagnostic.h` for the byte layout.

The original configurator uses separate vendor queries: master `02 04` and PLC `08 42 64`. The PLC's D1 reply supplies a little-endian packed version to the master's existing display cache. The patched PLC version producer reports `0x0121`; the master display follows that real vendor response rather than a synthetic cache value. The F2 application query independently reports semantic version 1.2.1 with a fresh nonce. These application-version reports are distinct from the USB device descriptor. After an image update, normal USB reconnection and a fresh configurator connection may be needed to discard cached display values.

The wire protocol itself does not allocate flash or RAM; the paired firmware's placement and measured reservations are described in [the architecture record](architecture.md). A test or structurally valid image is not proof of target timing, flash readback or maximum heap/stack demand.
