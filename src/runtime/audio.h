/* The host's sound mixer (audio.c). Voices play PCM from guest memory. */
#pragma once
#include <stdint.h>

/* silent: no device (headless); audio_render then mixes on demand. */
int  audio_init(int silent);
int  audio_rate(void);
void audio_render(int16_t* stereo, int frames);

/* Returns a voice id (0 = not played). vol 0..127, pan 0..127 (64 centre);
 * loop is how many times to repeat after the first play, -1 for ever. */
int  audio_play(const void* data, uint32_t bytes, int bits, int is_signed, int rate,
                int vol, int pan, int loop);
void audio_stop(int id);
int  audio_playing(int id);
void audio_set(int id, int vol, int pan, int rate);   /* -1 leaves a field */
void audio_stop_all(void);
void audio_master(int vol);

/* Called with every mixed block (for capture); may be NULL. */
extern void (*audio_tap)(const int16_t* stereo, int frames);
