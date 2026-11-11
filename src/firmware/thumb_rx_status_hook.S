.syntax unified
.cpu cortex-m3
.thumb

.section .text.dp104_plc_rx_status_hook,"ax",%progbits
.balign 4
.globl dp104_plc_rx_status_hook
.type dp104_plc_rx_status_hook,%function
.thumb_func
dp104_plc_rx_status_hook:
    /* The ISR saved its exception LR before this BL. Preserve the live scratch
     * arguments and 8-byte SP alignment; no C call or flag-setting instruction.
     * Replay the exact adjacent SR/DR loads once, retaining pre-clear SR in r2.
     */
    push {r0,r1,r4,lr}
    ldr r2, [r3]
    ldr r3, [r3,#4]
    ldr r0, .Lrx_samples
    str r2, [r0]
    ldr r1, [r4,#0x70]
    str r1, [r0,#4]
    ldr r1, [r0,#8]
    add.w r1, r1, #1
    str r1, [r0,#8]
    pop {r0,r1,r4,pc}
    .balign 4
.Lrx_samples:
    /* The C target asserts all three offsets. The planner extends the owned
     * BSS/heap reservation; this is not storage borrowed from the stock heap.
     */
    .word __dp104_plc_state + 8896
.size dp104_plc_rx_status_hook, .-dp104_plc_rx_status_hook

.extern __dp104_plc_state
