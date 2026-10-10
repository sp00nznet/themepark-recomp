/*
 * audio.c - the host's sound: a software mixer on waveOut.
 *
 * The game's digital effects are mixed here instead of by a Sound Blaster.
 * Voices play PCM straight out of guest memory (the game keeps its sample
 * banks loaded), resampled to the output rate with linear interpolation, with
 * the game's volume and pan. One thread feeds waveOut from a ring of blocks;
 * the mix runs on that thread, so the game thread only flips voice fields
 * under a lock.
 *
 * ponytail: waveOut, not WASAPI: ~60 ms of latency, which a park sim does not
 * notice. Lower it with WASAPI if a game ever needs tighter timing.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "audio.h"

#define RATE    44100
#define BLOCK   1024              /* frames per block: 23 ms */
#define NBLOCK  4
#define NVOICE  32

typedef struct {
    const uint8_t* data;
    uint32_t len;                 /* in sample frames */
    uint32_t pos_fx;              /* 16.16 position */
    uint32_t step_fx;             /* 16.16 per output frame */
    int bits, is_signed, loop;   /* loop: repeats left, -1 forever */
    int vol, pan;                 /* 0..127, 0..127 (64 = centre) */
    int id;                       /* 0 = free */
} voice_t;

static voice_t voices[NVOICE];
static CRITICAL_SECTION lock;
static HWAVEOUT wo;
static WAVEHDR hdr[NBLOCK];
static int16_t buf[NBLOCK][BLOCK * 2];
static HANDLE ev;
static int next_id = 1;
static int master = 127;
static int started;
void (*audio_tap)(const int16_t* stereo, int frames);

static int sample(const voice_t* v, uint32_t i) {
    if (v->bits == 16) return ((const int16_t*)v->data)[i];
    int s = v->data[i];
    return (v->is_signed ? (int8_t)s : s - 128) << 8;
}

static void mix(int16_t* out, int frames) {
    static int32_t acc[BLOCK * 2];
    memset(acc, 0, sizeof acc);
    EnterCriticalSection(&lock);
    for (int k = 0; k < NVOICE; k++) {
        voice_t* v = &voices[k];
        if (!v->id) continue;
        int gl = v->vol * (127 - v->pan) / 64, gr = v->vol * v->pan / 64;   /* 0..254 each */
        if (gl > 127) gl = 127;
        if (gr > 127) gr = 127;
        for (int f = 0; f < frames; f++) {
            uint32_t i = v->pos_fx >> 16;
            if (i >= v->len) {
                if (!v->loop) { v->id = 0; break; }
                if (v->loop > 0) v->loop--;          /* -1 loops forever */
                v->pos_fx -= v->len << 16; i = v->pos_fx >> 16;
            }
            int a = sample(v, i), b = i + 1 < v->len ? sample(v, i + 1) : a;
            int s = a + (int)(((int64_t)(b - a) * (v->pos_fx & 0xFFFF)) >> 16);
            acc[f * 2] += s * gl / 127;
            acc[f * 2 + 1] += s * gr / 127;
            v->pos_fx += v->step_fx;
        }
    }
    LeaveCriticalSection(&lock);
    for (int i = 0; i < frames * 2; i++) {
        int32_t s = acc[i] * master / 127;
        out[i] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
    }
}

static DWORD WINAPI feeder(LPVOID p) {
    (void)p;
    for (;;) {
        WaitForSingleObject(ev, INFINITE);
        for (int b = 0; b < NBLOCK; b++) {
            if (!(hdr[b].dwFlags & WHDR_DONE)) continue;
            mix(buf[b], BLOCK);
            if (audio_tap) audio_tap(buf[b], BLOCK);
            hdr[b].dwFlags &= ~WHDR_DONE;
            waveOutWrite(wo, &hdr[b], sizeof hdr[b]);
        }
    }
    return 0;
}

int audio_init(int silent) {
    InitializeCriticalSection(&lock);
    if (silent) return 0;
    WAVEFORMATEX f = { WAVE_FORMAT_PCM, 2, RATE, RATE * 4, 4, 16, 0 };
    ev = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (waveOutOpen(&wo, WAVE_MAPPER, &f, (DWORD_PTR)ev, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        fprintf(stderr, "[audio] no audio device: running silent\n");
        return 0;
    }
    for (int b = 0; b < NBLOCK; b++) {
        hdr[b].lpData = (LPSTR)buf[b];
        hdr[b].dwBufferLength = sizeof buf[b];
        waveOutPrepareHeader(wo, &hdr[b], sizeof hdr[b]);
        hdr[b].dwFlags |= WHDR_DONE;
    }
    started = 1;
    CloseHandle(CreateThread(NULL, 0, feeder, NULL, 0, NULL));
    SetEvent(ev);
    return 1;
}

/* Headless runs mix on demand: --record pulls 1/35 s of sound per frame. */
void audio_render(int16_t* stereo, int frames) {
    while (frames > 0) {
        int n = frames > BLOCK ? BLOCK : frames;
        mix(stereo, n);
        stereo += n * 2;
        frames -= n;
    }
}

int audio_rate(void) { return RATE; }

int audio_play(const void* data, uint32_t bytes, int bits, int is_signed, int rate,
               int vol, int pan, int loop) {
    if (!data || !bytes || rate <= 0) return 0;
    EnterCriticalSection(&lock);
    int k = 0;
    for (; k < NVOICE && voices[k].id; k++) {}
    if (k == NVOICE) {   /* all busy: take the oldest */
        int oldest = 0;
        for (int j = 1; j < NVOICE; j++) if (voices[j].id < voices[oldest].id) oldest = j;
        k = oldest;
    }
    voice_t* v = &voices[k];
    v->data = data;
    v->len = bytes / (bits == 16 ? 2 : 1);
    v->pos_fx = 0;
    v->step_fx = (uint32_t)(((uint64_t)rate << 16) / RATE);
    v->bits = bits; v->is_signed = is_signed; v->loop = loop;
    v->vol = vol < 0 ? 0 : vol > 127 ? 127 : vol;
    v->pan = pan < 0 ? 0 : pan > 127 ? 127 : pan;
    v->id = next_id++;
    if (next_id <= 0) next_id = 1;
    int id = v->id;
    LeaveCriticalSection(&lock);
    return id;
}

static voice_t* find(int id) {
    for (int k = 0; k < NVOICE; k++) if (voices[k].id == id && id) return &voices[k];
    return NULL;
}

void audio_stop(int id) {
    EnterCriticalSection(&lock);
    voice_t* v = find(id);
    if (v) v->id = 0;
    LeaveCriticalSection(&lock);
}

int audio_playing(int id) {
    EnterCriticalSection(&lock);
    int r = find(id) != NULL;
    LeaveCriticalSection(&lock);
    return r;
}

void audio_set(int id, int vol, int pan, int rate) {
    EnterCriticalSection(&lock);
    voice_t* v = find(id);
    if (v) {
        if (vol >= 0) v->vol = vol > 127 ? 127 : vol;
        if (pan >= 0) v->pan = pan > 127 ? 127 : pan;
        if (rate > 0) v->step_fx = (uint32_t)(((uint64_t)rate << 16) / RATE);
    }
    LeaveCriticalSection(&lock);
}

void audio_stop_all(void) {
    EnterCriticalSection(&lock);
    for (int k = 0; k < NVOICE; k++) voices[k].id = 0;
    LeaveCriticalSection(&lock);
}

void audio_master(int vol) { master = vol < 0 ? 0 : vol > 127 ? 127 : vol; }
