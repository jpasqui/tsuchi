OUTPUT_ARCH(aarch64)
ENTRY(_start)
PHDRS {
    text PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
    dynamic PT_DYNAMIC FLAGS(6);
}
SECTIONS {
    . = 0;
    .text : { KEEP(*(.crt0)) *(.text .text.*) *(.plt .iplt) } :text
    . = ALIGN(0x1000);
    .rodata : { *(.rodata .rodata.*) } :rodata
    .rela.dyn : { *(.rela.dyn) *(.rela.plt) } :rodata
    .hash : { *(.hash) } :rodata
    .dynsym : { *(.dynsym) } :rodata
    .dynstr : { *(.dynstr) } :rodata
    . = ALIGN(0x1000);
    .init_array : { KEEP(*(.init_array .init_array.*)) } :data
    .dynamic : { *(.dynamic) } :data :dynamic
    .got : { *(.got .got.plt) } :data
    .data : { *(.data .data.* .data.rel.ro .data.rel.ro.*) } :data
    . = ALIGN(16);
    HIDDEN(__bss_start = .);
    .bss (NOLOAD) : { *(.bss.module_object) *(.bss .bss.*) *(COMMON) . = ALIGN(16); } :data
    HIDDEN(__bss_end = .);
    /DISCARD/ : { *(.comment .note .note.* .eh_frame .eh_frame_hdr) }
}
