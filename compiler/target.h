#ifndef TCC_TARGET_H
#define TCC_TARGET_H

#define TARGET_DEFS_ONLY
#ifdef TCC_TARGET_I386
# include "i386/i386-gen.c"
# include "i386/i386-link.c"
#elif defined(TCC_TARGET_RISCV64)
# include "risc/riscv64-gen.c"
# include "risc/riscv64-link.c"
# include "risc/riscv64-asm.c"
#elif defined(TCC_TARGET_WASM32)
# include "wasm/wasm-gen.c"
# include "wasm/wasm-link.c"
#else
# error unknown target
#endif
#undef TARGET_DEFS_ONLY

#endif
