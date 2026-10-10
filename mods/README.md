# Mods

A mod is a folder here laid over the game folder: same layout as `game\`, and
any file it has replaces the game's when the game reads it. Files it doesn't
have come from the game as usual, and nothing in `game\` is ever changed.
Saves still go to `game\`.

```
mods\
  bigger-signs\
    GAME\DATA\LANG0-0.DAT        replaces game\GAME\DATA\LANG0-0.DAT
    THEME.CD\DATA\MSPR-0.DAT     replaces game\THEME.CD\DATA\MSPR-0.DAT
```

Play with one: `themepark.exe --mod bigger-signs`, or `mod=bigger-signs` under
`[game]` in `themepark.ini`. `--mod` also takes a full path.

Folder and file names are not case-sensitive. The game reads its data from
both `GAME\DATA` and `THEME.CD\DATA`; a mod must put a file where the game
looks for it (`--log` prints every file the game opens, and `(overlay)` marks
the ones a mod supplied).

Mods built from the game's own files are the game's material: share the tool
or the changes, not the files.
