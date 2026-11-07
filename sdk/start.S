// The start of a compiled mod: two words the loader reads, room for the file's header,
// which build_mod.py fills in, and then the header that says where the module's tables are.
.section .crt0,"ax",%progbits
.global _start
.hidden _start
_start:
    .word 0
    .word module_header - _start
    .fill 0x78, 1, 0
module_header:
    .ascii "MOD0"
    .word _DYNAMIC - module_header
    .word __bss_start - module_header
    .word __bss_end - module_header
    .word 0
    .word 0
    .word module_object - module_header

// The loader's own record of the module.
.section .bss.module_object,"aw",%nobits
.balign 16
.global module_object
.hidden module_object
module_object:
    .space 0x200
