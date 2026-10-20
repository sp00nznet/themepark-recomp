# Changelog

All notable changes. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
versions follow [SemVer](https://semver.org/).

## [Unreleased]

### Added
- The original 1994 CD works as well as GOG's release (#19). Its MAIN.EXE is
  GOG's but for two mouse fixes GOG made, which `run_lift.py` now applies, so
  both lift to the same C. Setup takes the CD's drive or folder and lays out
  `THEME.CD\` the way the CD's installer did (its 32 files, `SAVE\` and a
  `SNDSETUP.INF`). A MAIN.EXE the lift does not know now stops it with its
  SHA-1, instead of building something whose addresses are wrong.
- Wider parks (#11): the park's 640x480 screen at any width from 640 to
  1920, 480 high, picked in F10 (*Park resolution*: 800x480, 856x480 for
  16:9, 1120x480 for 21:9, or the screen's own shape), `--res WxH` or
  `[game] res`. The park fills the width, the pointer, edge scrolling and the
  view reach it, and the panel and the game's screens stay 640 wide at the
  left. 303 instructions of the lifted C read the screen's size instead of
  640/480 (docs/resolutions.md). Taller screens are not done yet. Needs
  pcrecomp's dos32-custom-vesa (a VESA mode of the host's own size).
- The opening film. It is a program of its own, INTRO.EXE, which the 1994
  installer's and GOG's launchers run before MAIN.EXE; the recomp ran only the
  game. INTRO.EXE is now lifted too (`run_lift.py --intro`, the setup does
  it) and linked into the same exe, and it plays in the same window before
  the title, music and voices included. Esc or Space skips it, as it always
  did; `--nointro` or `[game] intro=0` leaves it out. Headless runs leave it
  out unless `--intro` asks (needs pcrecomp#122).

### Fixed
- The pointer came apart, from the top down, after a long game (#6). Every
  change of resolution (R, and the screens that switch) ran the mouse
  set-up, which took three new 4 KB buffers from the game's memory pool and
  never gave them back; GOG's patch stops that by rewriting the game's own
  code, which does nothing to the lifted C. The pool keeps its blocks in a
  table of 256, and after about 60 switches it was full: the pointer was
  drawn from address 0, and from then on every allocation in the game failed
  (films and screens load into that pool too). The buffers are now taken
  once, as GOG meant.
- Ride films played nearly three times too fast: they time their frames on
  the DOS time of day, which moved every hundredth instead of in 1/18.2 s
  ticks as on DOS (needs pcrecomp#121). The takeover and game-over films
  were paced at the game's frame rate instead of their own retrace clock, and
  in 320x200 showed 6 frames a second instead of 17.5 (#10).
- Effects longer than 1.5 s (the bus arriving and leaving, fireworks, the
  screams on rides) repeated their opening for ever and only stopped when
  sound was turned off and on: the mixer's play position wrapped at 65536
  frames (#1).
- The year-end report and charts flashed up and closed at once: their loop
  never calls the input poll that paces the game, and closes itself after
  1000 passes. Frames are now also paced where they are shown (#4).
- Staff and goods negotiations were over in two or three seconds, too quick
  to make an offer: their loop paces itself on the vertical retrace only.
  Each pass is now a frame, and a negotiation takes about 18 s at the default
  speed (#5).
- The mouse drifted on its own (laying a path to the right also laid it
  downward) and pushing at the screen's edge barely scrolled: the game reads
  how far the mouse moved from where it last put its cursor, and the host fed
  it the window position. The window now captures the mouse and sends raw
  motion (needs pcrecomp#76).
- The captured mouse moved slower than the Windows pointer: it now follows
  the pointer's own speed and acceleration (raw input had neither).
- Holding an arrow key scrolled one step: key repeats now reach the game, as a
  PC keyboard sends them.
- 640x480 drew no rides, buildings or people: lift32 made 16-bit `sar` a
  logical shift, and the game halves its sprite offsets with it (needs
  pcrecomp#72).
- Sound init never ran: the game's memory manager took every block and the
  C library could not open SNDSETUP.INF; 1 MB is held back while it grabs
  (needs pcrecomp#70).

### Added
- Linux, natively: the same lifted C built with gcc and SDL2
  (`./setup.sh`, `./build-linux.sh`). An SDL window with every scaling mode,
  the captured mouse, the settings menu as dialogs, SDL audio for the
  effects, music on ALSA's sequencer, and the pad as an SDL GameController.
  Conformance is 6/6, save states load across runs, and the guest memory
  after a scripted run is identical to the Windows build's (needs
  pcrecomp#84).
- Save states: 9 slots in the settings menu, about 8 MB each, loading in a
  park in the same session or a later one (needs pcrecomp#77). `--save` and
  `--load` for scripted runs.
- Pixels doubled up by default: integer scaling (nearest, whole multiples)
  in a window of 1x, 2x or 3x the park's 640x480, picked in the settings menu
  and remembered; the largest that fits to start (2x on a 1080p screen).
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
