/*
 * sound.c - Theme Park's sound without HMI SOS.
 *
 * The game drives its sound through HMI's Sound Operating System, linked into
 * MAIN.EXE, which loads card drivers from HMIDRV.386 / HMIMDRV.386 at run time
 * and calls into them; the sample mixer is in those files too. None of that is
 * lifted, so the SOS API is replaced at its edge (run_lift.py OVERRIDES), and
 * everything above it stays the game's own lifted code: the channel table,
 * the de-duplication, the 20 Hz fades, the music selection, the scripts.
 *
 *   effects  start / done / stop / volume  -> audio.c's mixer
 *   music    init / start / stop / tempo   -> a host HMP sequencer on the
 *                                             Windows General MIDI synth
 *   drivers  detection, init, shutdown     -> succeed, with no hardware
 *
 * docs/sound.md has the map of the game's sound layer this is built on.
 * All SOS entry points are cdecl; a far pointer is an offset dword followed
 * by a selector dword.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"
#include "dos32.h"
#include "audio.h"

#define M8(a)  (*(uint8_t*)dos32_mem(a))
#define M16(a) (*(uint16_t*)dos32_mem(a))
#define M32(a) (*(uint32_t*)dos32_mem(a))
#define ARG(n) M32(g_esp + 4 + 4 * (n))       /* cdecl: the return address is at esp */

/* An overridden SOS function returns as its `ret` would: eax, pop the return. */
static void ret(uint32_t v) { g_eax = v; g_esp += 4; }

#define VARIANT     0x4AA727    /* sound bank variant: 0 11 kHz, 1 22 kHz, 2 44 kHz 16-bit */
#define NOOP_FN     0x0046E2A0u /* a lifted `ret`: where the driver's mixer tick points */

int sound_mute;                 /* --mute */
int sound_headless;             /* no devices: effects mixed for --record, music logged */

/* ---- drivers: there are none ------------------------------------------- */

/* Detection, system init/uninit, driver uninit, bank loading: all succeed. */
void sos_ok(void (*lifted)(void)) { (void)lifted; ret(0); }

/* sosDIGIInitDriver(id, far *hw, far *init, far *handle): handle 0, and the
 * mixer tick the driver would have provided (init+0x1C) points at a no-op, so
 * the SOS timer can still call it. */
void sos_digi_init_driver(void (*lifted)(void)) {
    (void)lifted;
    uint32_t init = ARG(3), handle = ARG(5);
    M32(handle) = 0;
    M32(init + 0x1C) = NOOP_FN;
    M16(init + 0x20) = 0x170;
    ret(0);
}

/* ---- effects ----------------------------------------------------------- */

static int voice_of[256];       /* the game's sample id -> mixer voice */

/* sosDIGIStartSample(h, far *sample) -> sample id. The struct (0x4AA729):
 * +0 data, +8 length, +0xC loop count (0xFFFF for ever), +0x10 volume
 * (0x7FFF full), +0x12 sample id, +0x1C flags (0x4000 looped). */
void sos_digi_start(void (*lifted)(void)) {
    (void)lifted;
    uint32_t s = ARG(1);
    uint32_t data = M32(s), len = M32(s + 8);
    int loops = M16(s + 0xC), vol = M16(s + 0x10), id = M16(s + 0x12) & 255, flags = M16(s + 0x1C);
    int variant = M8(VARIANT);
    int rate = variant == 2 ? 44100 : variant == 1 ? 22050 : 11025;
    int loop = !(flags & 0x4000) ? 0 : loops == 0xFFFF ? -1 : loops > 0 ? loops - 1 : 0;
    audio_stop(voice_of[id]);
    voice_of[id] = sound_mute ? 0 : audio_play(dos32_mem(data), len, variant == 2 ? 16 : 8, variant == 2, rate,
                                                vol >> 8, 64, loop);
    ret(id);
}

/* sosDIGISampleDone(h, id) -> 1 if finished or free. */
void sos_digi_done(void (*lifted)(void)) { (void)lifted; ret(!audio_playing(voice_of[ARG(1) & 255])); }

/* sosDIGIStopSample(h, id) */
void sos_digi_stop(void (*lifted)(void)) {
    (void)lifted;
    int id = ARG(1) & 255;
    audio_stop(voice_of[id]);
    voice_of[id] = 0;
    ret(0);
}

/* sosDIGISetSampleVolume(h, id, vol 0..0x7FFF) */
void sos_digi_volume(void (*lifted)(void)) {
    (void)lifted;
    audio_set(voice_of[ARG(1) & 255], (int)(ARG(2) & 0xFFFF) >> 8, -1, -1);
    ret(0);
}

/* ---- music: HMP on the General MIDI synth ------------------------------ */

/* HMP (HMI's MIDI): "HMIMIDIP", track count at +0x30, ticks per second at
 * +0x38 (120), tracks from +0x308, each a 12-byte header (+4 the chunk's
 * length including it) and MIDI events. Delta times are 7-bit groups, least
 * significant first, the last one with its top bit set. */
typedef struct { uint32_t tick; uint32_t msg; } mev_t;
typedef struct {
    mev_t* ev; int n;
    int rate;                   /* ticks per second */
    volatile int playing;
    volatile int tempo;         /* percent */
    double left;                /* headless: seconds of guest time still to "play" */
    HANDLE thread;
} song_t;

static song_t songs[8];
static HMIDIOUT midi;
static CRITICAL_SECTION midi_lock;
static int midi_volume = 127;   /* the game's music volume, 0..127 */
static uint8_t chan_vol[16];

static void midi_send(uint32_t msg) {
    if (!midi) return;
    if ((msg & 0xF0) == 0xB0 && ((msg >> 8) & 0x7F) == 7) {   /* channel volume: under the master */
        int c = msg & 15, v = (msg >> 16) & 0x7F;
        chan_vol[c] = (uint8_t)v;
        msg = (msg & 0xFF00FFFF) | ((uint32_t)(v * midi_volume / 127) << 16);
    }
    midiOutShortMsg(midi, msg);
}

static void midi_all_off(void) {
    if (!midi) return;
    for (int c = 0; c < 16; c++) {
        midiOutShortMsg(midi, 0xB0 | c | (123 << 8));   /* all notes off */
        midiOutShortMsg(midi, 0xB0 | c | (121 << 8));   /* reset controllers */
    }
}

static int cmp_ev(const void* a, const void* b) {
    const mev_t *x = a, *y = b;
    return x->tick < y->tick ? -1 : x->tick > y->tick;
}

static int parse_hmp(const uint8_t* p, uint32_t avail, song_t* s) {
    if (avail < 0x308 || memcmp(p, "HMIMIDIP", 8)) return 0;
    int ntracks = *(const uint32_t*)(p + 0x30);
    s->rate = *(const uint32_t*)(p + 0x38);
    if (s->rate <= 0 || s->rate > 1000) s->rate = 120;
    int cap = 4096;
    s->ev = malloc(cap * sizeof(mev_t)); s->n = 0;
    uint32_t off = 0x308;
    for (int t = 0; t < ntracks && off + 12 <= avail; t++) {
        uint32_t clen = *(const uint32_t*)(p + off + 4);
        if (clen < 12 || off + clen > avail) break;
        const uint8_t *q = p + off + 12, *end = p + off + clen;
        uint32_t tick = 0;
        uint8_t status = 0;
        while (q < end) {
            uint32_t d = 0; int sh = 0;
            for (;;) { uint8_t b = *q++; d |= (uint32_t)(b & 0x7F) << sh; sh += 7; if (b & 0x80 || q >= end) break; }
            tick += d;
            if (q >= end) break;
            uint8_t b = *q;
            if (b & 0x80) { status = b; q++; }
            if (status == 0xFF) {                       /* meta: length, then data; 2F ends */
                uint8_t type = *q++, len = *q++;
                q += len;
                if (type == 0x2F) break;
                continue;
            }
            if (status == 0xF0 || status == 0xF7) {    /* sysex: skip */
                uint8_t len = *q++; q += len; continue;
            }
            int n = (status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0 ? 1 : 2;
            uint32_t msg = status | (uint32_t)q[0] << 8 | (n == 2 ? (uint32_t)q[1] << 16 : 0);
            q += n;
            if (s->n == cap) { cap *= 2; s->ev = realloc(s->ev, cap * sizeof(mev_t)); }
            s->ev[s->n].tick = tick; s->ev[s->n].msg = msg; s->n++;
        }
        off += clen;
    }
    qsort(s->ev, s->n, sizeof(mev_t), cmp_ev);   /* ponytail: qsort is not stable; same-tick order across tracks may swap */
    return 1;
}

static DWORD WINAPI song_thread(LPVOID p) {
    song_t* s = p;
    LARGE_INTEGER f, t0, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    double at = 0;                                    /* seconds into the song */
    uint32_t last = 0;
    for (int i = 0; i < s->n && s->playing; i++) {
        at += (double)(s->ev[i].tick - last) / s->rate * 100.0 / (s->tempo > 0 ? s->tempo : 100);
        last = s->ev[i].tick;
        for (;;) {
            QueryPerformanceCounter(&t);
            double now = (double)(t.QuadPart - t0.QuadPart) / f.QuadPart;
            if (now >= at || !s->playing) break;
            Sleep(now + 0.003 < at ? 2 : 0);
        }
        if (!s->playing) break;
        EnterCriticalSection(&midi_lock);
        midi_send(s->ev[i].msg);
        LeaveCriticalSection(&midi_lock);
    }
    EnterCriticalSection(&midi_lock);
    midi_all_off();
    LeaveCriticalSection(&midi_lock);
    s->playing = 0;
    return 0;
}

static void song_stop(song_t* s) {
    s->playing = 0;
    if (s->thread) { WaitForSingleObject(s->thread, 2000); CloseHandle(s->thread); s->thread = NULL; }
}

/* sosMIDIInitDriver(id, far *hw, far *init, far *handle) */
void sos_midi_init_driver(void (*lifted)(void)) {
    (void)lifted;
    static int once;
    if (!once++) {
        InitializeCriticalSection(&midi_lock);
        if (!sound_mute && !sound_headless && midiOutOpen(&midi, MIDI_MAPPER, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
            fprintf(stderr, "[sound] no MIDI device: music off\n");
            midi = NULL;
        }
    }
    M32(ARG(5)) = 0;
    ret(0);
}

/* sosMIDIResetDriver(h) */
void sos_midi_reset(void (*lifted)(void)) {
    (void)lifted;
    if (midi) { EnterCriticalSection(&midi_lock); midi_all_off(); LeaveCriticalSection(&midi_lock); }
    ret(0);
}

/* sosMIDIInitSong(far *songDesc, far *trackMap, far *songHandle): the song's
 * data is the far pointer at songDesc+0. */
void sos_midi_init_song(void (*lifted)(void)) {
    (void)lifted;
    uint32_t desc = ARG(0), hp = ARG(4);
    int k = 0;
    for (; k < 8 && songs[k].ev; k++) {}
    if (k == 8) { ret(1); return; }
    uint32_t data = M32(desc);
    if (!parse_hmp(dos32_mem(data), 0x400000, &songs[k])) { ret(1); return; }
    songs[k].tempo = 100;
    M32(hp) = k;
    ret(0);
}

/* sosMIDIStartSong(song) */
void sos_midi_start(void (*lifted)(void)) {
    (void)lifted;
    song_t* s = &songs[ARG(0) & 7];
    song_stop(s);
    if (sound_headless) fprintf(stderr, "[sound] song start: %d events, %.0f s\n", s->n,
                                s->n ? (double)s->ev[s->n - 1].tick / s->rate : 0.0);
    if (sound_headless && s->ev) { s->left = s->n ? (double)s->ev[s->n - 1].tick / s->rate : 0; s->playing = 1; }
    if (s->ev && midi) {
        s->playing = 1;
        s->thread = CreateThread(NULL, 0, song_thread, s, 0, NULL);
    }
    ret(0);
}

/* sosMIDIStopSong(song) */
void sos_midi_stop(void (*lifted)(void)) { (void)lifted; song_stop(&songs[ARG(0) & 7]); ret(0); }

/* sosMIDIUnInitSong(song) */
void sos_midi_uninit_song(void (*lifted)(void)) {
    (void)lifted;
    song_t* s = &songs[ARG(0) & 7];
    song_stop(s);
    free(s->ev); s->ev = NULL; s->n = 0;
    ret(0);
}

/* sosMIDISongDone(song) -> 1 if not playing */
void sos_midi_done(void (*lifted)(void)) { (void)lifted; ret(!songs[ARG(0) & 7].playing); }

/* sosMIDISetTempo(song, percent) */
void sos_midi_tempo(void (*lifted)(void)) {
    (void)lifted;
    int pct = (int)ARG(1);
    songs[ARG(0) & 7].tempo = pct > 0 ? pct : 100;
    ret(0);
}

/* The game's music volume (0x4700AB, 0..127): it sends CC 7 to every channel
 * through the driver's send entry, which is not there; keep it as a master
 * under each channel's own volume instead. */
void music_volume(void (*lifted)(void)) {
    (void)lifted;
    midi_volume = (int)(ARG(0) & 0x7F);
    if (midi) {
        EnterCriticalSection(&midi_lock);
        for (int c = 0; c < 16; c++)
            midiOutShortMsg(midi, 0xB0 | c | (7 << 8) | ((uint32_t)((chan_vol[c] ? chan_vol[c] : 100) * midi_volume / 127) << 16));
        LeaveCriticalSection(&midi_lock);
    }
    ret(0);
}

/* sosMIDISendMIDIData(h, far *msg, len): raw MIDI bytes from the game, for
 * the sounds it plays through the synth. */
void sos_midi_send(void (*lifted)(void)) {
    (void)lifted;
    const uint8_t* p = dos32_mem(ARG(1));
    uint32_t len = ARG(3) & 0xFFFF;
    if (midi && len >= 1 && len <= 3) {
        uint32_t msg = p[0] | (len > 1 ? (uint32_t)p[1] << 8 : 0) | (len > 2 ? (uint32_t)p[2] << 16 : 0);
        EnterCriticalSection(&midi_lock);
        midi_send(msg);
        LeaveCriticalSection(&midi_lock);
    }
    ret(0);
}

/* Headless: songs run on guest time, so the game sees them finish when they would. */
void sound_tick(double dt) {
    for (int k = 0; k < 8; k++)
        if (sound_headless && songs[k].playing && (songs[k].left -= dt * (songs[k].tempo > 0 ? songs[k].tempo : 100) / 100.0) <= 0)
            songs[k].playing = 0;
}
