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
- **The network game, by hand** (#2): it runs over TCP for 2-4 players,
  stays in sync, survives a crash, and has house rules, all tested headless
  (docs/multiplayer.md). Still to do: the menu and two windows by hand, a
  LAN, Windows with Linux, a long game, a drop during the year end, the
  stock market. Maybe: loans between players with repayments, a chat line,
  a way to carry on without a player who is not coming back.
- **Linux, played by hand**: the window, captured mouse, dialogs and pad have
  only run headless or briefly; music needs a synth on ALSA, so a bundled
  soundfont would help here most of all.

## Housekeeping
- netlab recipe and QA run on the test VMs.
- Long-play soak: a year of park time with no fault.

## Out of scope
- Distributing the game, its data, or the lifted C.
- The SNES, Mega Drive, 3DO, Saturn, PlayStation and Amiga versions.
