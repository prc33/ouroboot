#ifndef TCC_UTILS_H
#define TCC_UTILS_H

typedef struct CString {
    int size;
    void *data;
    int size_allocated;
} CString;

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
