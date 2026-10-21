/* Music packs (music.c): the game's songs replaced by recordings. */
#pragma once
#include <stdint.h>

/* Where packs live (music\ beside mods\) and the game's DATA folder, whose
 * MUSIC banks name the songs. Set once by main.c. */
void music_set_dirs(const char* packs_dir, const char* data_dir);

/* The pack in use, by folder name; "" plays the game's own MIDI. */
void music_set_pack(const char* name);
const char* music_pack(void);

/* The packs there are (folder names), up to max; returns how many. */
int music_packs(char (*names)[64], int max);

/* The song whose HMP data is at `hmp` in guest memory: its name from the
 * game's banks (RIDE1, FANFARE, INTRO3...), or "" if it is not one of them. */
const char* music_song_name(const uint8_t* hmp);

/* The same for a sound effect's sample data (sound.c's mutes, #41). */
const char* music_effect_name(const uint8_t* data, uint32_t len);

/* The current pack's recording of that song, decoded to 16-bit PCM
 * (malloc'd, `ch` interleaved channels). NULL if the pack has none. */
int16_t* music_load(const char* song, uint32_t* frames, int* ch, int* rate);
