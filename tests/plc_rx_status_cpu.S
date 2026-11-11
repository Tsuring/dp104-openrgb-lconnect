.syntax unified
.cpu cortex-m3
.thumb

.macro CHECK_WORD offset, expected, failure
    ldr r0, [r4, #\offset]
    ldr r1, =\expected
    cmp r0, r1
    bne .Lfail\failure
.endm

.macro CHECK_FLAGS failure
    ldr r0, [r4, #52]
    ldr r1, =0xf8000000
    and.w r0, r0, r1
    cmp r0, r1
    bne .Lfail\failure
.endm

.macro CHECK_SP failure
    ldr r0, [r4, #56]
    ldr r1, [r4, #60]
    cmp r0, r1
    bne .Lfail\failure
.endm

.section .text.startup,"ax",%progbits
.balign 4
.globl _start
.type _start,%function
.thumb_func
_start:
    /* Use only the synthetic RW segment; no Linux process data is inspected. */
    ldr r0, =__rx_test_stack_top
    mov sp, r0

    /* Fake USART SR/DR and DMA channel-6 CNDTR in ordinary mapped RAM. */
    ldr r3, =__rx_test_usart
    ldr r0, =0xa5a5f00d
    str r0, [r3]
    ldr r0, =0xd00dfeed
    str r0, [r3, #4]
    ldr r4, =__rx_test_dma
    movs r0, #72
    str r0, [r4, #0x70]
    ldr r0, =0xd6a6c6a6
    str r0, [r4, #0x6c]
    ldr r0, =0xda7ada7a
    str r0, [r4, #0x74]
    ldr r5, =__rx_test_usart_guard_before
    ldr r0, =0x51a7e001
    str r0, [r5]
    ldr r5, =__rx_test_usart_guard_after
    ldr r0, =0x51a7e002
    str r0, [r5]

    /* The hook-owned words are exactly the appended target fields. */
    ldr r5, =__dp104_plc_state + 8892
    ldr r0, =0xca11ab1e
    str r0, [r5]
    ldr r5, =__dp104_plc_state + 8896
    ldr r0, =0xbad00001
    str r0, [r5]
    ldr r0, =0xbad00002
    str r0, [r5, #4]
    ldr r0, =0xffffffff
    str r0, [r5, #8]
    ldr r5, =__dp104_plc_state + 8908
    ldr r0, =0xca11ab1f
    str r0, [r5]

    /* Seed callee-saved registers with distinct sentinels. */
    ldr r5, =0x55555555
    ldr r6, =0x66666666
    ldr r7, =0x77777777
    ldr r8, =0x88888888
    ldr r9, =0x99999999
    ldr r10, =0xaaaaaaaa
    ldr r11, =0xbbbbbbbb
    ldr r12, =0xcccccccc
    ldr r3, =__rx_test_usart
    ldr r4, =__rx_test_dma
    mov r0, sp
    str r0, [r4, #60]

    /* Set NZCVQ last. Following LDR/BL instructions do not alter flags. */
    ldr r0, =0xf8000000
    msr APSR_nzcvq, r0
    ldr r0, =0x11223344
    ldr r1, =0x55667788
    bl dp104_plc_rx_status_hook

    /* Snapshot outputs before the fixture's own comparisons alter APSR. */
    str r0, [r4, #0]
    mrs r0, APSR
    str r0, [r4, #52]
    str r1, [r4, #4]
    str r2, [r4, #8]
    str r3, [r4, #12]
    str r4, [r4, #16]
    str r5, [r4, #20]
    str r6, [r4, #24]
    str r7, [r4, #28]
    str r8, [r4, #32]
    str r9, [r4, #36]
    str r10, [r4, #40]
    str r11, [r4, #44]
    str r12, [r4, #48]
    mov r0, sp
    str r0, [r4, #56]

    /* The displaced instructions must leave the original SR/DR results. */
    CHECK_WORD 0, 0x11223344, 1
    CHECK_WORD 4, 0x55667788, 2
    CHECK_WORD 8, 0xa5a5f00d, 3
    CHECK_WORD 12, 0xd00dfeed, 4
    CHECK_WORD 16, __rx_test_dma, 5
    CHECK_WORD 20, 0x55555555, 6
    CHECK_WORD 24, 0x66666666, 7
    CHECK_WORD 28, 0x77777777, 8
    CHECK_WORD 32, 0x88888888, 9
    CHECK_WORD 36, 0x99999999, 10
    CHECK_WORD 40, 0xaaaaaaaa, 11
    CHECK_WORD 44, 0xbbbbbbbb, 12
    CHECK_WORD 48, 0xcccccccc, 13
    CHECK_FLAGS 14
    CHECK_SP 15

    ldr r5, =__dp104_plc_state + 8896
    ldr r0, [r5]
    ldr r1, =0xa5a5f00d
    cmp r0, r1
    bne .Lfail16
    ldr r0, [r5, #4]
    movs r1, #72
    cmp r0, r1
    bne .Lfail17
    ldr r0, [r5, #8]
    movs r1, #0
    cmp r0, r1
    bne .Lfail18

    /* Adjacent state, DMA, and fake USART canaries must remain intact. */
    ldr r5, =__dp104_plc_state + 8892
    ldr r0, [r5]
    ldr r1, =0xca11ab1e
    cmp r0, r1
    bne .Lfail19
    ldr r5, =__dp104_plc_state + 8908
    ldr r0, [r5]
    ldr r1, =0xca11ab1f
    cmp r0, r1
    bne .Lfail20
    ldr r4, =__rx_test_dma
    ldr r0, [r4, #0x6c]
    ldr r1, =0xd6a6c6a6
    cmp r0, r1
    bne .Lfail21
    ldr r0, [r4, #0x70]
    movs r1, #72
    cmp r0, r1
    bne .Lfail22
    ldr r0, [r4, #0x74]
    ldr r1, =0xda7ada7a
    cmp r0, r1
    bne .Lfail23
    ldr r3, =__rx_test_usart
    ldr r0, [r3]
    ldr r1, =0xa5a5f00d
    cmp r0, r1
    bne .Lfail24
    ldr r0, [r3, #4]
    ldr r1, =0xd00dfeed
    cmp r0, r1
    bne .Lfail25
    ldr r5, =__rx_test_usart_guard_before
    ldr r0, [r5]
    ldr r1, =0x51a7e001
    cmp r0, r1
    bne .Lfail26
    ldr r5, =__rx_test_usart_guard_after
    ldr r0, [r5]
    ldr r1, =0x51a7e002
    cmp r0, r1
    bne .Lfail27

    movs r0, #0
    b .Lexit

.Lfail1:  movs r0, #1;  b .Lexit
.Lfail2:  movs r0, #2;  b .Lexit
.Lfail3:  movs r0, #3;  b .Lexit
.Lfail4:  movs r0, #4;  b .Lexit
.Lfail5:  movs r0, #5;  b .Lexit
.Lfail6:  movs r0, #6;  b .Lexit
.Lfail7:  movs r0, #7;  b .Lexit
.Lfail8:  movs r0, #8;  b .Lexit
.Lfail9:  movs r0, #9;  b .Lexit
.Lfail10: movs r0, #10; b .Lexit
.Lfail11: movs r0, #11; b .Lexit
.Lfail12: movs r0, #12; b .Lexit
.Lfail13: movs r0, #13; b .Lexit
.Lfail14: movs r0, #14; b .Lexit
.Lfail15: movs r0, #15; b .Lexit
.Lfail16: movs r0, #16; b .Lexit
.Lfail17: movs r0, #17; b .Lexit
.Lfail18: movs r0, #18; b .Lexit
.Lfail19: movs r0, #19; b .Lexit
.Lfail20: movs r0, #20; b .Lexit
.Lfail21: movs r0, #21; b .Lexit
.Lfail22: movs r0, #22; b .Lexit
.Lfail23: movs r0, #23; b .Lexit
.Lfail24: movs r0, #24; b .Lexit
.Lfail25: movs r0, #25; b .Lexit
.Lfail26: movs r0, #26; b .Lexit
.Lfail27: movs r0, #27; b .Lexit

.Lexit:
    movs r7, #1
    svc #0
    b .Lexit
.size _start, .-_start
