#!/usr/bin/env python3
"""
Generates arch/i386/isr_stubs.S: 32 CPU exception entry points (0-31) plus
16 IRQ entry points (32-47).

Why generated rather than a .macro/.rept loop in the .S file: TCC's
assembler doesn't support GNU-as .macro at all (confirmed absent from
its directive table), and earlier hard-won lessons from the musl port
(see docs/tcc-spike-findings.md) say not to lean on TCC's assembler
expression evaluator for anything non-trivial. Writing out 48 explicit,
boring stubs and checking in the *result* avoids both problems --
what TCC actually compiles is plain, unambiguous assembly, not a loop
whose correctness depends on assembler features we haven't verified.

Re-run this and commit the output if the stub count or numbering ever
needs to change:  python3 arch/gen_isr_stubs.py > arch/i386/isr_stubs.S
"""

# Exceptions where the CPU itself pushes an error code (i386 SDM vol 3,
# 6.15): everything else needs a dummy 0 pushed so the stack layout is
# identical either way.
HAS_ERROR_CODE = {8, 10, 11, 12, 13, 14, 17}

NUM_EXCEPTIONS = 32
NUM_IRQS = 16

print("/* GENERATED FILE -- see arch/gen_isr_stubs.py. Do not hand-edit. */")
print(".globl isr_common_stub")
print(".globl irq_common_stub")
print()

for n in range(NUM_EXCEPTIONS):
	print(f".globl isr{n}")
	print(f"isr{n}:")
	print("\tcli")
	if n not in HAS_ERROR_CODE:
		print("\tpush $0")
	print(f"\tpush ${n}")
	print("\tjmp isr_common_stub")
	print()

for irq in range(NUM_IRQS):
	vec = 32 + irq
	print(f".globl irq{irq}")
	print(f"irq{irq}:")
	print("\tcli")
	print("\tpush $0")
	print(f"\tpush ${vec}")
	print("\tjmp irq_common_stub")
	print()

print("""isr_common_stub:
\tpusha
\tmovw %gs, %ax
\tpush %eax
\tmovw %fs, %ax
\tpush %eax
\tmovw %es, %ax
\tpush %eax
\tmovw %ds, %ax
\tpush %eax
\tmovw $0x10, %ax
\tmovw %ax, %ds
\tmovw %ax, %es
\tmovw %ax, %fs
\tmovw %ax, %gs
\tpush %esp
\tcall isr_handler
\tadd $4, %esp
\tpop %eax
\tmovw %ax, %ds
\tpop %eax
\tmovw %ax, %es
\tpop %eax
\tmovw %ax, %fs
\tpop %eax
\tmovw %ax, %gs
\tpopa
\tadd $8, %esp
\tsti
\tiret

irq_common_stub:
\tpusha
\tmovw %gs, %ax
\tpush %eax
\tmovw %fs, %ax
\tpush %eax
\tmovw %es, %ax
\tpush %eax
\tmovw %ds, %ax
\tpush %eax
\tmovw $0x10, %ax
\tmovw %ax, %ds
\tmovw %ax, %es
\tmovw %ax, %fs
\tmovw %ax, %gs
\tpush %esp
\tcall irq_handler
\tadd $4, %esp
\tpop %eax
\tmovw %ax, %ds
\tpop %eax
\tmovw %ax, %es
\tpop %eax
\tmovw %ax, %fs
\tpop %eax
\tmovw %ax, %gs
\tpopa
\tadd $8, %esp
\tsti
\tiret""")
