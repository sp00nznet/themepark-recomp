# Testing and debugging

## Headless runs

```
build\themepark.exe --headless --seconds 60 --record out.mp4
```

No window, nothing on any screen, so it works over RDP. Time is guest time on
a virtual clock (pacing.md): runs are reproducible and faster than real time.
`--record` pipes frames to ffmpeg at 35 fps, scaled 2x nearest, and muxes in
the sound effects mixed on the frame clock (music is MIDI and is not captured;
song starts are logged as `[sound] song start`). `--wav file` writes the
effects alone. Headless runs never open an audio or MIDI device. Set
`TP_REALTIME=1` to run headless on the wall clock instead.

Frames for a look: `ffmpeg -ss 30 -i out.mp4 -frames:v 1 shot.png`, or a
contact sheet: `ffmpeg -i out.mp4 -vf "fps=1/2,scale=320:240,tile=6x4" -frames:v 1 sheet.png`.

## Scripted input

All times are guest seconds; positions are game pixels (320x200 in the menus,
640x480 in a park).

| Option | Does |
|---|---|
| `--click T:x,y` | left press at T, release 4 frames later, the pointer moved there first |
| `--key T:sc` | press a key, release it a sixth of a second later; `sc` is a set-1 scancode in hex (`1C` Enter, `39` space, `3B` F1, `18` O) |
| `--move T:dx,dy` | relative mouse motion, as the captured mouse sends it (pushing past an edge scrolls the park) |
| `--save T:slot`, `--load T:slot` | save or load a save state (slots 1-9, `states\` beside the exe) |
| `--fps-at T:N` | change the frame rate at T (0 = unpaced) |

`tools/conformance.py` has the script from the title into a park.

## Finding things out

| Option | Does |
|---|---|
| `--log` | dos32's log: ints, file opens, vector installs, DPMI allocations, port read counts |
| `--watch addr` | print a guest word whenever it changes (up to 8) |
| `--dump T:file` | write guest memory from 0x400000 to the heap top (0x5000000) to a file at T (diff two to find a variable) |
| `TP_SNDLOG=1` | log every effect start, stop and volume change and every MIDI message the game sends |
| `DOS32_BREAK=addr` | hardware write watchpoint: every write to the word, with the lifted function that made it |

Unhandled ints and ports are always reported once each. A guest fault prints
the guest address, the lifted function, registers, the last indirect calls and
the top of the stack.

How the calendar variable was found: dump at 60 s and 80 s in a park, read the
date off the two frames (1 FEB, 13 MAR), diff the dumps for a byte that went
1 -> 2. That is `0x4C4482`.

## Conformance

```
py -3 tools/conformance.py            # score this build against conformance.json
py -3 tools/conformance.py --update   # accept this run as the baseline
```

Six milestones from one scripted run (title, name prompt, main menu, park
calendar running, no faults, no unresolved dispatches), plus the lift's
function and error counts. Any line worse than `conformance.json` fails. With
no `game\` or no build it prints `SKIP` and exits 0. Today: 6/6.

## On netlab

The recipe is `projects/themepark-recomp.env` in recomp-netlab:

```
netlab check themepark-recomp --on testbox
```

builds on the farm (x64 clang-cl) and runs `tools\qa.cmd`, which is the
conformance script where `game\` exists and `SKIP: no game data` where it
doesn't (the test VMs).

## Checking the presenter with no screen

```
build\themepark.exe --seconds 70 <script> --present-test DIR
```

runs headless, then draws the last screen through every scaling mode with
Direct3D into a hidden 1440x1080 window and writes each back buffer to
`DIR\<mode>.bmp`. It exits non-zero if Direct3D could not draw. This is how
the shaders are checked when no display can be filmed.

## Windowed runs

At the console, run windowed builds on a virtual monitor so they never land on
a real screen:

```
offstage run --size 1280x960 --record window.mp4 -- build\themepark.exe --seconds 20
```

`--seconds` in a window is real time.
