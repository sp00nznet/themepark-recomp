# Architecture

```
game\GAME\MAIN.EXE  (MZ stub + LE image, Watcom C/C++32, DOS/4GW)
   │  run_lift.py
   │    pcrecomp tools/le/le_parse.py     objects at 0x400000, 19,728 fixups applied, PE wrap
   │    pcrecomp tools/disasm/disasm32.py function catalog -> analysis/d32_funcs.json
   │    pcrecomp tools/lift/lift32.py     Lifter(dos=True, call_pop=...)
   ▼
src\recomp\gen\   (generated, gitignored)
   recomp_NNNN.c      6,517 lifted functions
   recomp_dispatch.c  address -> function table, entry point
   image_data.c       the relocated image, copied into guest memory at start
   │  build.cmd (clang-cl, x64)          build-linux.sh (gcc, SDL2)
   ▼
build\themepark.exe                    build-linux/themepark
   pcrecomp runtime/dos32/dos32.c   DOS/4GW, DPMI, DOS, BIOS, VGA, mouse, timer (the guest's world)
   src/runtime/main.c               options, the game thread, headless --record, scripted input
   src/runtime/present.c            Windows: the window, keyboard, mouse, themepark.ini
   src/runtime/present_d3d.c        Windows: Direct3D 11 scaling shaders (GDI fallback in present.c)
   src/runtime/present_sdl.c        Linux: the same window on SDL2, its scaling and dialogs
   src/runtime/pad.c                an Xbox controller as the mouse and keys (XInput; SDL on Linux)
   src/runtime/game.c               Theme Park specifics: frame pacing, memory hold-back, save states
   src/runtime/sound.c              HMI SOS replaced: effects and music (docs/sound.md)
   src/runtime/audio.c              the effects mixer on waveOut (SDL audio on Linux)
   src/runtime/compat.h, compat.c   Linux: the Win32 names the shared files use (threads, ini)
```

The two systems share everything but the window, the sound device, the MIDI
synth and the pad. On Linux the MIDI goes to ALSA's sequencer (sound.c), and
the lifted C needs `-foptimize-sibling-calls` from gcc (CMakeLists.txt;
pcrecomp `docs/DOS32.md` says why).

## Who owns what

- **pcrecomp** owns everything any DOS/4GW game needs: the LE front end, the
  lifter's DOS mode, and the dos32 host (its reasoning is in pcrecomp's
  `docs/DOS32.md`). Gaps found here are fixed there, through PRs (#66–#69).
- **This repo** owns the title: which functions are overridden and why
  (`run_lift.py` `OVERRIDES`, `src/runtime/game.c`), the presenter, the
  options, and the scripts that drive the game in tests.

## Threads

- The **game thread** runs the lifted code from the Watcom entry point, on a
  256 MB stack (every guest call is a host call, and the game recurses). dos32
  delivers the game's timer, keyboard and mouse interrupts on this thread, at
  points where lifted code calls into the host.
- The **main thread** owns the window. It reads the screen with `dos32_frame`
  60 times a second and hands keys and the mouse to dos32, which queues them
  for the game thread.

Headless, there is no window: the main thread waits for the game thread, and
the frame callback (70 Hz of guest time) records video and fires scripted
input.

## Guest memory and the game

The LE objects are placed at 0x400000 rather than the linker's 0x10000,
because Watcom's bases fall on conventional memory and the VGA window. Code at
0x400000–0x484DEB, data at 0x4A0000–0x4D2340; the game then takes 64 MB of DPMI
memory for its heap. pcrecomp `docs/DOS32.md` has the full map.

Variables this repo relies on (all found with the tools in testing.md):

| Address | What |
|---|---|
| `0x4C6652` | attract/menu state word: `0x66` title, `0x50` name prompt, `0` demo or menus |
| `0x4C4482` | month of the park calendar, 0 = January |
| `0x4C6606` | idle-frame counter; 3000 starts the attract demo |
| `0x4A14B5` | the game's timer-interrupt tick count (72.8 Hz) |

## Overrides

A function in `run_lift.py` `OVERRIDES` is lifted as `sub_X_lifted`, and
`sub_X` becomes a host function that is handed the lifted body. Three kinds:

- **Pacing**: `0x0040107F`, the per-frame input poll, becomes `pace_frame`
  (game.c), which waits for the frame slot and then runs the lifted body. See
  [pacing.md](pacing.md).
- **Memory**: `0x00472950`, Bullfrog's grab-everything memory manager, runs
  inside `hold_back_memory` (game.c), which keeps 1 MB away from it.
- **Sound**: 28 HMI SOS entry points and the game's music-volume call become host functions in sound.c that do
  the job themselves and return as the `ret` would, never calling the lifted
  body. See [sound.md](sound.md).

## The opening film

The film is a program of its own, `INTRO.EXE` (179 KB: the same Watcom and
DOS/4GW, its own copies of Bullfrog's memory manager and HMI SOS), which the
1994 launcher and GOG's run as `intro -C:/THEME.CD` before `main`. GOG's and
the CD's copies are the same file (SHA-1 fdb4076e…). It plays `data/intro.dat`
(16 MB) and the sounds and songs in it.

- `run_lift.py --intro` lifts it to `src/recomp/gen_intro/` (1,057 functions)
  with its own override table, `INTRO_OVERRIDES`: the same host functions as
  the game's (memory hold-back, the SOS entry points), at the intro's
  addresses, found by matching the game's overridden functions byte for byte
  with relocated words masked. Every symbol gets an `intro_` prefix
  (`intro_00401554`, `intro_dispatch_table`, `intro_image`), so both lifts
  link into one exe.
- main.c's game thread runs `dos32_run` twice: the intro, with the intro's
  image and dispatch table in the config, then the game. dos32 starts the
  second run on a fresh machine (pcrecomp#122) while the window stays up.
  sound.c's two addresses (the sound bank variant byte, a lifted `ret` for the
  driver's mixer tick) point at the intro's copies while it runs, and its
  voices and songs are stopped when it ends.
- The film paces itself on the timer HMI SOS sets to 240 Hz, so it needs no
  pacing hook: it runs 169 s, its voices each ending before the next begins.
  Esc and Space skip it (it never reads the mouse).
- Headless runs leave it out unless `--intro` asks, so scripted input keeps
  its timing from the start of the game. `--nointro` and `[game] intro=0` leave
  it out of windowed play.
