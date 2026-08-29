#ifndef TCC_VSTACK_H
#define TCC_VSTACK_H

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
#endif
