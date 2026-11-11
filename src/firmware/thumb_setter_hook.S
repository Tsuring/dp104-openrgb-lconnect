.syntax unified
.thumb

.section .text.dp104_plc_setter_hook,"ax",%progbits
.balign 2
.globl dp104_plc_setter_hook
.type dp104_plc_setter_hook,%function
.thumb_func
dp104_plc_setter_hook:
    /* Save original setter arguments; 24 bytes preserves 8-byte AAPCS alignment. */
    push.w {r0-r3, r12, lr}
    bl dp104_plc_should_skip_setter
    cmp r0, #0
    beq .Ldp104_plc_setter_legacy

    pop.w {r0-r3, r12, lr}
    bx lr

.Ldp104_plc_setter_legacy:
    pop.w {r0-r3, r12, lr}
    b.w dp104_plc_setter_legacy

.size dp104_plc_setter_hook, .-dp104_plc_setter_hook

.extern dp104_plc_should_skip_setter
.extern dp104_plc_setter_legacy
