    .option push
    .option rvc
    .option norelax

    .section .text.dp104_master_task_hook,"ax",@progbits
    .align 2
    .globl dp104_master_task_hook
    .type dp104_master_task_hook, @function

/* Preserve the vendor worker's entry caller-saved state around the bridge
 * pump, then tail-call the original worker with its arguments unchanged. */
dp104_master_task_hook:
    addi sp, sp, -64
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

    call dp104_master_task_pump

    lw   ra,  0(sp)
    lw   t0,  4(sp)
    lw   t1,  8(sp)
    lw   t2, 12(sp)
    lw   t3, 16(sp)
    lw   t4, 20(sp)
    lw   t5, 24(sp)
    lw   t6, 28(sp)
    lw   a0, 32(sp)
    lw   a1, 36(sp)
    lw   a2, 40(sp)
    lw   a3, 44(sp)
    lw   a4, 48(sp)
    lw   a5, 52(sp)
    lw   a6, 56(sp)
    lw   a7, 60(sp)
    addi sp, sp, 64
    j    dp104_master_task_legacy

    .size dp104_master_task_hook, .-dp104_master_task_hook
    .option pop
