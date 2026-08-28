#ifndef TCC_TYPES_H
#define TCC_TYPES_H
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
