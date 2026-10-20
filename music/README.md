# Music packs

A music pack replaces the game's MIDI songs with recordings: the PlayStation
or other CD versions' tracks, re-recorded MIDI on a better synth, the Mega
Drive or SNES music, anything. A pack is a folder here of files named after
the game's songs:

```
music\
  psx\
    RIDE1.ogg
    RIDE2.ogg
    ...
    FANFARE.wav
```

Pick one in the settings menu (F10, *Music*), with `themepark.exe --music psx`,
or with `music=psx` under `[sound]` in `themepark.ini`. *The game's own (MIDI)*
goes back. A change takes effect at once: the song playing ends, and the
game's next one comes from the new pack.

Any song the pack has no file for plays as the game's own MIDI, so a pack can
replace one song or all of them.

## The songs

The game plays a short tune while a ride or shop is shown, and others at
moments in the game. The names are the game's own (from `MUSIC0` and `MUSIC1`
in `GAME\DATA`):

| File | When |
|---|---|
| `RIDE1` ... `RIDE17` | a ride or shop's window is open, one tune per ride type |
| `HRTBEAT`, `FANFARE`, `TOVER`, `WIN2`, `FUNERAL` | moments their names suggest (a takeover, an award, winning, going bust); not yet all seen in play |
| `INTRO` ... `INTRO5` | the opening film's five parts (with the intro, PR #30) |

The game decides when a song ends by asking whether it is still playing, so
a recording can be any length; when it ends, the game plays its next song
(or the same one again) as it would after the MIDI.

## Formats

- **Ogg Vorbis** (`.ogg`)
- **WAV**: PCM, 16 or 8-bit (`.wav`)

Any sample rate, mono or stereo. Names are not case-sensitive. For anything
else (FLAC, MP3, a CD rip), convert with ffmpeg:

```
ffmpeg -i track02.flac -c:a libvorbis -q:a 6 RIDE1.ogg
```

The music volume in the game's options applies to a pack too. Packs are your
own files: share the pack's track list or tools, not ripped music.
