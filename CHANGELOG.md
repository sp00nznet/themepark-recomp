# Changelog

All notable changes. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
versions follow [SemVer](https://semver.org/).

## [Unreleased]

### Added
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
