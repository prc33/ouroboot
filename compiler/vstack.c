#define USING_GLOBALS
#include "tcc.h"
#include "vstack.h"

static SValue stack[1 + VSTACK_SIZE];
ST_DATA SValue *vtop;

#define MAX_TEMP_LOCAL_VARIABLE_NUMBER 8
static struct {
    int location;
    short size, align;
} temp_locals[MAX_TEMP_LOCAL_VARIABLE_NUMBER];
static int nb_temp_locals;

ST_FUNC void vstack_init(void)
{
    vtop = stack + 1;
    memset(vtop, 0, sizeof *vtop);
    --vtop;
}

ST_FUNC int vstack_depth(void)
{
    return vtop - (stack + 1) + 1;
}

ST_FUNC SValue *vstack_base(void)
{
    return stack + 1;
}

static void vstack_push(const SValue *v)
{
    if (vtop >= stack + VSTACK_SIZE)
        tcc_error("memory full (vstack)");
    *++vtop = *v;
}

ST_FUNC void vpushv(SValue *v)
{
    vstack_push(v);
}

ST_FUNC void vdup(void)
{
    vstack_push(vtop);
}

ST_FUNC void vstack_drop(void)
{
    --vtop;
}

ST_FUNC void vstack_push_typed(CType *type, int location, CValue *value)
{
    SValue entry;
    memset(&entry, 0, sizeof entry);
    entry.type = *type;
    entry.r = location;
    entry.r2 = VT_CONST;
    entry.c = *value;
    vstack_push(&entry);
}

ST_FUNC void vstack_swap(void)
{
    SValue tmp = vtop[0];
    vtop[0] = vtop[-1];
    vtop[-1] = tmp;
}

ST_FUNC void vstack_rotate_bottom(int n)
{
    int i;
    SValue tmp = vtop[-n + 1];
    for (i = -n + 1; i; ++i)
        vtop[i] = vtop[i + 1];
    vtop[0] = tmp;
}

ST_FUNC void vstack_rotate_top(SValue *e, int n)
{
    int i;
    SValue tmp = *e;
    for (i = 0; i < n - 1; ++i)
        e[-i] = e[-i - 1];
    e[-n + 1] = tmp;
}

ST_FUNC int vstack_temp_local(int size, int align, int *frame_loc,
                              int (*is_free)(int, void *), void *opaque)
{
    int i, location;

    for (i = 0; i < nb_temp_locals; ++i)
        if (temp_locals[i].size >= size && temp_locals[i].align == align
            && is_free(temp_locals[i].location, opaque))
            return temp_locals[i].location;

    location = *frame_loc = (*frame_loc - size) & -align;
    if (nb_temp_locals < MAX_TEMP_LOCAL_VARIABLE_NUMBER) {
        temp_locals[nb_temp_locals].location = location;
        temp_locals[nb_temp_locals].size = size;
        temp_locals[nb_temp_locals].align = align;
        ++nb_temp_locals;
    }
    return location;
}

ST_FUNC void vstack_clear_temp_locals(void)
{
    nb_temp_locals = 0;
}
