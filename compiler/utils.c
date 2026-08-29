#include "common.h"
#include "utils.h"

extern void _tcc_error(const char *fmt, ...) NORETURN PRINTF_LIKE(1,2);

#undef free
#undef malloc
#undef realloc

PUB_FUNC void tcc_free(void *ptr)
{
    free(ptr);
}

PUB_FUNC void *tcc_malloc(unsigned long size)
{
    void *ptr = malloc(size);
    if (!ptr && size)
        _tcc_error("memory full (malloc)");
    return ptr;
}

PUB_FUNC void *tcc_mallocz(unsigned long size)
{
    void *ptr = tcc_malloc(size);
    memset(ptr, 0, size);
    return ptr;
}

PUB_FUNC void *tcc_realloc(void *ptr, unsigned long size)
{
    void *ptr1 = realloc(ptr, size);
    if (!ptr1 && size)
        _tcc_error("memory full (realloc)");
    return ptr1;
}

#define free(p) use_tcc_free(p)
#define malloc(s) use_tcc_malloc(s)
#define realloc(p, s) use_tcc_realloc(p, s)

ST_FUNC void dynarray_add(void *ptab, int *nb_ptr, void *data)
{
    int nb = *nb_ptr, nb_alloc;
    void **pp = *(void ***)ptab;

    if ((nb & (nb - 1)) == 0) {
        nb_alloc = nb ? nb * 2 : 1;
        pp = tcc_realloc(pp, nb_alloc * sizeof(void *));
        *(void ***)ptab = pp;
    }
    pp[nb++] = data;
    *nb_ptr = nb;
}

ST_FUNC void dynarray_reset(void *pp, int *n)
{
    void **p;
    for (p = *(void ***)pp; *n; ++p, --*n)
        if (*p)
            tcc_free(*p);
    tcc_free(*(void **)pp);
    *(void **)pp = NULL;
}

ST_FUNC void strcat_vprintf(char *buf, int buf_size, const char *fmt, va_list ap)
{
    int len = strlen(buf);
    vsnprintf(buf + len, buf_size - len, fmt, ap);
}

ST_FUNC void strcat_printf(char *buf, int buf_size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    strcat_vprintf(buf, buf_size, fmt, ap);
    va_end(ap);
}

ST_FUNC int exact_log2p1(int value)
{
    int result;

    if (!value)
        return 0;
    for (result = 1; value >= 1 << 8; result += 8)
        value >>= 8;
    if (value >= 1 << 4)
        result += 4, value >>= 4;
    if (value >= 1 << 2)
        result += 2, value >>= 2;
    if (value >= 1 << 1)
        ++result;
    return result;
}

ST_FUNC char *pstrcpy(char *buf, size_t buf_size, const char *s)
{
    char *q, *q_end;
    int c;

    if (buf_size > 0) {
        q = buf;
        q_end = buf + buf_size - 1;
        while (q < q_end) {
            c = *s++;
            if (c == '\0')
                break;
            *q++ = c;
        }
        *q = '\0';
    }
    return buf;
}

ST_FUNC char *pstrcat(char *buf, size_t buf_size, const char *s)
{
    size_t len = strlen(buf);
    if (len < buf_size)
        pstrcpy(buf + len, buf_size - len, s);
    return buf;
}

ST_FUNC char *pstrncpy(char *out, const char *in, size_t num)
{
    memcpy(out, in, num);
    out[num] = '\0';
    return out;
}

PUB_FUNC char *tcc_basename(const char *name)
{
    char *p = strchr(name, 0);
    while (p > name && !IS_DIRSEP(p[-1]))
        --p;
    return p;
}

PUB_FUNC char *tcc_fileextension(const char *name)
{
    char *b = tcc_basename(name);
    char *e = strrchr(b, '.');
    return e ? e : strchr(b, 0);
}

PUB_FUNC char *tcc_strdup(const char *str)
{
    char *ptr = tcc_malloc(strlen(str) + 1);
    strcpy(ptr, str);
    return ptr;
}

ST_FUNC void cstr_realloc(CString *cstr, int new_size)
{
    int size = cstr->size_allocated;
    if (size < 8)
        size = 8;
    while (size < new_size)
        size *= 2;
    cstr->data = tcc_realloc(cstr->data, size);
    cstr->size_allocated = size;
}

ST_FUNC void cstr_ccat(CString *cstr, int ch)
{
    int size = cstr->size + 1;
    if (size > cstr->size_allocated)
        cstr_realloc(cstr, size);
    ((unsigned char *)cstr->data)[size - 1] = ch;
    cstr->size = size;
}

ST_FUNC void cstr_cat(CString *cstr, const char *str, int len)
{
    int size;
    if (len <= 0)
        len = strlen(str) + 1 + len;
    size = cstr->size + len;
    if (size > cstr->size_allocated)
        cstr_realloc(cstr, size);
    memmove((unsigned char *)cstr->data + cstr->size, str, len);
    cstr->size = size;
}

ST_FUNC void cstr_wccat(CString *cstr, int ch)
{
    int size = cstr->size + sizeof(nwchar_t);
    if (size > cstr->size_allocated)
        cstr_realloc(cstr, size);
    *(nwchar_t *)((unsigned char *)cstr->data + size - sizeof(nwchar_t)) = ch;
    cstr->size = size;
}

ST_FUNC void cstr_new(CString *cstr)
{
    memset(cstr, 0, sizeof(*cstr));
}

ST_FUNC void cstr_free(CString *cstr)
{
    tcc_free(cstr->data);
    cstr_new(cstr);
}

ST_FUNC void cstr_reset(CString *cstr)
{
    cstr->size = 0;
}

ST_FUNC int cstr_printf(CString *cstr, const char *fmt, ...)
{
    va_list v;
    int len, size;

    va_start(v, fmt);
    len = vsnprintf(NULL, 0, fmt, v);
    va_end(v);
    size = cstr->size + len + 1;
    if (size > cstr->size_allocated)
        cstr_realloc(cstr, size);
    va_start(v, fmt);
    vsnprintf((char *)cstr->data + cstr->size, size, fmt, v);
    va_end(v);
    cstr->size += len;
    return len;
}
