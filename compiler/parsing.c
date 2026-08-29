#include "tcc.h"

TokenString *macro_stack;

ST_INLN void tok_str_new(TokenString *s)
{
    s->str = NULL;
    s->len = s->lastlen = 0;
    s->allocated_len = 0;
    s->last_line_num = -1;
}

ST_FUNC TokenString *tok_str_alloc(void)
{
    TokenString *str = tcc_malloc(sizeof *str);
    tok_str_new(str);
    return str;
}

ST_FUNC int *tok_str_dup(TokenString *s)
{
    int *str = tcc_malloc(s->len * sizeof(int));
    memcpy(str, s->str, s->len * sizeof(int));
    return str;
}

ST_FUNC void tok_str_free_str(int *str)
{
    tcc_free(str);
}

ST_FUNC void tok_str_free(TokenString *str)
{
    tok_str_free_str(str->str);
    tcc_free(str);
}

ST_FUNC int *tok_str_realloc(TokenString *s, int new_size)
{
    int size = s->allocated_len;
    if (size < 16)
        size = 16;
    while (size < new_size)
        size *= 2;
    if (size > s->allocated_len) {
        s->str = tcc_realloc(s->str, size * sizeof(int));
        s->allocated_len = size;
    }
    return s->str;
}

ST_FUNC void tok_str_add(TokenString *s, int t)
{
    if (s->len >= s->allocated_len)
        tok_str_realloc(s, s->len + 1);
    s->str[s->len++] = t;
}

ST_FUNC void begin_macro(TokenString *str, int alloc)
{
    str->alloc = alloc;
    str->prev = macro_stack;
    str->prev_ptr = macro_ptr;
    str->save_line_num = file->line_num;
    macro_ptr = str->str;
    macro_stack = str;
}

ST_FUNC void end_macro(void)
{
    TokenString *str = macro_stack;
    macro_stack = str->prev;
    macro_ptr = str->prev_ptr;
    file->line_num = str->save_line_num;
    if (str->alloc) {
        if (str->alloc == 2)
            str->str = NULL;
        tok_str_free(str);
    }
}

ST_FUNC void tok_str_add2(TokenString *s, int t, CValue *cv)
{
    int len = s->lastlen = s->len;
    int *str = s->str;
    if (len + TOK_MAX_SIZE >= s->allocated_len)
        str = tok_str_realloc(s, len + TOK_MAX_SIZE + 1);
    str[len++] = t;
    switch (t) {
    case TOK_CINT: case TOK_CUINT: case TOK_CCHAR: case TOK_LCHAR:
    case TOK_CFLOAT: case TOK_LINENUM:
#if LONG_SIZE == 4
    case TOK_CLONG: case TOK_CULONG:
#endif
        str[len++] = cv->tab[0]; break;
    case TOK_PPNUM: case TOK_PPSTR: case TOK_STR: case TOK_LSTR: {
        size_t words = 1 + (cv->str.size + sizeof(int) - 1) / sizeof(int);
        if (len + words >= s->allocated_len)
            str = tok_str_realloc(s, len + words + 1);
        str[len] = cv->str.size;
        memcpy(&str[len + 1], cv->str.data, cv->str.size);
        len += words;
        break;
    }
    case TOK_CDOUBLE: case TOK_CLLONG: case TOK_CULLONG:
#if LONG_SIZE == 8
    case TOK_CLONG: case TOK_CULONG:
#endif
#if LDOUBLE_SIZE == 8
    case TOK_CLDOUBLE:
#endif
        str[len++] = cv->tab[0]; str[len++] = cv->tab[1]; break;
#if LDOUBLE_SIZE == 12
    case TOK_CLDOUBLE:
        str[len++] = cv->tab[0]; str[len++] = cv->tab[1]; str[len++] = cv->tab[2]; break;
#elif LDOUBLE_SIZE == 16
    case TOK_CLDOUBLE:
        str[len++] = cv->tab[0]; str[len++] = cv->tab[1];
        str[len++] = cv->tab[2]; str[len++] = cv->tab[3]; break;
#endif
    }
    s->len = len;
}

ST_FUNC void tok_str_add_tok(TokenString *s)
{
    CValue value;
    if (file->line_num != s->last_line_num) {
        s->last_line_num = file->line_num;
        value.i = s->last_line_num;
        tok_str_add2(s, TOK_LINENUM, &value);
    }
    tok_str_add2(s, tok, &tokc);
}
