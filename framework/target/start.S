// The start of the module as the system's loader expects it, and the entry the
// game calls every frame.

.section .crt0,"ax",%progbits
.global _start
.hidden _start
_start:
    .word 0
    .word module_header - _start
module_header:
    .ascii "MOD0"
    .word _DYNAMIC - module_header
    .word __bss_start - module_header
    .word __bss_end - module_header
    .word 0
    .word 0
    .word module_object - module_header

.section .bss.module_object,"aw",%nobits
.balign 16
.global module_object
.hidden module_object
module_object:
    .space 0x200

// Stands in for the engine's per-frame function. The framework runs first,
// with the frame's length in s0; then the game's own function runs with its
// arguments as they came.
.text
.balign 4
.global tsuchi_engine_tick
.hidden tsuchi_engine_tick
tsuchi_engine_tick:
    stp x29, x30, [sp, #-48]!
    mov x29, sp
    stp x0, x1, [sp, #16]
    str q0, [sp, #32]
    bl tsuchi_target_frame
    ldr q0, [sp, #32]
    ldp x0, x1, [sp, #16]
    ldp x29, x30, [sp], #48
    adrp x16, tsuchi_previous_tick
    ldr x16, [x16, :lo12:tsuchi_previous_tick]
    br x16

// The supply of hook thunks (framework/core/hooks.h). Each is two
// instructions: its number, then a jump to the shared part, which asks where
// that thunk goes right now and jumps there with every argument as it came.
// The framework counts them by where they end, and uses kHookThunks at most.
.equ TSUCHI_HOOK_THUNKS, 1024
.balign 4
.global tsuchi_hook_thunks
.hidden tsuchi_hook_thunks
tsuchi_hook_thunks:
.set thunk_number, 0
.rept TSUCHI_HOOK_THUNKS
    mov w17, #thunk_number
    b tsuchi_hook_jump
.set thunk_number, thunk_number + 1
.endr
.global tsuchi_hook_thunks_end
.hidden tsuchi_hook_thunks_end
tsuchi_hook_thunks_end:

tsuchi_hook_jump:
    stp x29, x30, [sp, #-224]!
    mov x29, sp
    stp x0, x1, [sp, #16]
    stp x2, x3, [sp, #32]
    stp x4, x5, [sp, #48]
    stp x6, x7, [sp, #64]
    str x8, [sp, #80]
    stp q0, q1, [sp, #96]
    stp q2, q3, [sp, #128]
    stp q4, q5, [sp, #160]
    stp q6, q7, [sp, #192]
    mov w0, w17
    add x1, sp, #224
    ldr x2, [sp, #8]
    bl tsuchi_hook_enter
    mov x16, x0
    mov x17, x1
    ldp q6, q7, [sp, #192]
    ldp q4, q5, [sp, #160]
    ldp q2, q3, [sp, #128]
    ldp q0, q1, [sp, #96]
    ldr x8, [sp, #80]
    ldp x6, x7, [sp, #64]
    ldp x4, x5, [sp, #48]
    ldp x2, x3, [sp, #32]
    ldp x0, x1, [sp, #16]
    ldp x29, x30, [sp], #224
    cbz x17, 1f
    adr x30, tsuchi_hook_return
1:
    br x16

// A hook returns with its caller's stack intact. Save every AAPCS64 return
// register before releasing its record, then restore them and the original LR.
// The incoming stack arguments were never moved, including for tail calls.
tsuchi_hook_return:
    stp x29, x30, [sp, #-112]!
    mov x29, sp
    stp x0, x1, [sp, #16]
    str x8, [sp, #32]
    stp q0, q1, [sp, #48]
    stp q2, q3, [sp, #80]
    add x0, sp, #112
    bl tsuchi_hook_leave
    mov x16, x0
    ldp q2, q3, [sp, #80]
    ldp q0, q1, [sp, #48]
    ldr x8, [sp, #32]
    ldp x0, x1, [sp, #16]
    ldp x29, x30, [sp], #112
    mov x30, x16
    br x16

// Where the patch's block sends a code site, with the site's number in w17.
.global tsuchi_site_jump
.hidden tsuchi_site_jump
tsuchi_site_jump:
    orr w17, w17, #0x80000000
    b tsuchi_hook_jump

// The supply of script hook gateways (framework/core/dynamic_hooks.h). The hook
// engine calls one in place of the game's function. Each is two instructions:
// its number, then the shared part, which keeps every register a call can use
// in a CallFrame: x0 to x7, x8 for a result the caller gives memory for, and q0
// to q7 whole. After tsuchi_dynamic_dispatch, the result registers come back
// from the frame: x0 and x1, x8, and q0 to q3, as AAPCS64 returns them.
// CallFrame: x[8] at 0, x8 at 64, q[8] at 80, 208 bytes.
.equ TSUCHI_DYNAMIC_GATEWAYS, 256
.balign 4
.global tsuchi_dynamic_gateways
.hidden tsuchi_dynamic_gateways
tsuchi_dynamic_gateways:
.set gateway_number, 0
.rept TSUCHI_DYNAMIC_GATEWAYS
    mov w17, #gateway_number
    b tsuchi_dynamic_gateway
.set gateway_number, gateway_number + 1
.endr
.global tsuchi_dynamic_gateways_end
.hidden tsuchi_dynamic_gateways_end
tsuchi_dynamic_gateways_end:

tsuchi_dynamic_gateway:
    sub sp, sp, #224
    stp x29, x30, [sp, #208]
    add x29, sp, #208
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    str x8, [sp, #64]
    stp q0, q1, [sp, #80]
    stp q2, q3, [sp, #112]
    stp q4, q5, [sp, #144]
    stp q6, q7, [sp, #176]
    mov w0, w17
    mov x1, sp
    bl tsuchi_dynamic_dispatch
    ldp q2, q3, [sp, #112]
    ldp q0, q1, [sp, #80]
    ldr x8, [sp, #64]
    ldp x0, x1, [sp, #0]
    ldp x29, x30, [sp, #208]
    add sp, sp, #224
    ret

// Calls `target` (x0) with the registers in a CallFrame (x1), x8 and q0 to q7
// included, and stores the result registers back: x0 and x1, and q0 to q3.
.global tsuchi_dynamic_call
.hidden tsuchi_dynamic_call
tsuchi_dynamic_call:
    stp x29, x30, [sp, #-32]!
    mov x29, sp
    str x19, [sp, #16]
    mov x19, x1
    mov x16, x0
    ldp q0, q1, [x19, #80]
    ldp q2, q3, [x19, #112]
    ldp q4, q5, [x19, #144]
    ldp q6, q7, [x19, #176]
    ldr x8, [x19, #64]
    ldp x0, x1, [x19, #0]
    ldp x2, x3, [x19, #16]
    ldp x4, x5, [x19, #32]
    ldp x6, x7, [x19, #48]
    blr x16
    stp x0, x1, [x19, #0]
    stp q0, q1, [x19, #80]
    stp q2, q3, [x19, #112]
    ldr x19, [sp, #16]
    ldp x29, x30, [sp], #32
    ret

// Where the patch's native routes enter the framework (tools/gen/stubs.py, native_shared). The route's own code
// pushed x30 and x16, then the shared code pushed x17 and x0, x1 and the flags: sp+0 x0, +8 x1, +16 the flags,
// +32 x17, +48 x30, +56 x16. x30 is the route's `returned`, which says which route this is. Every register is
// kept in a frame (native_routes.h) for the route's handler, which may change them; then the frame is put back
// and the route's own code goes on with what the game had there, as when no framework is there.
.equ NATIVE_FRAME, 0x340
.global tsuchi_native_jump
.hidden tsuchi_native_jump
tsuchi_native_jump:
    sub sp, sp, #NATIVE_FRAME
    stp x2, x3, [sp, #0x10]
    stp x4, x5, [sp, #0x20]
    stp x6, x7, [sp, #0x30]
    stp x8, x9, [sp, #0x40]
    stp x10, x11, [sp, #0x50]
    stp x12, x13, [sp, #0x60]
    stp x14, x15, [sp, #0x70]
    stp x18, x19, [sp, #0x90]
    stp x20, x21, [sp, #0xA0]
    stp x22, x23, [sp, #0xB0]
    stp x24, x25, [sp, #0xC0]
    stp x26, x27, [sp, #0xD0]
    stp x28, x29, [sp, #0xE0]
    ldr x2, [sp, #NATIVE_FRAME]                  // the game's x0 and x1
    ldr x3, [sp, #NATIVE_FRAME + 8]
    stp x2, x3, [sp, #0x00]
    ldr x2, [sp, #NATIVE_FRAME + 32]             // its x17
    ldr x3, [sp, #NATIVE_FRAME + 56]             // its x16
    stp x3, x2, [sp, #0x80]
    ldr x2, [sp, #NATIVE_FRAME + 48]             // its x30
    add x3, sp, #NATIVE_FRAME + 64               // its sp
    stp x2, x3, [sp, #0xF0]
    stp q0, q1, [sp, #0x100]
    stp q2, q3, [sp, #0x120]
    stp q4, q5, [sp, #0x140]
    stp q6, q7, [sp, #0x160]
    stp q8, q9, [sp, #0x180]
    stp q10, q11, [sp, #0x1A0]
    stp q12, q13, [sp, #0x1C0]
    stp q14, q15, [sp, #0x1E0]
    stp q16, q17, [sp, #0x200]
    stp q18, q19, [sp, #0x220]
    stp q20, q21, [sp, #0x240]
    stp q22, q23, [sp, #0x260]
    stp q24, q25, [sp, #0x280]
    stp q26, q27, [sp, #0x2A0]
    stp q28, q29, [sp, #0x2C0]
    stp q30, q31, [sp, #0x2E0]
    mrs x2, fpcr
    mrs x3, fpsr
    str x2, [sp, #0x300]
    str x3, [sp, #0x308]
    ldr x2, [sp, #NATIVE_FRAME + 16]             // the flags
    str x2, [sp, #0x310]
    str x30, [sp, #0x318]                        // where the route goes on
    mov x0, sp
    mov x1, x30
    bl tsuchi_native_dispatch
    ldr x2, [sp, #0x300]
    ldr x3, [sp, #0x308]
    msr fpcr, x2
    msr fpsr, x3
    ldp q0, q1, [sp, #0x100]
    ldp q2, q3, [sp, #0x120]
    ldp q4, q5, [sp, #0x140]
    ldp q6, q7, [sp, #0x160]
    ldp q8, q9, [sp, #0x180]
    ldp q10, q11, [sp, #0x1A0]
    ldp q12, q13, [sp, #0x1C0]
    ldp q14, q15, [sp, #0x1E0]
    ldp q16, q17, [sp, #0x200]
    ldp q18, q19, [sp, #0x220]
    ldp q20, q21, [sp, #0x240]
    ldp q22, q23, [sp, #0x260]
    ldp q24, q25, [sp, #0x280]
    ldp q26, q27, [sp, #0x2A0]
    ldp q28, q29, [sp, #0x2C0]
    ldp q30, q31, [sp, #0x2E0]
    // What the shared code pops on its way back: x0, x1, the flags and x17, and the route pops x30 and x16.
    ldp x2, x3, [sp, #0x00]
    str x2, [sp, #NATIVE_FRAME]
    str x3, [sp, #NATIVE_FRAME + 8]
    ldr x2, [sp, #0x310]
    str x2, [sp, #NATIVE_FRAME + 16]
    ldp x3, x2, [sp, #0x80]
    str x2, [sp, #NATIVE_FRAME + 32]
    str x3, [sp, #NATIVE_FRAME + 56]
    ldr x2, [sp, #0xF0]
    str x2, [sp, #NATIVE_FRAME + 48]
    ldr x30, [sp, #0x318]
    ldp x4, x5, [sp, #0x20]
    ldp x6, x7, [sp, #0x30]
    ldp x8, x9, [sp, #0x40]
    ldp x10, x11, [sp, #0x50]
    ldp x12, x13, [sp, #0x60]
    ldp x14, x15, [sp, #0x70]
    ldp x18, x19, [sp, #0x90]
    ldp x20, x21, [sp, #0xA0]
    ldp x22, x23, [sp, #0xB0]
    ldp x24, x25, [sp, #0xC0]
    ldp x26, x27, [sp, #0xD0]
    ldp x28, x29, [sp, #0xE0]
    ldp x2, x3, [sp, #0x10]
    add sp, sp, #NATIVE_FRAME
    ldr x17, [sp, #16]
    msr nzcv, x17
    ldp x0, x1, [sp], #32
    ldp x17, xzr, [sp], #16
    ret
