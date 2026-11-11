    .option push
    .option rvc
    .option norelax

    .section .text.dp104_master_host_hook,"ax",@progbits
    .align 2
    .globl dp104_master_host_hook
    .type dp104_master_host_hook, @function

/*
 * Non-installed RawHID callback veneer for the selected RV32 master.
 * The callback ABI is dp104_master_host_intercept(a0, a1, a2) -> bool.
 * A false result tail-calls the displaced legacy callback with its entry
 * caller-saved state intact. A true result returns zero to the vendor caller.
 *
 * The direct JAL tail transfer intentionally avoids borrowing a caller-saved
 * register after restoring it. The final link must keep the legacy callback
 * within the architectural +/-1 MiB JAL range.
 */
dp104_master_host_hook:
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

    /* Entry sp is ABI-aligned; subtracting 64 preserves 16-byte alignment. */
    call dp104_master_host_intercept
    beqz a0, .Ldp104_master_host_legacy

.Ldp104_master_host_handled:
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
    li   a0, 0
    ret

.Ldp104_master_host_legacy:
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
    j    dp104_master_host_legacy

    .size dp104_master_host_hook, .-dp104_master_host_hook
    .option pop
