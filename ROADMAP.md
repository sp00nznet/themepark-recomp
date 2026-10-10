# Roadmap

## Next
- **Sound.** Replace the HMI SOS layer (the game loads `HMIDRV.386` drivers at
  run time and calls into them) at its API: digital effects through a host
  mixer, the music through Windows MIDI or a bundled synth.
- **The presenter.** Our own Direct3D 11 window, as in redalert2-recomp and
  gunman: sharp-bilinear / smooth / CRT / nearest / integer scaling (F12),
  borderless fullscreen, vsync, and settings kept in `themepark.ini`.

## Then: the remaster
- **Speed control**: the frame rate as a slider, 25% to 400%, from the menu.
- **High resolution**: the game's own 640x480 SVGA mode first; then a wider
  park view if the renderer allows it.
- **Quick-save and autosave** slots that answer the game's own save screens.
- **Mods**: a `mods\` folder laid over the game's data files.
- **Controller** support mapped onto the mouse and keys.
- **Fixes to the original's own bugs** as they turn up, each documented.
- **Linux** build of the host (dos32 is Win32 today).

## Housekeeping
- netlab recipe and QA run on the test VMs.
- Long-play soak: a year of park time with no fault.

## Out of scope
- Distributing the game, its data, or the lifted C.
- The SNES, Mega Drive, 3DO, Saturn, PlayStation and Amiga versions.
