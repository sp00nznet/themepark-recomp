/*
 * compat.h - the Win32 the shared host files use (game.c, main.c, sound.c,
 * audio.c), so each is one file on both systems. On Windows it is windows.h;
 * on Linux, threads, locks, the clock and themepark.ini (compat.c) under the
 * same names. The window, sound device and pad differ more and have their own
 * Linux sides (present_sdl.c, audio.c, pad.c).
 */
#pragma once
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_PATH 4096
#define WINAPI
#define INFINITE 0xFFFFFFFFu
#define INVALID_FILE_ATTRIBUTES 0xFFFFFFFFu
#define STACK_SIZE_PARAM_IS_A_RESERVATION 0
typedef uint32_t DWORD;
typedef void* LPVOID;
typedef struct { int64_t QuadPart; } LARGE_INTEGER;
typedef struct thread_s* HANDLE;
typedef pthread_mutex_t CRITICAL_SECTION;

#define _stricmp strcasecmp
#define _popen popen
#define _pclose pclose
#define Sleep(ms) usleep((useconds_t)(ms) * 1000u)
#define EnterCriticalSection pthread_mutex_lock
#define LeaveCriticalSection pthread_mutex_unlock

static inline void InitializeCriticalSection(CRITICAL_SECTION* m) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m, &a);
}
static inline int QueryPerformanceFrequency(LARGE_INTEGER* f) { f->QuadPart = 1000000000; return 1; }
static inline int QueryPerformanceCounter(LARGE_INTEGER* t) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    t->QuadPart = (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
    return 1;
}
static inline DWORD GetTickCount(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (DWORD)(t.QuadPart / 1000000);
}
static inline DWORD GetFileAttributesA(const char* p) {
    struct stat st;
    return stat(p, &st) ? INVALID_FILE_ATTRIBUTES : S_ISDIR(st.st_mode) ? 0x10 : 0x80;
}
static inline int CreateDirectoryA(const char* p, void* sec) { (void)sec; return mkdir(p, 0777) == 0; }

/* compat.c */
DWORD  GetModuleFileNameA(void* module, char* out, DWORD n);
HANDLE CreateThread(void* sec, size_t stack, DWORD (*fn)(LPVOID), LPVOID arg, DWORD flags, DWORD* id);
DWORD  WaitForSingleObject(HANDLE h, DWORD ms);   /* a thread: 0 when it has ended, else 0x102 */
int    CloseHandle(HANDLE h);
DWORD  GetPrivateProfileStringA(const char* sec, const char* key, const char* def, char* out, DWORD n,
                                const char* path);
unsigned GetPrivateProfileIntA(const char* sec, const char* key, int def, const char* path);
int    WritePrivateProfileStringA(const char* sec, const char* key, const char* val, const char* path);
#endif

/* The folder the program is in, ending in a separator (either works on Windows). */
static inline void exe_dir(char* out, size_t n) {
    GetModuleFileNameA(NULL, out, (DWORD)n);
    char* s = out + strlen(out);
    while (s > out && s[-1] != '/' && s[-1] != '\\') s--;
    *s = 0;
}
