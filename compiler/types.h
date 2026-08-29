#ifndef TCC_TYPES_H
#define TCC_TYPES_H

/* CType.t: basic type, qualifiers, storage, and bitfield metadata. */
#define VT_BTYPE       0x000f
#define VT_VOID             0
#define VT_BYTE             1
#define VT_SHORT            2
#define VT_INT              3
#define VT_LLONG            4
#define VT_PTR              5
#define VT_FUNC             6
#define VT_STRUCT           7
#define VT_FLOAT            8
#define VT_DOUBLE           9
#define VT_LDOUBLE         10
#define VT_BOOL            11
#define VT_QLONG           13
#define VT_QFLOAT          14

#define VT_UNSIGNED    0x0010
#define VT_DEFSIGN     0x0020
#define VT_ARRAY       0x0040
#define VT_BITFIELD    0x0080
#define VT_CONSTANT    0x0100
#define VT_VOLATILE    0x0200
#define VT_VLA         0x0400
#define VT_LONG        0x0800

#define VT_EXTERN  0x00001000
#define VT_STATIC  0x00002000
#define VT_TYPEDEF 0x00004000
#define VT_INLINE  0x00008000

#define VT_STRUCT_SHIFT 20
#define VT_STRUCT_MASK (((1U << 12) - 1) << VT_STRUCT_SHIFT | VT_BITFIELD)
#define BIT_POS(t) (((t) >> VT_STRUCT_SHIFT) & 0x3f)
#define BIT_SIZE(t) (((t) >> (VT_STRUCT_SHIFT + 6)) & 0x3f)

#define VT_UNION    (1 << VT_STRUCT_SHIFT | VT_STRUCT)
#define VT_ENUM     (2 << VT_STRUCT_SHIFT)
#define VT_ENUM_VAL (3 << VT_STRUCT_SHIFT)

#define IS_ENUM(t) ((t & VT_STRUCT_MASK) == VT_ENUM)
#define IS_ENUM_VAL(t) ((t & VT_STRUCT_MASK) == VT_ENUM_VAL)
#define IS_UNION(t) ((t & (VT_STRUCT_MASK | VT_BTYPE)) == VT_UNION)
#define VT_STORAGE (VT_EXTERN | VT_STATIC | VT_TYPEDEF | VT_INLINE)
#define VT_TYPE (~(VT_STORAGE | VT_STRUCT_MASK))

/* Symbol created by the assembler before C parsing sees it. */
#define VT_ASM (VT_VOID | VT_UNSIGNED)
#define IS_ASM_SYM(sym) (((sym)->type.t & (VT_BTYPE | VT_ASM)) == VT_ASM)

#define BFVAL(M, N) ((unsigned)((M) & ~((M) << 1)) * (N))
#define BFGET(X, M) (((X) & (M)) / BFVAL(M, 1))
#define BFSET(X, M, N) ((X) = ((X) & ~(M)) | BFVAL(M, N))

ST_INLN int is_float(int t);
ST_FUNC int is_integer_btype(int bt);
ST_FUNC int btype_size(int bt);
ST_FUNC int type_size(CType *type, int *align);
ST_INLN CType *pointed_type(CType *type);
ST_FUNC void mk_pointer(CType *type);
ST_FUNC int compare_types(CType *type1, CType *type2, int unqualified);
ST_FUNC int is_compatible_types(CType *type1, CType *type2);
ST_FUNC int is_compatible_unqualified_types(CType *type1, CType *type2);
ST_FUNC int combine_types(CType *dest, SValue *left, SValue *right, int op);
ST_FUNC int is_null_pointer(SValue *value);
ST_FUNC void type_to_str(char *buf, int size, CType *type, const char *name);
ST_FUNC void type_incompatibility_error(CType *source, CType *dest,
                                        const char *message);
ST_FUNC void type_incompatibility_warning(CType *source, CType *dest,
                                          const char *message);
#endif
