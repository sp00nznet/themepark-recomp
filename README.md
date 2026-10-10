# Theme Park — static recompilation

A static recompilation of Bullfrog's **Theme Park** (DOS, 1994) to a native
Windows program. The game's own 32-bit code is translated to C by
[pcrecomp](https://github.com/sp00nznet/pcrecomp) and runs on its DOS/4GW host;
no emulator and no DOSBox. You supply your own copy (the GOG release); nothing
from the game is in this repository.

Part of the [pcrecomp](https://github.com/sp00nznet/pcrecomp) family, and built
in its shared house style: the same pipeline, CLI flags, headless recording and
conformance harness as the other recomp repos.

## Status

**v0.1.0 — alpha.** The whole game is lifted (6,517 functions, no lift errors)
and runs from the title through the name prompt, menus, new-game setup and
world map into a running park. It has not been played for long by a person
yet. Sound effects and music play through the host (HMI SOS replaced;
[docs/sound.md](docs/sound.md)). The remaster features below are on the [roadmap](ROADMAP.md),
not done.

| Area | State |
|---|---|
| Boot, title, attract demo | works |
| Name prompt, main menu, setup screens, world map | works |
| Park simulation (calendar, visitors, money) | runs; long play untested |
| Keyboard and mouse | work |
| Xbox controller | mapped onto the mouse and keys; not yet tried with a real pad |
| Settings menu (F10) | scaling, colour, speed, fullscreen, mute, 640x480 default |
| 640x480 SVGA mode | the game's own hi-res park view, now the default (R toggles, `--lowres` for the original) |
| Sound effects | host mixer, 44.1 kHz 16-bit (the SB16 bank) |
| Music | the game's General MIDI songs on the Windows synth |
| Saving and loading | untested |
| Own presenter | Direct3D 11: sharp-bilinear, smooth, CRT, nearest, integer (F12), vivid colour (F9), borderless fullscreen (F11); GDI fallback |

Conformance (`tools/conformance.py`): **6/6 milestones** — title, name prompt,
main menu, park calendar running, no faults, no unresolved dispatches.

## Screenshots

Real output of the recompiled game.

| | |
|---|---|
| ![Title](docs/screenshots/title.png) | ![Attract demo](docs/screenshots/attract-park.png) |
| ![Main menu](docs/screenshots/main-menu.png) | ![World map](docs/screenshots/world-map.png) |
| ![Buying land](docs/screenshots/buy-land.png) | ![A new park](docs/screenshots/new-park.png) |
| ![The park in 640x480](docs/screenshots/hires-park.png) | |

## Getting Started

You need Windows 10 or 11 (x64) and Theme Park from GOG: either installed
through GOG Galaxy, or GOG's offline installer
(`setup_theme_park_1.01_cs_(77027).exe`).

### Quick start

1. Download this repository (green **Code** button, **Download ZIP**) and unzip
   it somewhere without an `&` in the path, for example `C:\themepark`.
2. Double-click **`Setup.cmd`**.
   - It checks for Python 3, the `capstone` and `pefile` packages, the pcrecomp
     toolkit, Visual Studio 2022 with C++, LLVM (clang-cl), CMake and Ninja, and
     asks before installing anything missing.
   - It finds your GOG install (or the GOG installer in this folder or your
     Downloads) and asks for the path if it can't.
   - It lifts the game to C (a few minutes), then builds it (5 to 20 minutes).
   - If a step fails it says what to do in one sentence; the details are in
     `setup.log`. Run it again and finished steps are skipped.
3. Double-click **`Theme Park (recomp).cmd`**.

### Step by step

The Quick start runs exactly these commands.

1. Prerequisites:
   - Python 3.10+ (`py -3 --version` prints `Python 3.x`; if `python` opens
     the Microsoft Store, use `py -3`, or turn off the Store alias in
     Settings > Apps > Advanced app settings > App execution aliases).
   - `py -3 -m pip install --user capstone pefile`
   - Visual Studio 2022 (or Build Tools) with *Desktop development with C++*,
     LLVM 17+ (`winget install LLVM.LLVM`), CMake 3.20+, Ninja. Open a new
     terminal after installing so PATH picks them up.
2. Put [pcrecomp](https://github.com/sp00nznet/pcrecomp) beside this folder as
   `..\tools` (or set `PCRECOMP` to its path):
   ```
   git clone https://github.com/sp00nznet/pcrecomp ..\tools
   ```
3. Put your game in `game\` (it must contain `GAME\MAIN.EXE` and `THEME.CD\DATA`):
   ```
   innoextract -s -d game "setup_theme_park_1.01_cs_(77027).exe"
   ```
   or copy your GOG install folder to `game\`.
4. Lift:
   ```
   py -3 run_lift.py
   ```
   The first run builds the function catalog (`analysis\d32_funcs.json`, a few
   minutes). It ends with:
   ```
   ============================================================
     functions 6517   errors 0   files 17   lines 911,420   8.3s
   ============================================================
   ```
5. Build: `build.cmd`. The result is `build\themepark.exe`.
6. Run: `build\themepark.exe`.

Usual trip-ups: `py` vs `python` (above); a path with `&` in it breaks the
build; a pcrecomp checkout older than the DOS host stops the configure with
`pcrecomp at ... has no runtime/dos32` — `git -C ..\tools pull`.

## Usage

```
themepark.exe                       play in a window: F10 or right-click on the bars for the settings
                                    menu; F12 scaling, F9 vivid colour, F11 / Alt+Enter fullscreen
themepark.exe --fps 35              the game's speed: frames per second (default 25; Ctrl+PgUp/PgDn in game)
themepark.exe --lowres              parks in 320x200, as the original started (R toggles either way)
themepark.exe --scale crt           scaling: sharp (default), smooth, crt, nearest, integer
themepark.exe --fullscreen          borderless fullscreen;  --gdi: no Direct3D;  --vsync 0
themepark.exe --mute                no sound
themepark.exe --lang 1              language: 0 English (default), 1 German, 2 Italian, 3 Spanish, 4 French
themepark.exe --headless --seconds 60 --record out.mp4
                                    no window: run 60 guest seconds and film it (needs ffmpeg)
themepark.exe --headless --seconds 90 --click 10:160,100 --key 13:14 ...
                                    scripted input at guest times (game pixels, set-1 scancodes)
```

An Xbox controller works alongside the mouse: left stick the pointer, A / B
the buttons, D-pad or right stick to scroll, LB / RB speed, Y the 640x480
switch, X Space, Start Esc, Back F1 (not yet tried on a real pad).

Display choices are remembered in `themepark.ini` beside the exe (`[display]
scale`, `vivid`, `fullscreen`, `vsync`; `[game] fps`, `hires`).

Headless runs keep their own clock, so the same script gives the same run every
time, faster than real time. Debugging aids (`--watch`, `--dump`, `--log`,
`DOS32_BREAK`) are in [docs/testing.md](docs/testing.md).

## Building from source

`run_lift.py` then `build.cmd`, as in Step by step. `build.cmd` configures a
64-bit clang-cl build with Ninja into `build\` (set `BUILD_DIR` for another
folder, `PCRECOMP` for another toolkit). The lifted C in `src\recomp\gen\` is
generated from your copy of the game on your machine and is never distributed.

- [docs/architecture.md](docs/architecture.md): how the pieces fit.
- [docs/pacing.md](docs/pacing.md): why the game needs a frame limiter it never had.
- [docs/testing.md](docs/testing.md): headless runs, scripted input, the conformance harness.
- [docs/sound.md](docs/sound.md): how HMI SOS is replaced.

## License

MIT for the code in this repository ([LICENSE](LICENSE)). Theme Park is © 1994
Bullfrog Productions / Electronic Arts; this project contains none of it and
needs your own copy.
