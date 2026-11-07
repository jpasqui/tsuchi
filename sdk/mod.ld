/* The layout of a compiled mod: code, then what is only read, then data, each on its own
 * pages and at the same place in the file as in memory, and the memory that starts as
 * zeros on the pages after the data. */
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
    /* The functions that set up the mod's global variables. They are kept as a plain list,
     * which the mod's own start-up code runs once, so that the loader does not run them too. */
    .tsuchi_setup : {
        __tsuchi_setup_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        __tsuchi_setup_end = .;
    } :data
    .dynamic : { *(.dynamic) } :data :dynamic
    .got : { *(.got .got.plt) } :data
    .data : { *(.data .data.* .data.rel.ro .data.rel.ro.*) } :data
    . = ALIGN(0x1000);
    __bss_start = .;
    .bss (NOLOAD) : { *(.bss.module_object) *(.bss .bss.*) *(COMMON) . = ALIGN(16); } :data
    __bss_end = .;
    /DISCARD/ : { *(.comment .note .note.* .eh_frame .eh_frame_hdr) }
}
