/* SPDX-License-Identifier: MIT */
/* The EL2 stub links and loads at STUB_PA (QEMU virt RAM base, 0x40000000).
 * QEMU loads the ELF by its program headers (vaddr == paddr here), so the
 * entry and every section sit where the harness expects. */
ENTRY(_start)
SECTIONS
{
	. = DEFINED(BASE) ? BASE : 0x40200000;
	.text : { *(.text.start) *(.text*) }
	.rodata : { *(.rodata*) }
	.data : { *(.data*) }
	. = ALIGN(16);
	.bss : { *(.bss*) *(COMMON) }
	/DISCARD/ : { *(.note*) *(.comment) *(.ARM.attributes) }
}
