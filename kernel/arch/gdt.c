/* Flat GDT: one 4GB code segment, one 4GB data segment, ring 0 only for
 * now (no ring3 until userspace, P5+), plus a TSS descriptor. The
 * Multiboot loader leaves *some* working flat GDT in place -- enough
 * that P3 could run at all -- but its exact contents are unspecified,
 * so we load our own as soon as possible, per the spec's own advice. */
#include "kernel.h"

struct gdt_entry {
	unsigned short limit_low;
	unsigned short base_low;
	unsigned char base_mid;
	unsigned char access;
	unsigned char granularity;
	unsigned char base_high;
} __attribute__((packed));

struct gdt_ptr {
	unsigned short limit;
	unsigned int base;
} __attribute__((packed));

struct tss_entry {
	unsigned int prev_tss;
	unsigned int esp0;
	unsigned int ss0;
	unsigned int unused[23];
} __attribute__((packed));

#define GDT_ENTRIES 9
static struct gdt_entry gdt[GDT_ENTRIES];
static struct gdt_ptr gp;
struct tss_entry tss;

static void gdt_set_entry(int i, unsigned int base, unsigned int limit,
                           unsigned char access, unsigned char gran) {
	gdt[i].base_low = base & 0xFFFF;
	gdt[i].base_mid = (base >> 16) & 0xFF;
	gdt[i].base_high = (base >> 24) & 0xFF;
	gdt[i].limit_low = limit & 0xFFFF;
	gdt[i].granularity = ((limit >> 16) & 0x0F) | (gran & 0xF0);
	gdt[i].access = access;
}

static inline void gdt_flush(struct gdt_ptr *p) {
	__asm__ volatile (
		"lgdt (%0)\n"
		"movw $0x10, %%ax\n"
		"movw %%ax, %%ds\n"
		"movw %%ax, %%es\n"
		"movw %%ax, %%fs\n"
		"movw %%ax, %%gs\n"
		"movw %%ax, %%ss\n"
		"ljmp $0x08, $1f\n"
		"1:\n"
		: : "r"(p) : "eax"
	);
}

static inline void tss_flush(void) {
	__asm__ volatile ("movw $0x2B, %%ax\n ltr %%ax\n" : : : "eax");
}

void gdt_init(void) {
	gp.limit = (sizeof(struct gdt_entry) * GDT_ENTRIES) - 1;
	gp.base = (unsigned int)(unsigned long)&gdt;

	gdt_set_entry(0, 0, 0, 0, 0);                /* null descriptor */
	gdt_set_entry(1, 0, 0xFFFFFFFF, 0x9A, 0xCF); /* 0x08: kernel code */
	gdt_set_entry(2, 0, 0xFFFFFFFF, 0x92, 0xCF); /* 0x10: kernel data */
	gdt_set_entry(3, 0, 0xFFFFFFFF, 0xFA, 0xCF); /* 0x18: user code (ring3, unused until P5) */
	gdt_set_entry(4, 0, 0xFFFFFFFF, 0xF2, 0xCF); /* 0x20: user data (ring3, unused until P5) */

	unsigned int tss_base = (unsigned int)(unsigned long)&tss;
	unsigned int tss_limit = tss_base + sizeof(struct tss_entry);
	gdt_set_entry(5, tss_base, tss_limit, 0x89, 0x40); /* 0x28: TSS */

	/* 0x30, 0x38, 0x40: TLS slots, Linux's GDT_ENTRY_TLS_MIN..MAX (6-8).
	 * Left empty here -- filled in by set_thread_area at runtime (see
	 * arch/syscall.c) the first time a process calls it. Existing
	 * unconditionally, whether or not anything ever uses them, exactly
	 * matches real Linux's layout and is what set_thread_area's
	 * "allocate slot 6/7/8" logic assumes is available. */
	for (int i = 6; i <= 8; i++)
		gdt_set_entry(i, 0, 0, 0, 0);

	for (unsigned int i = 0; i < sizeof(tss); i++)
		((unsigned char *)&tss)[i] = 0;
	tss.ss0 = 0x10;
	tss.esp0 = 0; /* set for real once a kernel stack per task exists */

	gdt_flush(&gp);
	tss_flush();

	kprintf("gdt: loaded (6 entries), tss loaded\n");
}

/* Must be called with a valid kernel stack before any ring3->ring0
 * transition can happen: the CPU loads esp/ss from here on privilege-
 * level change (interrupt or int 0x80 arriving from ring3). Left at 0
 * by gdt_init deliberately -- forgetting to call this before dropping
 * to ring3 should fault loudly (esp=0), not silently corrupt memory. */
void tss_set_kernel_stack(unsigned int esp0) {
	tss.esp0 = esp0;
}

/* Called by SYS_set_thread_area (arch/syscall.c). Installs a full
 * 4GB present+writable ring3-accessible data descriptor at `base` into
 * GDT slot `index` (6, 7, or 8) -- no GDTR reload needed, since the
 * GDT's own base/limit in memory don't change, only one entry's
 * contents do, and the CPU re-reads GDT entries fresh every time a
 * segment register is loaded against them. */
void gdt_set_tls_entry(int index, unsigned int base) {
	gdt_set_entry(index, base, 0xFFFFFFFF, 0xF2, 0xCF);
}
