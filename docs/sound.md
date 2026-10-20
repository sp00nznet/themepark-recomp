# Sound

Theme Park drives its sound through HMI's Sound Operating System (SOS),
linked into MAIN.EXE. SOS loads card drivers from `GAME\HMIDRV.386`,
`HMIMDRV.386` and `HMIDET.386` at run time and calls into them through
function-pointer tables; the digital mixer itself lives in those files. None
of that code is in the lifted image, so the SOS API is replaced at its edge
(`run_lift.py` `OVERRIDES`, `src/runtime/sound.c`) and everything above it
stays the game's own lifted code.

Everything here was read from the game's binary and data, and from runs of the
recompiled game; no HMI SDK material and no other implementation was used.

## Layers

```
gameplay, scripts, menus
   │
Bullfrog's sound layer (lifted)
   0x470C80 play (tag, snd)      0x470FE0 play unless already playing
   0x471AD0 play looped          0x4705A4 / 0x471984 fade in / out (20 Hz timer callbacks)
   0x470350 stop one             0x470DC0 stop all
   0x470A20 play music track     0x470990 stop music    0x4701A0 restart if finished
   0x470E40 music tempo          0x4700AB music volume (overridden)
   │
HMI SOS (replaced)                                       host
   digital  0x47943F start  0x4792DB done  0x479A48 stop  0x4779F8 volume  -> audio.c mixer
   MIDI     0x46F68F init song  0x46FC05 start  0x46FC8D stop  0x46FBC2 uninit
            0x472174 done  0x479E5C tempo  0x477202 send          -> HMP sequencer, midiOut
   drivers  detection, init, shutdown                            -> succeed, no hardware
   timer    0x4721FC / 0x4722F4 (hooks int 8, runs events)        -> stays lifted
```

The game keeps a 32-slot channel table at `0x4C7998`; a slot number is the SOS
sample id. Flags: `0x4AA714` MIDI present, `0x4AA715` music on, `0x4AA720`
digital present, `0x4AA721` effects on.

## Effects

`SNDSn-v.TAB` / `.DAT`: TAB entries are 32 bytes (name at +0, DAT offset at
+0x12, size at +0x1A); the DAT is headerless PCM, and the last 16 bytes of each
sample are not sound (the game passes size − 0x10). The variant `v` is chosen
from the card named in `SNDSETUP.INF` (`0x45058B`) and kept at `0x4AA727`:

| v | Format | Card |
|---|---|---|
| 0 | 8-bit unsigned, 11025 Hz | fallback |
| 1 | 8-bit unsigned, 22050 Hz | SB, SB Pro, GUS |
| 2 | 16-bit signed, 44100 Hz | SB16 (GOG's setting) |

The start-sample struct (static at `0x4AA729`): +0 data, +8 length, +0xC loop
count (0xFFFF for ever), +0x10 volume (0x7FFF full), +0x12 sample id, +0x1C
flags (0x4000 looped). The game never pans or changes rate.

`audio.c` mixes up to 32 voices from guest memory to 44.1 kHz stereo on
waveOut (SDL audio on Linux). A voice's position is 48.16 fixed point: it was
16.16 in 32 bits, which wraps at 65536 frames, so every effect longer than
1.5 s at 44.1 kHz (the bus arriving and leaving, fireworks, the screams on
rides) replayed its first 1.5 s for ever and never reported itself finished
(issue #1). Headless runs mix on the 70 Hz frame clock instead, so `--record`
carries the effects.

## Music

`MUSICn-v.TAB` / `.DAT` hold HMP songs (`HMIMIDIP`): track count at +0x30,
ticks per second at +0x38 (120), tracks from +0x308 with a 12-byte header
(+4 the chunk length), then MIDI events whose delta times are 7-bit groups,
least significant first, the last one with its top bit set. Variant 2 (`.GEN`)
is General MIDI and is what `MUSIC = GENERAL` selects; 0 is AdLib FM, 1 Roland.

`MUSIC0` has 22 songs: a jingle per ride type (RIDE1–17), and HRTBEAT,
FANFARE, TOVER, WIN2, FUNERAL. Every one parses with balanced notes (checked
with a standalone reader). Songs play while a ride is shown, which is why an
empty new park is silent.

The host sequencer merges the tracks and plays them on the Windows General
MIDI synth (`MIDI_MAPPER`), or on Linux on the first synth port of ALSA's
sequencer (FluidSynth or TiMidity++ as a server; `TP_MIDI=client:port` picks
one), at 120 ticks per second scaled by the game's tempo
percentage. The game's music volume scales each channel's CC 7. Headless runs
open no MIDI device; a song "plays" for its length in guest time so the game
sees it finish, and each start is logged:

```
[sound] song start: 464 events, 18 s
[sound] song start: 668 events, 23 s
```

## Music packs

A pack (music/README.md) replaces songs with recordings. The game hands SOS
only a song's HMP bytes, so music.c names the song by finding those bytes in
the game's banks: each `.TAB` entry is 32 bytes, the name at +0, the offset
into the `.DAT` at +0x12, the length at +0x1A, and all three variants are
searched. When a named song starts and the pack has a file for it, the file is
decoded (stb_vorbis for Ogg, or a WAV reader) and played on a stereo mixer
voice instead of the sequencer, and "song done" asks the mixer. Decoding
happens at the start, so only the playing song is in memory. Picking a pack in
the menu ends the songs playing at the game's next "done?" poll, on the game
thread, and the game starts its next song from the new pack.

## Two things that were in the way

- **Memory.** Bullfrog's memory manager (`0x472950`) mallocs every block it can
  get, down to 16 bytes. The C library then could not allocate the `FILE` for
  `fopen("C:/THEME.CD//sndsetup.inf")`, so sound init never ran and the game
  printed "No sound drivers defined". The host holds 1 MB of DPMI memory back
  while that function runs (`hold_back_memory`, game.c; pcrecomp#70).
- **`SNDSETUP.INF`.** GOG keeps it beside its DOSBox config, not in `THEME.CD`;
  `Setup.cmd` copies it in. Without it the game runs silent.
