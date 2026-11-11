    .option push
    .option norvc
    .option norelax

    .section .text.test_start,"ax",@progbits
    .align 2
    .globl _start
    .type _start, @function
_start:
    call main
    li   a7, 93
    ecall
    .size _start, .-_start

    .section .text.test_cases,"ax",@progbits
    .align 2

    .macro SAVE_TEST_FRAME
    addi sp, sp, -128
    sw   ra,  0(sp)
    sw   s0,  4(sp)
    sw   s1,  8(sp)
    sw   s2, 12(sp)
    sw   s3, 16(sp)
    sw   s4, 20(sp)
    sw   s5, 24(sp)
    sw   s6, 28(sp)
    sw   s7, 32(sp)
    sw   s8, 36(sp)
    sw   s9, 40(sp)
    sw   s10,44(sp)
    sw   s11,48(sp)
    la   t0, hook_test_expected_sp
    sw   sp, 0(t0)
    .endm

    .macro SET_CALLEE_SENTINELS
    li   s0,  0x51000000
    li   s1,  0x51000001
    li   s2,  0x51000002
    li   s3,  0x51000003
    li   s4,  0x51000004
    li   s5,  0x51000005
    li   s6,  0x51000006
    li   s7,  0x51000007
    li   s8,  0x51000008
    li   s9,  0x51000009
    li   s10, 0x5100000a
    li   s11, 0x5100000b
    .endm

    .macro SET_CALLER_SENTINELS
    li   t0, 0x70000010
    li   t1, 0x70000011
    li   t2, 0x70000012
    li   t3, 0x70000013
    li   t4, 0x70000014
    li   t5, 0x70000015
    li   t6, 0x70000016
    li   a0, 0x11223344
    li   a1, 0x22334455
    li   a2, 0x33445566
    li   a3, 0x44556677
    li   a4, 0x55667788
    li   a5, 0x66778899
    li   a6, 0x778899aa
    li   a7, 0x1234abcd
    .endm

    .macro RESTORE_TEST_FRAME
    lw   ra,  0(sp)
    lw   s0,  4(sp)
    lw   s1,  8(sp)
    lw   s2, 12(sp)
    lw   s3, 16(sp)
    lw   s4, 20(sp)
    lw   s5, 24(sp)
    lw   s6, 28(sp)
    lw   s7, 32(sp)
    lw   s8, 36(sp)
    lw   s9, 40(sp)
    lw   s10,44(sp)
    lw   s11,48(sp)
    addi sp, sp, 128
    ret
    .endm

    .macro CHECK_CALLEE_SENTINELS fail_label
    li   t0, 0x51000000
    bne  s0, t0, \fail_label
    li   t0, 0x51000001
    bne  s1, t0, \fail_label
    li   t0, 0x51000002
    bne  s2, t0, \fail_label
    li   t0, 0x51000003
    bne  s3, t0, \fail_label
    li   t0, 0x51000004
    bne  s4, t0, \fail_label
    li   t0, 0x51000005
    bne  s5, t0, \fail_label
    li   t0, 0x51000006
    bne  s6, t0, \fail_label
    li   t0, 0x51000007
    bne  s7, t0, \fail_label
    li   t0, 0x51000008
    bne  s8, t0, \fail_label
    li   t0, 0x51000009
    bne  s9, t0, \fail_label
    li   t0, 0x5100000a
    bne  s10, t0, \fail_label
    li   t0, 0x5100000b
    bne  s11, t0, \fail_label
    .endm

    .globl dp104_test_false_case
    .type dp104_test_false_case, @function
dp104_test_false_case:
    SAVE_TEST_FRAME
    la   t0, hook_test_intercept_mode
    sw   zero, 0(t0)
    SET_CALLEE_SENTINELS
    SET_CALLER_SENTINELS
    call dp104_master_host_hook
    .globl dp104_test_false_after_hook
dp104_test_false_after_hook:
    la   t0, hook_test_expected_sp
    lw   t1, 0(t0)
    bne  sp, t1, .Lfalse_bad_stack
    li   t0, 0x11223344
    bne  a0, t0, .Lfalse_bad_return
    CHECK_CALLEE_SENTINELS .Lfalse_bad_callee
    li   a0, 0
    j    .Lfalse_done
.Lfalse_bad_stack:
    li   a0, 1
    j    .Lfalse_done
.Lfalse_bad_return:
    li   a0, 2
    j    .Lfalse_done
.Lfalse_bad_callee:
    li   a0, 3
.Lfalse_done:
    RESTORE_TEST_FRAME
    .size dp104_test_false_case, .-dp104_test_false_case

    .globl dp104_test_handled_case
    .type dp104_test_handled_case, @function
dp104_test_handled_case:
    SAVE_TEST_FRAME
    la   t0, hook_test_intercept_mode
    li   t1, 1
    sw   t1, 0(t0)
    SET_CALLEE_SENTINELS
    SET_CALLER_SENTINELS
    call dp104_master_host_hook
    .Lhandled_after_hook:
    /* Capture every caller-saved register before using any scratch register. */
    sw   ra,  64(sp)
    sw   t0,  68(sp)
    sw   t1,  72(sp)
    sw   t2,  76(sp)
    sw   t3,  80(sp)
    sw   t4,  84(sp)
    sw   t5,  88(sp)
    sw   t6,  92(sp)
    sw   a0,  96(sp)
    sw   a1, 100(sp)
    sw   a2, 104(sp)
    sw   a3, 108(sp)
    sw   a4, 112(sp)
    sw   a5, 116(sp)
    sw   a6, 120(sp)
    sw   a7, 124(sp)

    la   t0, hook_test_expected_sp
    lw   t1, 0(t0)
    bne  sp, t1, .Lhandled_bad_stack
    la   t0, .Lhandled_after_hook
    lw   t1, 64(sp)
    bne  t0, t1, .Lhandled_bad_ra
    li   t0, 0x70000010
    lw   t1, 68(sp)
    bne  t0, t1, .Lhandled_bad_t
    li   t0, 0x70000011
    lw   t1, 72(sp)
    bne  t0, t1, .Lhandled_bad_t
    li   t0, 0x70000012
    lw   t1, 76(sp)
    bne  t0, t1, .Lhandled_bad_t
    li   t0, 0x70000013
    lw   t1, 80(sp)
    bne  t0, t1, .Lhandled_bad_t
    li   t0, 0x70000014
    lw   t1, 84(sp)
    bne  t0, t1, .Lhandled_bad_t
    li   t0, 0x70000015
    lw   t1, 88(sp)
    bne  t0, t1, .Lhandled_bad_t
    li   t0, 0x70000016
    lw   t1, 92(sp)
    bne  t0, t1, .Lhandled_bad_t
    lw   t1, 96(sp)
    bnez t1, .Lhandled_bad_result
    li   t0, 0x22334455
    lw   t1, 100(sp)
    bne  t0, t1, .Lhandled_bad_a
    li   t0, 0x33445566
    lw   t1, 104(sp)
    bne  t0, t1, .Lhandled_bad_a
    li   t0, 0x44556677
    lw   t1, 108(sp)
    bne  t0, t1, .Lhandled_bad_a
    li   t0, 0x55667788
    lw   t1, 112(sp)
    bne  t0, t1, .Lhandled_bad_a
    li   t0, 0x66778899
    lw   t1, 116(sp)
    bne  t0, t1, .Lhandled_bad_a
    li   t0, 0x778899aa
    lw   t1, 120(sp)
    bne  t0, t1, .Lhandled_bad_a
    li   t0, 0x1234abcd
    lw   t1, 124(sp)
    bne  t0, t1, .Lhandled_bad_a
    CHECK_CALLEE_SENTINELS .Lhandled_bad_callee
    li   a0, 0
    j    .Lhandled_done
.Lhandled_bad_stack:
    li   a0, 30
    j    .Lhandled_done
.Lhandled_bad_ra:
    li   a0, 31
    j    .Lhandled_done
.Lhandled_bad_t:
    li   a0, 32
    j    .Lhandled_done
.Lhandled_bad_result:
    li   a0, 33
    j    .Lhandled_done
.Lhandled_bad_a:
    li   a0, 34
    j    .Lhandled_done
.Lhandled_bad_callee:
    li   a0, 35
.Lhandled_done:
    RESTORE_TEST_FRAME
    .size dp104_test_handled_case, .-dp104_test_handled_case

    .section .text.test_callbacks,"ax",@progbits
    .align 2
    .globl dp104_master_host_intercept
    .type dp104_master_host_intercept, @function
dp104_master_host_intercept:
    /* Keep the callback ABI's stack alignment, and save only its own return state. */
    addi sp, sp, -16
    sw   ra, 12(sp)
    sw   s0,  8(sp)

    la   t0, hook_test_intercept_args
    sw   a0, 0(t0)
    sw   a1, 4(t0)
    sw   a2, 8(t0)
    la   t0, hook_test_intercept_calls
    lw   t1, 0(t0)
    addi t1, t1, 1
    sw   t1, 0(t0)
    andi t1, sp, 15
    la   t0, hook_test_callback_sp_mod_16
    sw   t1, 0(t0)
    la   t0, hook_test_intercept_mode
    lw   s0, 0(t0)

    /* Deliberately overwrite caller-saved state; return values are mode-driven. */
    li   ra, 0x6a6a0001
    li   t0, 0x6a6a0010
    li   t1, 0x6a6a0011
    li   t2, 0x6a6a0012
    li   t3, 0x6a6a0013
    li   t4, 0x6a6a0014
    li   t5, 0x6a6a0015
    li   t6, 0x6a6a0016
    li   a1, 0x6a6a0021
    li   a2, 0x6a6a0022
    li   a3, 0x6a6a0023
    li   a4, 0x6a6a0024
    li   a5, 0x6a6a0025
    li   a6, 0x6a6a0026
    li   a7, 0x6a6a0027
    mv   a0, s0

    lw   s0, 8(sp)
    lw   ra, 12(sp)
    addi sp, sp, 16
    ret
    .size dp104_master_host_intercept, .-dp104_master_host_intercept

    .globl dp104_master_host_legacy
    .type dp104_master_host_legacy, @function
dp104_master_host_legacy:
    /* Record the exact state delivered by the veneer before using scratch regs. */
    addi sp, sp, -80
    sw   ra,  0(sp)
    sw   t0,  4(sp)
    sw   t1,  8(sp)
    sw   t2, 12(sp)
    sw   t3, 16(sp)
    sw   t4, 20(sp)
    sw   t5, 24(sp)
    sw   t6, 28(sp)
    sw   a0, 32(sp)
    sw   a1, 36(sp)
    sw   a2, 40(sp)
    sw   a3, 44(sp)
    sw   a4, 48(sp)
    sw   a5, 52(sp)
    sw   a6, 56(sp)
    sw   a7, 60(sp)

    addi t1, sp, 80
    la   t0, hook_test_legacy_entry_sp
    sw   t1, 0(t0)
    la   t0, hook_test_legacy_observed
    lw   t1,  0(sp)
    sw   t1,  0(t0)
    lw   t1,  4(sp)
    sw   t1,  4(t0)
    lw   t1,  8(sp)
    sw   t1,  8(t0)
    lw   t1, 12(sp)
    sw   t1, 12(t0)
    lw   t1, 16(sp)
    sw   t1, 16(t0)
    lw   t1, 20(sp)
    sw   t1, 20(t0)
    lw   t1, 24(sp)
    sw   t1, 24(t0)
    lw   t1, 28(sp)
    sw   t1, 28(t0)
    lw   t1, 32(sp)
    sw   t1, 32(t0)
    lw   t1, 36(sp)
    sw   t1, 36(t0)
    lw   t1, 40(sp)
    sw   t1, 40(t0)
    lw   t1, 44(sp)
    sw   t1, 44(t0)
    lw   t1, 48(sp)
    sw   t1, 48(t0)
    lw   t1, 52(sp)
    sw   t1, 52(t0)
    lw   t1, 56(sp)
    sw   t1, 56(t0)
    lw   t1, 60(sp)
    sw   t1, 60(t0)
    la   t0, hook_test_legacy_calls
    lw   t1, 0(t0)
    addi t1, t1, 1
    sw   t1, 0(t0)

    /* A passthrough legacy stub returns the original first argument in a0. */
    lw   a0, 32(sp)
    lw   ra,  0(sp)
    addi sp, sp, 80
    ret
    .size dp104_master_host_legacy, .-dp104_master_host_legacy

    .option pop
