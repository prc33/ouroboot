#ifndef TCC_EXPR_H
#define TCC_EXPR_H
ST_FUNC void unary(void);
ST_FUNC void gexpr(void);
ST_FUNC int expr_const(void);
ST_FUNC void expr_eq(void);
ST_FUNC void expr_const1(void);
ST_FUNC int64_t expr_const64(void);
ST_FUNC void init_prec(void);
ST_FUNC int gvtst(int inv, int t);
ST_FUNC void gvtst_set(int inv, int t);

ST_DATA int in_sizeof;
ST_DATA int in_generic;

ST_FUNC void vsetc(CType *type, int r, CValue *value);
ST_FUNC void vseti(int r, int value);
ST_FUNC void vpush(CType *type);
ST_FUNC void vpushsym(CType *type, Sym *sym);
ST_FUNC void gen_test_zero(int op);
ST_FUNC void gen_cast_s(int type);
ST_FUNC int parse_btype(CType *type, AttributeDef *ad);
ST_FUNC CType *type_decl(CType *type, AttributeDef *ad, int *v, int td);
ST_FUNC void expr_type(CType *type, void (*expr_fn)(void));
ST_FUNC void parse_builtin_params(int nocode, const char *args);
ST_FUNC void decl_initializer_alloc(CType *type, AttributeDef *ad, int r,
                                    int has_init, int v, int scope);
ST_FUNC void block(int is_expr);
ST_FUNC void vla_runtime_type_size(CType *type, int *align);
ST_FUNC Sym *find_field(CType *type, int v, int *offset);
ST_FUNC void gfunc_param_typed(Sym *func, Sym *arg);
ST_FUNC void skip_or_save_block(TokenString **str);
#endif
