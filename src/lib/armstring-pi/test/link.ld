ENTRY(_start)
SECTIONS {
  . = 0x10000;
  .text : { *(.text.start) *(.text*) *(.rodata*) }
  .data : { *(.data*) }
  . = ALIGN(4); __bss_start = .;
  .bss  : { *(.bss*) *(COMMON) }
  . = ALIGN(4); __bss_end = .;
  /* first-level page table: 4096 entries x 4 bytes, 16KB-aligned */
  . = ALIGN(16384); _page_table = .; . = . + 16384;
  . = ALIGN(8); . = . + 0x10000; _stack_top = .;
}
