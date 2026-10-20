#!/bin/bash
# Linux setup: the same steps as Setup.cmd (tools/setup.ps1), for a native
# Linux program -- no Wine, no DOSBox.
#
#   1. check the tools (gcc, CMake, Python 3 with capstone and pefile, git,
#      SDL2), and say which packages to install when one is missing
#   2. copy your copy of Theme Park into game/: an installed copy (GOG's, or
#      any folder holding GAME/MAIN.EXE), the 1994 CD (mounted), or GOG's offline
#      installer, which it unpacks with innoextract (nothing is run)
#   3. fetch the pcrecomp toolkit beside this folder, as ../tools
#   4. lift MAIN.EXE and INTRO.EXE to C (run_lift.py), then build build-linux/themepark
#   5. leave a launcher here: themepark.sh
#
#   ./setup.sh                      # look for the game
#   ./setup.sh ~/GOG\ Games/Theme\ Park   # the folder, or GOG's setup_theme_park_*.exe
#
# Every step is skipped when its result is already there, so running it
# again carries on from where it stopped.
set -e
cd "$(dirname "$0")"
ROOT=$PWD
TOOLKIT=${PCRECOMP:-$(cd .. && pwd)/tools}
PCRECOMP_URL=https://github.com/sp00nznet/pcrecomp

say()  { printf '%s\n' "$*"; }
step() { printf '\n\033[36m%s\033[0m\n' "$*"; }
fail() { printf '\n\033[31mSetup stopped: %s\033[0m\n' "$*" >&2; exit 1; }

# ---- 1. tools -------------------------------------------------------------------
step "[1/5] Checking the tools"
need=()
for t in gcc cmake python3 git pkg-config; do command -v "$t" >/dev/null || need+=("$t"); done
command -v pkg-config >/dev/null && ! pkg-config --exists sdl2 && need+=("SDL2's development files")
if [ ${#need[@]} -gt 0 ]; then
  say "  Missing: ${need[*]}. From your package manager, for example:"
  say "    Debian, Ubuntu: sudo apt install gcc cmake python3 python3-pip git pkg-config libsdl2-dev libasound2-dev innoextract"
  say "    Fedora:         sudo dnf install gcc cmake python3 python3-pip git pkgconf sdl2-compat-devel alsa-lib-devel innoextract"
  say "    Arch:           sudo pacman -S gcc cmake python python-pip git pkgconf sdl2 alsa-lib innoextract"
  say "  (innoextract only to unpack GOG's installer; ffmpeg only for --record.)"
  fail "install them, then run ./setup.sh again."
fi
pkg-config --exists alsa || say "  No ALSA headers (libasound2-dev / alsa-lib-devel): it will build, with no music."
if ! python3 -c 'import capstone, pefile' 2>/dev/null; then
  say "  The Python packages capstone and pefile are missing; installing them for your user (pip)."
  python3 -m pip install --user --quiet capstone pefile 2>/dev/null ||
    python3 -m pip install --user --quiet --break-system-packages capstone pefile ||
    fail "pip could not install them. Install python3-capstone and python3-pefile from your package manager, then run ./setup.sh again."
fi
say "  ok"

# ---- 2. the game ----------------------------------------------------------------
# The folder under $1 holding GAME/MAIN.EXE and GAME/DATA, any case: GOG's
# install, a 1994 install, or the 1994 CD itself (no THEME.CD/ there).
game_in() {
  find "$1" -ipath '*/GAME/MAIN.EXE' 2>/dev/null | while read -r f; do
    d=$(dirname "$(dirname "$f")")
    if [ -n "$(find "$d" -maxdepth 2 -ipath "$d/GAME/DATA")" ]; then echo "$d"; break; fi
  done
}

step "[2/5] Copying your copy of Theme Park into game/"
if [ -n "$(game_in game)" ]; then
  say "  already in game/"
else
  src=
  inst=
  if [ -n "$1" ]; then
    if [ -d "$1" ]; then src=$(game_in "$1"); else inst=$1; fi
  else
    for d in "$HOME/GOG Games" "$HOME/Games" "$HOME/games"; do
      [ -d "$d" ] && src=$(game_in "$d") && [ -n "$src" ] && break
    done
    [ -n "$src" ] || inst=$(ls "$HOME"/Downloads/setup_theme_park_*.exe "$ROOT"/setup_theme_park_*.exe 2>/dev/null | head -1)
  fi
  if [ -z "$src" ] && [ -n "$inst" ]; then
    say "  GOG's installer: $inst"
    command -v innoextract >/dev/null || fail "innoextract unpacks it; install it (see above), then run ./setup.sh again."
    say "  unpacking it into game/ (about 200 MB; nothing is run)"
    innoextract --silent --output-dir game "$inst"
  elif [ -n "$src" ]; then
    say "  copying $src (about 180 MB)"
    mkdir -p game
    cp -r "$src"/. game/
  else
    fail "Theme Park was not found. Run ./setup.sh /the/folder/holding/GAME (the mounted CD, or GOG's setup_theme_park_*.exe)."
  fi
  [ -n "$(game_in game)" ] || fail "game/ has no GAME/MAIN.EXE after copying. Is that Theme Park (GOG, or the 1994 CD)?"
  chmod -R u+w game   # files copied off a CD are read-only
  # From the CD: do what its installer did, minus the copy of GAME/. These 32
  # are the files it put in C:\THEME.CD\DATA; the game reads them from there
  # and nowhere else. GOG's THEME.CD/DATA is the same 32.
  if [ -z "$(find game -maxdepth 2 -ipath game/THEME.CD/DATA)" ]; then
    say "  that is the 1994 CD: setting up THEME.CD/ the way its installer did"
    gd=$(find game -maxdepth 2 -ipath game/GAME/DATA | head -1)
    mkdir -p game/THEME.CD/DATA
    for f in HPANEL-0.DAT HPANEL-0.TAB MAWAR0-0.DAT MAWAR1-0.DAT MAWPAL-0.DAT MBLK-0.DAT MBLK-0.TAB \
             MCUP-0.DAT MCUP-0.TAB MHAND-0.DAT MHAND-0.TAB MMAP-0.DAT MMENU-0.DAT MMENU-1.DAT MNEG-0.DAT \
             MNGPAL-0.DAT MPALETTE.DAT MPANEL-0.DAT MPANEL-0.TAB MRES-0.DAT MRSPAL-0.DAT MRSSPR-0.DAT \
             MRSSPR-0.TAB MSHARE-0.DAT MSPR-0.DAT MSPR-0.TAB MSTAP-0.DAT MSTATE-0.DAT MSTOCK-0.DAT \
             MSTPAL-0.DAT MSTSPR-0.DAT MSTSPR-0.TAB; do
      cp "$(find "$gd" -maxdepth 1 -iname "$f" | head -1)" game/THEME.CD/DATA/ || fail "the CD has no $f"
    done
    cp -r "$(find game -maxdepth 2 -ipath game/GAME/SAVE | head -1)" game/THEME.CD/SAVE
  fi
  # GOG keeps the sound setup beside its DOSBox config, not in THEME.CD.
  cd_dir=$(find game -maxdepth 1 -iname THEME.CD | head -1)
  snd=$(find game -iname SNDSETUP.INF | head -1)
  if [ -n "$snd" ] && [ -z "$(find "$cd_dir" -maxdepth 1 -iname SNDSETUP.INF)" ]; then cp "$snd" "$cd_dir/SNDSETUP.INF"; fi
  # The CD has none (its setup program writes it): GOG's settings, which the game's sound start-up needs.
  [ -n "$(find "$cd_dir" -maxdepth 1 -iname SNDSETUP.INF)" ] ||
    printf 'SOUNDFX = SB16 220 7 1\r\nMUSIC = GENERAL 330 0 0\r\n' > "$cd_dir/SNDSETUP.INF"
fi
main_exe=$(find game -ipath 'game/GAME/MAIN.EXE' | head -1)

# ---- 3. the toolkit -------------------------------------------------------------
step "[3/5] The pcrecomp toolkit"
if [ ! -f "$TOOLKIT/tools/lift/lift32.py" ]; then
  say "  fetching it into $TOOLKIT"
  git clone --quiet --depth 1 "$PCRECOMP_URL" "$TOOLKIT"
fi
grep -q dos32_state_save "$TOOLKIT/runtime/dos32/dos32.h" 2>/dev/null && grep -q fix_case "$TOOLKIT/runtime/dos32/dos32.c" ||
  fail "your pcrecomp at $TOOLKIT is older than this game needs on Linux: git -C \"$TOOLKIT\" pull"
say "  $TOOLKIT"
export PCRECOMP=$TOOLKIT

# ---- 4. lift + build ------------------------------------------------------------
step "[4/5] Lifting MAIN.EXE to C, then building (the first lift finds the functions: a few minutes)"
if [ -f src/recomp/gen/image_data.c ]; then
  say "  already lifted (src/recomp/gen)"
else
  python3 run_lift.py --exe "$main_exe" || fail "the lift failed."
fi
if [ ! -f src/recomp/gen_intro/image_data.c ]; then   # the opening film
  intro_exe=$(find "$(dirname "$main_exe")" -maxdepth 1 -iname intro.exe | head -n 1)
  [ -n "$intro_exe" ] && { python3 run_lift.py --intro --exe "$intro_exe" || fail "the intro's lift failed."; }
fi
cmake -B build-linux -DCMAKE_BUILD_TYPE=Release "-DPCRECOMP=$TOOLKIT" >/dev/null || fail "CMake could not configure the build."
cmake --build build-linux -j "$(nproc)" || fail "the build failed."
[ -x build-linux/themepark ] || fail "the build finished but build-linux/themepark is missing."

# ---- 5. launcher ----------------------------------------------------------------
printf '#!/bin/sh\ncd "$(dirname "$0")" && exec build-linux/themepark "$@"\n' > themepark.sh
chmod +x themepark.sh
step "[5/5] Ready: ./themepark.sh"
say "Click in the picture to play; F10 for the settings menu (save states, window size, scaling...)."
say "Music needs a General MIDI synth on ALSA's sequencer: FluidSynth or TiMidity++ running as a server."
