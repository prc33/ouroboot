#ifndef TCC_VSTACK_H
#define TCC_VSTACK_H

#include "types.h"
#include "target.h"
#include "parsing.h"

#define VSTACK_SIZE 256

#define VT_VALMASK   0x003f
#define VT_CONST     0x0030
#define VT_LLOCAL    0x0031
#define VT_LOCAL     0x0032
#define VT_CMP       0x0033
#define VT_JMP       0x0034
#define VT_JMPI      0x0035
#define VT_LVAL      0x0100
#define VT_SYM       0x0200
#define VT_MUSTCAST  0x0C00

typedef struct SValue {
    CType type;
    unsigned short r;
    unsigned short r2;
    union {
        struct { int jtrue, jfalse; };
        CValue c;
    };
    union {
        struct { unsigned short cmp_op, cmp_r; };
        struct Sym *sym;
    };
} SValue;

ST_FUNC void vstack_init(void);
ST_FUNC int vstack_depth(void);
ST_FUNC SValue *vstack_base(void);
ST_FUNC void vstack_drop(void);
ST_FUNC void vstack_push_typed(CType *, int, CValue *);
ST_FUNC void vstack_swap(void);
ST_FUNC void vstack_rotate_bottom(int n);
ST_FUNC void vstack_rotate_top(SValue *e, int n);
ST_FUNC int vstack_temp_local(int, int, int *);
ST_FUNC void vstack_clear_temp_locals(void);
ST_FUNC void vpushv(SValue *v);
ST_FUNC void vdup(void);
extern SValue *vtop;
#endif
