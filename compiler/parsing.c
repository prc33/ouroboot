#include "parsing.h"
#include "symbols.h"

extern void _tcc_error(const char *fmt, ...) NORETURN PRINTF_LIKE(1,2);

TokenString *macro_stack;
static TokenSym *hash_ident[TOK_HASH_SIZE];
static CString tok_text;

/* This table maps punctuation tokens back to their source spelling. */
static const unsigned char tok_two_chars[] = {
    '<','=', TOK_LE, '>','=', TOK_GE, '!','=', TOK_NE,
    '&','&', TOK_LAND, '|','|', TOK_LOR, '+','+', TOK_INC,
    '-','-', TOK_DEC, '=','=', TOK_EQ, '<','<', TOK_SHL,
    '>','>', TOK_SAR, '+','=', TOK_A_ADD, '-','=', TOK_A_SUB,
    '*','=', TOK_A_MUL, '/','=', TOK_A_DIV, '%','=', TOK_A_MOD,
    '&','=', TOK_A_AND, '^','=', TOK_A_XOR, '|','=', TOK_A_OR,
    '-','>', TOK_ARROW, '.','.', TOK_TWODOTS, '#','#', TOK_TWOSHARPS,
    0
};

ST_FUNC const char *get_tok_str(int v, CValue *cv)
{
    char *p;
    int i, len;

    cstr_reset(&tok_text);
    p = tok_text.data;
    switch (v) {
    case TOK_CINT: case TOK_CUINT: case TOK_CLONG: case TOK_CULONG:
    case TOK_CLLONG: case TOK_CULLONG:
        sprintf(p, "%llu", (unsigned long long)cv->i);
        break;
    case TOK_LCHAR:
        cstr_ccat(&tok_text, 'L');
    case TOK_CCHAR:
        cstr_ccat(&tok_text, '\'');
        add_char(&tok_text, cv->i);
        cstr_ccat(&tok_text, '\'');
        cstr_ccat(&tok_text, 0);
        break;
    case TOK_PPNUM: case TOK_PPSTR:
        return (char *)cv->str.data;
    case TOK_LSTR:
        cstr_ccat(&tok_text, 'L');
    case TOK_STR:
        cstr_ccat(&tok_text, '"');
        if (v == TOK_STR) {
            len = cv->str.size - 1;
            for (i = 0; i < len; ++i)
                add_char(&tok_text, ((unsigned char *)cv->str.data)[i]);
        } else {
            len = cv->str.size / sizeof(nwchar_t) - 1;
            for (i = 0; i < len; ++i)
                add_char(&tok_text, ((nwchar_t *)cv->str.data)[i]);
        }
        cstr_ccat(&tok_text, '"');
        cstr_ccat(&tok_text, 0);
        break;
    case TOK_CFLOAT: cstr_cat(&tok_text, "<float>", 0); break;
    case TOK_CDOUBLE: cstr_cat(&tok_text, "<double>", 0); break;
    case TOK_CLDOUBLE: cstr_cat(&tok_text, "<long double>", 0); break;
    case TOK_LINENUM: cstr_cat(&tok_text, "<linenumber>", 0); break;
    case TOK_LT: v = '<'; goto single;
    case TOK_GT: v = '>'; goto single;
    case TOK_DOTS: return strcpy(p, "...");
    case TOK_A_SHL: return strcpy(p, "<<=");
    case TOK_A_SAR: return strcpy(p, ">>=");
    case TOK_EOF: return strcpy(p, "<eof>");
    default:
        if (v < TOK_IDENT) {
            const unsigned char *q = tok_two_chars;
            while (*q) {
                if (q[2] == v) {
                    *p++ = q[0]; *p++ = q[1]; *p = 0;
                    return tok_text.data;
                }
                q += 3;
            }
            if (v >= 127) {
                sprintf(tok_text.data, "<%02x>", v);
                return tok_text.data;
            }
single:
            *p++ = v; *p = 0;
        } else if (v < tok_ident) {
            return table_ident[v - TOK_IDENT]->str;
        } else if (v >= SYM_FIRST_ANOM) {
            sprintf(p, "L.%u", v - SYM_FIRST_ANOM);
        } else {
            return NULL;
        }
    }
    return tok_text.data;
}

static TokenSym *tok_alloc_new(TokenSym **slot, const char *str, int len)
{
    TokenSym *ts;
    int i = tok_ident - TOK_IDENT;
    if (tok_ident >= SYM_FIRST_ANOM)
        _tcc_error("memory full (symbols)");
    if (!(i % TOK_ALLOC_INCR))
        table_ident = tcc_realloc(table_ident,
            (i + TOK_ALLOC_INCR) * sizeof(*table_ident));
    ts = tcc_malloc(sizeof(*ts) + len);
    memset(ts, 0, sizeof(*ts));
    table_ident[i] = ts;
    ts->tok = tok_ident++;
    ts->len = len;
    memcpy(ts->str, str, len);
    ts->str[len] = 0;
    *slot = ts;
    return ts;
}

ST_FUNC TokenSym *tok_alloc(const char *str, int len)
{
    TokenSym *ts, **slot;
    unsigned h = TOK_HASH_INIT;
    int i;
    for (i = 0; i < len; ++i)
        h = TOK_HASH_FUNC(h, (unsigned char)str[i]);
    slot = &hash_ident[h & (TOK_HASH_SIZE - 1)];
    while ((ts = *slot)) {
        if (ts->len == len && !memcmp(ts->str, str, len))
            return ts;
        slot = &ts->hash_next;
    }
    return tok_alloc_new(slot, str, len);
}

ST_FUNC void token_syms_init(void)
{
    memset(hash_ident, 0, sizeof(hash_ident));
    tok_ident = TOK_IDENT;
    cstr_new(&tok_text);
    cstr_realloc(&tok_text, STRING_MAX_SIZE);
}

ST_FUNC int token_syms_free(void)
{
    int i, n = tok_ident - TOK_IDENT;
    for (i = 0; i < n; ++i)
        tcc_free(table_ident[i]);
    tcc_free(table_ident);
    table_ident = NULL;
    cstr_free(&tok_text);
    return n;
}

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
