// Hook B trampoline, copied into the guarded executable plugin tail.
// Preserve every caller-saved GPR, all SIMD lanes (including the upper halves
// of v8-v15), NZCV, FPSR and FPCR around the C helper. x19-x29 are callee-saved.
// No plugin register-liveness assumptions beyond the displaced w10 load.
// Slots are patched to apply_dual and the branch back to base+0x5c6d0.

    .section .text.picoet_leaf, "ax", %progbits
    .globl picoet_leaf_start, picoet_leaf_end
    .hidden picoet_leaf_start, picoet_leaf_end
    .globl picoet_leaf_apply_slot, picoet_leaf_ret_pc
    .hidden picoet_leaf_apply_slot, picoet_leaf_ret_pc
    .type picoet_leaf_start, %function
    .p2align 2
picoet_leaf_start:
    sub sp, sp, #704
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x30, [sp, #144]
    stp q0, q1, [sp, #160]
    stp q2, q3, [sp, #192]
    stp q4, q5, [sp, #224]
    stp q6, q7, [sp, #256]
    stp q8, q9, [sp, #288]
    stp q10, q11, [sp, #320]
    stp q12, q13, [sp, #352]
    stp q14, q15, [sp, #384]
    stp q16, q17, [sp, #416]
    stp q18, q19, [sp, #448]
    stp q20, q21, [sp, #480]
    stp q22, q23, [sp, #512]
    stp q24, q25, [sp, #544]
    stp q26, q27, [sp, #576]
    stp q28, q29, [sp, #608]
    stp q30, q31, [sp, #640]
    mrs x16, nzcv
    str x16, [sp, #672]
    mrs x16, fpsr
    str x16, [sp, #680]
    mrs x16, fpcr
    str x16, [sp, #688]
    add x0, sp, #704
    ldr x16, picoet_leaf_apply_slot
    blr x16
    ldr x16, [sp, #688]
    msr fpcr, x16
    ldr x16, [sp, #680]
    msr fpsr, x16
    ldr x16, [sp, #672]
    msr nzcv, x16
    ldp q0, q1, [sp, #160]
    ldp q2, q3, [sp, #192]
    ldp q4, q5, [sp, #224]
    ldp q6, q7, [sp, #256]
    ldp q8, q9, [sp, #288]
    ldp q10, q11, [sp, #320]
    ldp q12, q13, [sp, #352]
    ldp q14, q15, [sp, #384]
    ldp q16, q17, [sp, #416]
    ldp q18, q19, [sp, #448]
    ldp q20, q21, [sp, #480]
    ldp q22, q23, [sp, #512]
    ldp q24, q25, [sp, #544]
    ldp q26, q27, [sp, #576]
    ldp q28, q29, [sp, #608]
    ldp q30, q31, [sp, #640]
    ldp x0, x1, [sp, #0]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp x8, x9, [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldp x16, x17, [sp, #128]
    ldp x18, x30, [sp, #144]
    add sp, sp, #704
    ldr w10, [sp, #0x24]
picoet_leaf_ret_pc:
    b .
    .p2align 3
picoet_leaf_apply_slot:
    .quad 0
picoet_leaf_end:
    .size picoet_leaf_start, .-picoet_leaf_start
