# Changelog

All notable changes. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
versions follow [SemVer](https://semver.org/).

## [Unreleased]

## [0.3.0-alpha.1] - 2026-10-21

### Added
- Modern rules (F10, *Modern rules*; `[rules]` in themepark.ini; `--rules`):
  fixes to how the 1994 game plays, each off until turned on. In a network
  game everyone plays by the host's (docs/rules.md).
  - Guards go after thugs nearby (#8). A patrolling guard stands still and
    looks once every 16 seconds, straight ahead along the path; with the rule
    it heads for any thug within 12 tiles. In the demo park, one guard caught
    12 of 12 thugs instead of 5.
  - Stuck guests find their way out (#17): a guest the game cannot route out
    is moved out of the pocket it is fenced into, or along the shortest way
    to the gate, after a few seconds.
  - The advisor keeps advising (#16): advice no longer fades away over a long
    session, he warns of the year end in November and names what research has
    found, and the shop-price advice no longer asks for twenty times the base
    price an hour in.
  - Entertainers entertain (#29): no flash between guests, no flicker when
    fully trained (the walk animation no longer restarts every tile), the
    panel's PEOPLE ENTERTAINED and PEOPLE MADE HAPPY count (the second was
    never written), and dropped staff keep their trained speed.
  - Handymen stay on the job (#7): back from a strike or off the paths, to
    the nearest path and their patrol instead of mowing a far lawn; patrol
    legs take the shortest way along the paths, so patrols of up to 8
    waypoints go round.
  - More room to build (#23, #21, #15): fences under raised track, lamp posts
    and fountains on paths, and track that keeps building in a full park (track pieces
    share a table with the guests, who could fill it).
  - Lamps light up in the dark (#18): lamp posts glow while rain and storms
    darken the park; only the picture changes.
  - The guards rule also lets go of a caught thug whose guard was moved or
    fired, which paced round her or froze for good (#29).
- `tools/rulescheck.py` tries each rule off and on in the demo park;
  `--call T:ADDR[:args]` calls one of the game's functions in a test run
  (hire staff, send thugs, load the demo park).

### Fixed
- The attract demo ended the moment it started when parks open in 640x480
  (the default): the demo centring the pointer counted as the player moving
  it.
- Setup stopped at the build when an earlier setup had left an older
  pcrecomp in `../tools` (#33): it now checks for the newest toolkit feature
  the game uses, and fetches a current pcrecomp into `../pcrecomp` (leaving
  the old one alone), or updates an old `../pcrecomp`.
- The Windows installer carries version information and a manifest, and the
  release lists SHA-256 checksums, which should make antivirus false
  positives less likely (#33; it is still not code-signed).

## [0.2.0-alpha.1] - 2026-10-20

### Added
- An installer for Windows and Linux (recomp-launcher): it finds GOG's
  install, or takes the 1994 CD's drive or GOG's offline installer, and
  builds the recomp from it on the player's machine. Nothing of the game is
  in it. Each release carries both.
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
- Music packs (#12): the game's songs replaced by recordings, such as the
  PlayStation version's CD tracks. A pack is a folder, `music\NAME\`, of
  Ogg Vorbis or WAV files named after the songs (RIDE1 ... RIDE17, FANFARE,
  INTRO...; music/README.md); songs it lacks stay MIDI. Picked in the
  settings menu (F10, *Music*), with `--music NAME` or `[sound] music=`, and
  a change is heard at once. The game's music volume applies.
- The network game Bullfrog left in (#2): 2 to 4 players over TCP,
  `--host N` and `--join ADDR`. The game's own NetBIOS protocol runs
  unchanged; `net.c` carries its sessions. Each player runs their own park
  in a shared world, in step frame by frame, and Ctrl+T sends thugs to the
  others. Headless with 2, 3 and 4 players, every machine's shared state is
  identical after each sync, and thugs arrive in every other park. How it
  works: docs/multiplayer.md.
- Dropping out and back in: every machine checkpoints every 30 s; when one
  crashes or loses its link the others wait, and when it is back (`--join`
  again, or `--host N --resume` for the host) everyone goes back to the
  newest checkpoint they all have.
- House rules, from F10's *Network game*: give money to another player,
  share your research with them, and as the host grant or fine.
- *Network game* in the settings menu: host, join (asks for the address)
  and resume, without the command line. Loading a save state is off in a
  network game.

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
