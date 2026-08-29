#ifndef TCC_UTILS_H
#define TCC_UTILS_H

typedef struct CString {
    int size;
    void *data;
    int size_allocated;
} CString;

PUB_FUNC void tcc_free(void *ptr);
PUB_FUNC void *tcc_malloc(unsigned long size);
PUB_FUNC void *tcc_mallocz(unsigned long size);
PUB_FUNC void *tcc_realloc(void *ptr, unsigned long size);
ST_FUNC void dynarray_add(void *ptab, int *nb_ptr, void *data);
ST_FUNC void dynarray_reset(void *pp, int *n);
ST_FUNC void strcat_vprintf(char *buf, int buf_size, const char *fmt, va_list ap);
ST_FUNC void strcat_printf(char *buf, int buf_size, const char *fmt, ...) PRINTF_LIKE(3,4);
ST_FUNC int exact_log2p1(int value);

ST_FUNC char *pstrcpy(char *buf, size_t buf_size, const char *s);
ST_FUNC char *pstrcat(char *buf, size_t buf_size, const char *s);
ST_FUNC char *pstrncpy(char *out, const char *in, size_t num);
PUB_FUNC char *tcc_basename(const char *name);
PUB_FUNC char *tcc_fileextension(const char *name);
PUB_FUNC char *tcc_strdup(const char *str);

ST_FUNC void cstr_realloc(CString *cstr, int new_size);
ST_FUNC void cstr_ccat(CString *cstr, int ch);
ST_FUNC void cstr_cat(CString *cstr, const char *str, int len);
ST_FUNC void cstr_wccat(CString *cstr, int ch);
ST_FUNC void cstr_new(CString *cstr);
ST_FUNC void cstr_free(CString *cstr);
ST_FUNC void cstr_reset(CString *cstr);
ST_FUNC int cstr_printf(CString *cstr, const char *fmt, ...) PRINTF_LIKE(2,3);

#endif
