/*
 * compat.c - Linux: the Win32 calls compat.h declares. Threads on pthreads,
 * and themepark.ini read and written as Windows' profile functions do it:
 * [section] and key=value lines, a missing file or key gives the default.
 */
#ifndef _WIN32
#include <errno.h>
#include <string.h>
#include "compat.h"

DWORD GetModuleFileNameA(void* module, char* out, DWORD n) {
    (void)module;
    ssize_t k = readlink("/proc/self/exe", out, n - 1);
    if (k < 0) k = 0;
    out[k] = 0;
    return (DWORD)k;
}

struct thread_s { pthread_t t; DWORD (*fn)(LPVOID); LPVOID arg; volatile int done; int joined, closed; };

static void* thread_main(void* p) {
    struct thread_s* h = p;
    h->fn(h->arg);
    h->done = 1;
    return NULL;
}

HANDLE CreateThread(void* sec, size_t stack, DWORD (*fn)(LPVOID), LPVOID arg, DWORD flags, DWORD* id) {
    (void)sec; (void)flags; (void)id;
    struct thread_s* h = calloc(1, sizeof *h);
    pthread_attr_t a;
    pthread_attr_init(&a);
    if (stack) pthread_attr_setstacksize(&a, stack);
    h->fn = fn; h->arg = arg;
    if (pthread_create(&h->t, &a, thread_main, h)) { free(h); h = NULL; }
    pthread_attr_destroy(&a);
    return h;
}

DWORD WaitForSingleObject(HANDLE h, DWORD ms) {
    for (DWORD waited = 0; !h->done; waited += 5) {
        if (ms != INFINITE && waited >= ms) return 0x102;   /* WAIT_TIMEOUT */
        usleep(5000);
    }
    if (!h->joined) { pthread_join(h->t, NULL); h->joined = 1; }
    return 0;
}

/* ponytail: a thread closed before it ends is detached and its handle leaks. */
int CloseHandle(HANDLE h) {
    if (!h) return 0;
    if (h->joined) free(h);
    else pthread_detach(h->t);
    return 1;
}

/* ---- themepark.ini ------------------------------------------------------- */
static char* read_all(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* s = malloc((size_t)n + 1);
    s[fread(s, 1, (size_t)n, f)] = 0;
    fclose(f);
    return s;
}

/* The value of key in [sec], as a pointer to its first character and a length. */
static int find(const char* s, const char* sec, const char* key, const char** v, size_t* vn,
                const char** sec_end) {
    int in = 0;
    *sec_end = NULL;
    for (const char* l = s; l && *l; ) {
        const char* e = strchr(l, '\n');
        size_t n = e ? (size_t)(e - l) : strlen(l);
        while (n && (l[n - 1] == '\r' || l[n - 1] == ' ')) n--;
        if (l[0] == '[') {
            if (in) return 0;
            in = n > 2 && strlen(sec) == n - 2 && !strncasecmp(l + 1, sec, n - 2);
            if (in) *sec_end = e ? e + 1 : l + strlen(l);
        } else if (in) {
            const char* eq = memchr(l, '=', n);
            size_t kn = strlen(key);
            *sec_end = e ? e + 1 : l + strlen(l);
            if (eq && (size_t)(eq - l) == kn && !strncasecmp(l, key, kn)) { *v = eq + 1; *vn = n - kn - 1; return 1; }
        }
        l = e ? e + 1 : NULL;
    }
    return 0;
}

DWORD GetPrivateProfileStringA(const char* sec, const char* key, const char* def, char* out, DWORD n,
                               const char* path) {
    char* s = read_all(path);
    const char *v, *end;
    size_t vn;
    if (s && find(s, sec, key, &v, &vn, &end)) snprintf(out, n, "%.*s", (int)vn, v);
    else snprintf(out, n, "%s", def ? def : "");
    free(s);
    return (DWORD)strlen(out);
}

unsigned GetPrivateProfileIntA(const char* sec, const char* key, int def, const char* path) {
    char v[32];
    GetPrivateProfileStringA(sec, key, "", v, sizeof v, path);
    return v[0] ? (unsigned)atoi(v) : (unsigned)def;
}

int WritePrivateProfileStringA(const char* sec, const char* key, const char* val, const char* path) {
    char* s = read_all(path);
    const char *v, *end;
    size_t vn;
    if (!s) s = calloc(1, 1);
    FILE* f = fopen(path, "wb");
    if (!f) { free(s); return 0; }
    if (find(s, sec, key, &v, &vn, &end))              /* replace the value */
        fprintf(f, "%.*s%s%s", (int)(v - s), s, val, v + vn);
    else if (end)                                         /* a new key at the end of its section */
        fprintf(f, "%.*s%s=%s\n%s", (int)(end - s), s, key, val, end);
    else                                                  /* a new section */
        fprintf(f, "%s%s[%s]\n%s=%s\n", s, s[0] && s[strlen(s) - 1] != '\n' ? "\n" : "", sec, key, val);
    fclose(f);
    free(s);
    return 1;
}

#ifdef COMPAT_TEST
/* gcc -DCOMPAT_TEST compat.c && ./a.out: the ini round trip. */
#include <assert.h>
int main(void) {
    const char* p = "/tmp/compat_test.ini";
    char v[64];
    remove(p);
    assert(GetPrivateProfileIntA("display", "window", 7, p) == 7);
    WritePrivateProfileStringA("display", "scale", "integer", p);
    WritePrivateProfileStringA("game", "fps", "25", p);
    WritePrivateProfileStringA("display", "vivid", "1", p);
    WritePrivateProfileStringA("display", "scale", "crt", p);
    GetPrivateProfileStringA("display", "scale", "", v, sizeof v, p);
    assert(!strcmp(v, "crt"));
    assert(GetPrivateProfileIntA("display", "vivid", 0, p) == 1);
    assert(GetPrivateProfileIntA("game", "fps", 0, p) == 25);
    assert(GetPrivateProfileIntA("game", "vivid", 5, p) == 5);
    puts("compat: ok");
    return 0;
}
#endif
#endif
