// A stand-in handler for the real ARM64 register-capture entry.
.text
.global tsuchi_native_dispatch
tsuchi_native_dispatch:
    ldr x2, [x0, #0x28]
    add x2, x2, #7
    str x2, [x0, #0x28]
    ldr x2, [x0, #0x140]
    eor x2, x2, #0xff
    str x2, [x0, #0x140]
    ldr x2, [x0, #0x310]
    eor x2, x2, #0x20000000
    str x2, [x0, #0x310]
    ldr x2, [x0, #0x300]
    eor x2, x2, #0x400000
    str x2, [x0, #0x300]
    ldr x2, [x0, #0x308]
    eor x2, x2, #1
    str x2, [x0, #0x308]
    adrp x2, dispatch_calls
    add x2, x2, :lo12:dispatch_calls
    ldr w3, [x2]
    add w3, w3, #1
    str w3, [x2]
    ret
.data
.global dispatch_calls
dispatch_calls:
    .word 0
