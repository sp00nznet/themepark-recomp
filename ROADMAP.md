# Roadmap

## Next
- **Long play**: a year of park time scripted, with no fault and saves loading.
- **A volume slider** in the settings menu.

## Then: the remaster
- **A wider park view**: 16:9 beyond the game's 640x480, if its renderer allows.
- **A bundled General MIDI soundfont** option, for machines whose Windows synth
  sounds thin.
- **Quick-save keys and autosave** on top of the save states.
- **Fixes to the original's own bugs** as they turn up, each documented.
- **The network game, by hand** (#2): it runs over TCP for 2-4 players and
  stays in sync headless (docs/multiplayer.md). Still to do: play it at
  windows and over a LAN, a long game, the stock market and the other
  shared commands, and Windows with Linux. Then a gentler exit when a
  player leaves (today the game just closes), and a way to host and join
  from the settings menu.
- **Linux, played by hand**: the window, captured mouse, dialogs and pad have
  only run headless or briefly; music needs a synth on ALSA, so a bundled
  soundfont would help here most of all.

## Housekeeping
- netlab recipe and QA run on the test VMs.
- Long-play soak: a year of park time with no fault.

## Out of scope
- Distributing the game, its data, or the lifted C.
- The SNES, Mega Drive, 3DO, Saturn, PlayStation and Amiga versions.
