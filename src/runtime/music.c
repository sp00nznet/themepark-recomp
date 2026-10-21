/*
 * music.c - music packs: the game's songs replaced by recordings.
 *
 * A pack is a folder, music\NAME\ beside mods\, of files named after the
 * game's songs: RIDE1.ogg ... RIDE17.ogg, HRTBEAT, FANFARE, TOVER, WIN2,
 * FUNERAL, and the intro's INTRO ... INTRO5 (music/README.md). Ogg Vorbis or
 * WAV (16 or 8-bit PCM), any rate, mono or stereo. A song the pack lacks
 * plays as the game's own MIDI.
 *
 * The game hands SOS a song's HMP data and nothing else, so the song is named
 * by finding those bytes in the game's banks (DATA\MUSIC<bank>-<variant>.DAT,
 * indexed by the .TAB beside it: 32-byte entries, the name at +0, the
 * offset at +0x12, the length at +0x1A).
 */
#include "compat.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#ifndef _WIN32
#include <dirent.h>
#endif

#include "music.h"

#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_HEADER_ONLY
#include "../third_party/stb_vorbis.c"

static char packs_dir[MAX_PATH], data_dir[MAX_PATH];
static char pack[64];

void music_set_dirs(const char* packs, const char* data) {
    snprintf(packs_dir, sizeof packs_dir, "%s", packs);
    snprintf(data_dir, sizeof data_dir, "%s", data);
}

void music_set_pack(const char* name) { snprintf(pack, sizeof pack, "%s", name ? name : ""); }
const char* music_pack(void) { return pack; }

/* ---- folders --------------------------------------------------------------- */

/* Calls fn(name, is_dir) for each entry of dir. */
static void list_dir(const char* dir, void (*fn)(const char*, int, void*), void* u) {
#ifdef _WIN32
    char pat[MAX_PATH];
    WIN32_FIND_DATAA d;
    snprintf(pat, sizeof pat, "%s\\*", dir);
    HANDLE h = FindFirstFileA(pat, &d);
    if (h == INVALID_HANDLE_VALUE) return;
    do if (d.cFileName[0] != '.') fn(d.cFileName, !!(d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY), u);
    while (FindNextFileA(h, &d));
    FindClose(h);
#else
    DIR* dp = opendir(dir);
    if (!dp) return;
    for (struct dirent* e; (e = readdir(dp)) != NULL; ) {
        if (e->d_name[0] == '.') continue;
        char p[MAX_PATH];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        DWORD a = GetFileAttributesA(p);
        fn(e->d_name, a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY), u);
    }
    closedir(dp);
#endif
}

typedef struct { char (*names)[64]; int n, max; } pack_list;
static void add_pack(const char* name, int is_dir, void* u) {
    pack_list* l = u;
    if (is_dir && l->n < l->max && strlen(name) < 64) snprintf(l->names[l->n++], 64, "%s", name);
}

int music_packs(char (*names)[64], int max) {
    pack_list l = { names, 0, max };
    list_dir(packs_dir, add_pack, &l);
    return l.n;
}

/* ---- naming the song --------------------------------------------------------- */

typedef struct { char name[19]; uint8_t* data; uint32_t len; } bank_song;
static bank_song known[512];   /* MUSIC songs, then SNDS effects */
static int nknown = -1;

static uint8_t* read_file(const char* dir, const char* name, uint32_t* len) {
    char p[MAX_PATH], low[64];
    size_t i = 0;
    for (; name[i] && i < sizeof low - 1; i++) low[i] = (char)tolower((unsigned char)name[i]);
    low[i] = 0;
    FILE* f = NULL;
    for (int k = 0; k < 2 && !f; k++) {          /* Linux installs may be lower case */
        snprintf(p, sizeof p, "%s/%s", dir, k ? low : name);
        f = fopen(p, "rb");
    }
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* b = n > 0 ? malloc((size_t)n) : NULL;
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    if (b) *len = (uint32_t)n;
    return b;
}

static void load_banks(void) {
    nknown = 0;
    for (int kind = 0; kind < 2; kind++)
    for (int bank = 0; bank < 2; bank++)
        for (int var = 0; var < 3; var++) {
            char tn[32], dn[32];
            uint32_t tl = 0, dl = 0;
            const char* k = kind ? "SNDS" : "MUSIC";
            snprintf(tn, sizeof tn, "%s%d-%d.TAB", k, bank, var);
            snprintf(dn, sizeof dn, "%s%d-%d.DAT", k, bank, var);
            uint8_t *tab = read_file(data_dir, tn, &tl), *dat = tab ? read_file(data_dir, dn, &dl) : NULL;
            /* ponytail: the banks stay loaded (a few hundred KB) for the run */
            for (uint32_t e = kind ? 0 : 32; dat && e + 32 <= tl && nknown < 512; e += 32) {
                uint32_t off = *(uint32_t*)(tab + e + 0x12), len = *(uint32_t*)(tab + e + 0x1A);
                if (!len || off + len > dl) continue;
                bank_song* s = &known[nknown++];
                memcpy(s->name, tab + e, 18);
                s->name[18] = 0;
                char* dot = strchr(s->name, '.');
                if (dot) *dot = 0;
                s->data = dat + off;
                s->len = len;
            }
            free(tab);
        }
}

const char* music_song_name(const uint8_t* hmp) {
    if (nknown < 0) load_banks();
    for (int i = 0; i < nknown; i++)
        if (!memcmp(hmp, known[i].data, known[i].len)) return known[i].name;
    return "";
}

/* The effect whose sample data is at `data` (`len` bytes): its name in the
 * SNDS banks (CASH, SICK, GRPBOO1...), or "". The game may skip the
 * sample's 16-byte header, so both starts are tried; 64 bytes decide. */
const char* music_effect_name(const uint8_t* data, uint32_t len) {
    if (nknown < 0) load_banks();
    uint32_t n = len < 64 ? len : 64;
    for (int i = 0; i < nknown; i++)
        for (uint32_t skip = 0; skip <= 16; skip += 16)
            if (known[i].len >= n + skip && !memcmp(data, known[i].data + skip, n)) return known[i].name;
    return "";
}

/* ---- the recording ------------------------------------------------------------- */

/* WAV: PCM, 8 or 16-bit (WAVE_FORMAT_EXTENSIBLE too). */
static int16_t* load_wav(const char* path, uint32_t* frames, int* ch, int* rate) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t h[12];
    int16_t* out = NULL;
    int bits = 0;
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) { fclose(f); return NULL; }
    for (;;) {
        uint8_t c[8];
        if (fread(c, 1, 8, f) != 8) break;
        uint32_t n = *(uint32_t*)(c + 4);
        if (!memcmp(c, "fmt ", 4) && n >= 16) {
            uint8_t fm[16];
            if (fread(fm, 1, 16, f) != 16) break;
            int tag = *(uint16_t*)fm;
            *ch = *(uint16_t*)(fm + 2); *rate = (int)*(uint32_t*)(fm + 4); bits = *(uint16_t*)(fm + 14);
            if ((tag != 1 && tag != 0xFFFE) || *ch < 1 || *ch > 2 || (bits != 8 && bits != 16)) break;
            fseek(f, (long)(n - 16 + (n & 1)), SEEK_CUR);
        } else if (!memcmp(c, "data", 4) && bits) {
            uint32_t bps = (uint32_t)(bits / 8 * *ch);
            *frames = n / bps;
            out = malloc((size_t)*frames * *ch * 2);
            if (!out) break;
            if (bits == 16) { if (fread(out, bps, *frames, f) != *frames) { free(out); out = NULL; } }
            else {
                for (uint32_t i = 0; i < *frames * (uint32_t)*ch; i++) out[i] = (int16_t)((fgetc(f) - 128) << 8);
            }
            break;
        } else fseek(f, (long)(n + (n & 1)), SEEK_CUR);
    }
    fclose(f);
    return out;
}

typedef struct { const char* song; char path[MAX_PATH]; const char* dir; } find_t;
static void match_file(const char* name, int is_dir, void* u) {
    find_t* q = u;
    const char* dot = strrchr(name, '.');
    if (is_dir || !dot || q->path[0]) return;
    size_t stem = (size_t)(dot - name);
    if (stem != strlen(q->song) || _strnicmp(name, q->song, stem)) return;
    if (_stricmp(dot, ".ogg") && _stricmp(dot, ".wav")) return;
    snprintf(q->path, sizeof q->path, "%s/%s", q->dir, name);
}

int16_t* music_load(const char* song, uint32_t* frames, int* ch, int* rate) {
    if (!pack[0] || !song[0]) return NULL;
    char dir[MAX_PATH];
    snprintf(dir, sizeof dir, "%s/%s", packs_dir, pack);
    find_t q = { song, "", dir };
    list_dir(dir, match_file, &q);
    if (!q.path[0]) return NULL;
    int16_t* pcm = NULL;
    const char* ext = strrchr(q.path, '.');
    if (!_stricmp(ext, ".wav")) pcm = load_wav(q.path, frames, ch, rate);
    else {
        int n = stb_vorbis_decode_filename(q.path, ch, rate, &pcm);
        if (n <= 0 || *ch < 1 || *ch > 2) { free(pcm); pcm = NULL; }
        else *frames = (uint32_t)n;
    }
    if (!pcm) fprintf(stderr, "[music] cannot read %s (Ogg Vorbis, or 8/16-bit PCM WAV, 1 or 2 channels)\n", q.path);
    else fprintf(stderr, "[music] %s: %s, %.0f s\n", song, q.path, (double)*frames / *rate);
    return pcm;
}

#undef STB_VORBIS_HEADER_ONLY
#include "../third_party/stb_vorbis.c"
