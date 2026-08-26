#!/bin/sh
# Converts a real riscv64 .s file (real mnemonics) into the .long-encoded
# form our own TCC can actually assemble -- riscv64 TCC has no integrated
# assembler for real instructions (only directives: .globl/.weak/.set/.long
# etc; see docs/riscv-port-findings.md), so every hand-written riscv64 .S
# file in this kernel is generated this way, not hand-encoded and not
# assembled at build time.
#
# Same technique, same tool, as demo/patches/musl-riscv64-tcc-compat.patch's
# tools/gen_asm_words.sh (used there for musl's setjmp/longjmp/etc) and as
# arch/gen_isr_stubs.py's role for i386's isr_stubs.S: a discovery tool run
# by hand, offline, whose *output* gets checked in and hand-edited never
# again. kernel/Makefile never invokes riscv64-linux-gnu-as -- only our own
# ../compiler/tcc, for either ARCH.
#
# Uses .long (4 bytes on every TCC target), not .word -- TCC's generic
# .word directive is 2 bytes (x86/GAS legacy). See docs/riscv-port-findings.md
# for the bug this caused the first time around.
#
# Usage: arch/gen_riscv64_asm.sh <in.s> > <out.S>
set -e
IN="$1"
riscv64-linux-gnu-as -march=rv64imafd_zicsr -o /tmp/_kgen.o "$IN"
echo "/* GENERATED from $IN by arch/gen_riscv64_asm.sh -- do not hand-edit. */"
riscv64-linux-gnu-nm /tmp/_kgen.o | awk '$2=="T"||$2=="t"{print $1, $3}' > /tmp/_kgen.syms
riscv64-linux-gnu-objdump -d /tmp/_kgen.o | awk -v symsfile=/tmp/_kgen.syms '
BEGIN {
  while ((getline line < symsfile) > 0) {
    split(line, f, " ")
    addr = f[1]; name = f[2]
    syms[addr] = syms[addr] " " name
  }
}
/^[0-9a-f]+ </ {
  addr = $1
  n = split(syms[addr], names, " ")
  for (i = 1; i <= n; i++) {
    print ".globl " names[i]
    print names[i] ":"
  }
  next
}
/^ +[0-9a-f]+:/ {
  ins=$2; $1=""; $2="";
  gsub(/^[ \t]+/,"");
  printf "\t.long 0x%s\t/* %s */\n", ins, $0
}'
