#include "kernel.h"

void _riscv64_entry(void) {
	kmain(0, 0); /* hartid/dtb unused -- see arch/risc/riscv64_memmap.h */
	for (;;)
		riscv_wfi();
}
