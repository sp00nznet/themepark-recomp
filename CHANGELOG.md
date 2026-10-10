# Changelog

All notable changes. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
versions follow [SemVer](https://semver.org/).

## [Unreleased]

### Fixed
- The mouse drifted on its own (laying a path to the right also laid it
  downward) and pushing at the screen's edge barely scrolled: the game reads
  how far the mouse moved from where it last put its cursor, and the host fed
  it the window position. The window now captures the mouse and sends raw
  motion (needs pcrecomp#76).
- Holding an arrow key scrolled one step: key repeats now reach the game, as a
  PC keyboard sends them.
- 640x480 drew no rides, buildings or people: lift32 made 16-bit `sar` a
  logical shift, and the game halves its sprite offsets with it (needs
  pcrecomp#72).
- Sound init never ran: the game's memory manager took every block and the
  C library could not open SNDSETUP.INF; 1 MB is held back while it grabs
  (needs pcrecomp#70).

### Added
- Save states: 9 slots in the settings menu, about 8 MB each, loading in a
  park in the same session or a later one (needs pcrecomp#77). `--save` and
  `--load` for scripted runs.
- Window size in the settings menu: whole multiples of 320x240 that fit the
  screen, remembered.
- `--move T:dx,dy` (relative mouse motion) for scripted runs; `TP_SNDLOG=1`
  logs the game's sound calls.
- Mods: `--mod NAME` lays `mods\NAME` over the game's files (needs
  pcrecomp#73).
- Settings menu on F10 or a right-click on the bars: scaling, colour, speed,
  fullscreen, mute and whether parks open in 640x480.
- Xbox controller (XInput, loaded at run time) as the mouse and keys.
- Parks open in the game's own 640x480 SVGA mode (it shipped behind the R
  key); `--lowres` / `[game] hires=0` for the original default.
- Speed control: Ctrl+PgUp / Ctrl+PgDn steps the game from 12 to 120 frames
  per second, shown in the title bar and remembered.
- Sound: HMI SOS replaced at its API (docs/sound.md). Effects from the 44.1 kHz
  16-bit bank through a host mixer on waveOut; the game's General MIDI songs
  through a host HMP sequencer on the Windows synth. `--mute`; headless
  `--record` carries the effects, `--wav` writes them alone.
- Direct3D 11 presenter (gunman's shaders): sharp-bilinear, smooth, CRT,
  nearest and integer scaling on F12, vivid colour on F9, borderless
  fullscreen on F11 / Alt+Enter, vsync; GDI when Direct3D is unavailable.
  Choices kept in `themepark.ini`, with `--scale`, `--fullscreen`, `--gdi`,
  `--vsync` and `[game] fps`.

## [0.1.0] - 2026-10-10

### Added
- The whole of MAIN.EXE lifted with pcrecomp: 6,517 functions, no lift errors,
  through pcrecomp's LE front end and DOS host (needs pcrecomp#66, #67, #68, #69).
- The game boots and runs from the title through the name prompt, main menu,
  new-game setup, world map and land purchase into a running park.
- Frame pacing at the game's input poll (`--fps`, default 25): the DOS game has
  no limiter, and natively its idle timeout fired in a fraction of a second
  (docs/pacing.md).
- A window with 4:3 letterboxing and F11 / Alt+Enter fullscreen.
- Headless runs on a virtual clock with `--record`, scripted `--click` / `--key`,
  and `--watch` / `--dump` for finding game variables.
- English by default (`--lang` picks German, Italian, Spanish or French).
- `Setup.cmd`: one-click setup from a GOG install or GOG's offline installer.
- `tools/conformance.py`: 6 milestones from boot to a running park.
