#!/bin/bash
# Linux and macOS setup: the same steps as Setup.cmd (tools/setup.ps1), for a
# native program -- no Wine, no DOSBox.
#
#   1. check the tools (gcc, CMake, Python 3 with capstone and pefile, git,
#      SDL2), and say which packages to install when one is missing
#   2. copy your copy of Theme Park into game/: an installed copy (GOG's, or
#      any folder holding GAME/MAIN.EXE), the 1994 CD (mounted), or GOG's offline
#      installer, which it unpacks with innoextract (nothing is run)
#   3. fetch the pcrecomp toolkit beside this folder, as ../pcrecomp
#   4. lift MAIN.EXE and INTRO.EXE to C (run_lift.py), then build build-linux/themepark
#   5. leave a launcher here: themepark.sh
#
#   ./setup.sh                      # look for the game
#   ./setup.sh ~/GOG\ Games/Theme\ Park   # the folder, or GOG's setup_theme_park_*.exe
#   ./setup.sh --yes --game-dir DIR  # as the recomp launcher runs it (it asks nothing either way)
#   ./setup.sh --targets web,3ds     # also the browser and 3DS builds, in Docker (builds/web, builds/3ds)
#
# Every step is skipped when its result is already there, so running it
# again carries on from where it stopped.
set -e
cd "$(dirname "$0")"
ROOT=$PWD
GAME_ARG=
TARGETS=
while [ $# -gt 0 ]; do
  case $1 in
    --yes) ;;                                  # nothing here asks
    --game-dir) GAME_ARG=$2; shift ;;
    --targets) TARGETS=$(printf '%s' "$2" | tr ',' ' '); shift ;;
    *) GAME_ARG=$1 ;;
  esac
  shift
done
set -- ${GAME_ARG:+"$GAME_ARG"}
# The toolkit: PCRECOMP, else ../pcrecomp (where the launcher puts it), else
# ../tools (older setups), else ../pcrecomp is cloned.
PARENT=$(cd .. && pwd)
if [ -n "$PCRECOMP" ]; then TOOLKIT=$PCRECOMP
elif [ ! -d "$PARENT/pcrecomp" ] && [ -d "$PARENT/tools/runtime/dos32" ]; then TOOLKIT=$PARENT/tools
else TOOLKIT=$PARENT/pcrecomp; fi
PCRECOMP_URL=https://github.com/sp00nznet/pcrecomp

N=5; [ -n "$TARGETS" ] && N=6
say()  { printf '%s\n' "$*"; }
step() { printf '\n\033[36m%s\033[0m\n' "$*"; }
fail() { printf '\n\033[31mSetup stopped: %s\033[0m\n' "$*" >&2; exit 1; }

# ---- 1. tools -------------------------------------------------------------------
step "[1/$N] Checking the tools"
if [ "$(uname)" = Darwin ]; then   # Xcode's command line tools and Homebrew; what is missing from brew is installed
  xcode-select -p >/dev/null 2>&1 || fail "Xcode's command line tools are missing: run xcode-select --install, then this again."
  command -v brew >/dev/null || fail "Homebrew is missing: install it from https://brew.sh, then run this again."
  for f in cmake pkg-config sdl2 innoextract python; do
    brew list --formula "$f" >/dev/null 2>&1 || { say "  brew install $f"; brew install --quiet "$f" || fail "brew could not install $f."; }
  done
fi
need=()
for t in gcc cmake python3 git pkg-config; do command -v "$t" >/dev/null || need+=("$t"); done
command -v pkg-config >/dev/null && ! pkg-config --exists sdl2 && need+=("SDL2's development files")
if [ ${#need[@]} -gt 0 ]; then
  say "  Missing: ${need[*]}. From your package manager, for example:"
  say "    Debian, Ubuntu: sudo apt install gcc cmake python3 python3-pip git pkg-config libsdl2-dev libasound2-dev innoextract"
  say "    Fedora:         sudo dnf install gcc cmake python3 python3-pip git pkgconf sdl2-compat-devel alsa-lib-devel innoextract"
  say "    Arch:           sudo pacman -S gcc cmake python python-pip git pkgconf sdl2 alsa-lib innoextract"
  say "    macOS:          xcode-select --install; brew install cmake pkg-config sdl2 innoextract"
  say "  (innoextract only to unpack GOG's installer; ffmpeg only for --record.)"
  fail "install them, then run ./setup.sh again."
fi
[ "$(uname)" = Darwin ] || pkg-config --exists alsa || say "  No ALSA headers (libasound2-dev / alsa-lib-devel): it will build, with no music."
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

step "[2/$N] Copying your copy of Theme Park into game/"
if [ -n "$(game_in game)" ]; then
  say "  already in game/"
else
  src=
  inst=
  if [ -n "$1" ]; then
    if [ -d "$1" ]; then
      src=$(game_in "$1")
      [ -n "$src" ] || inst=$(ls "$1"/setup_theme_park_*.exe 2>/dev/null | head -1)   # the folder GOG's installer is in
    else inst=$1; fi
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
step "[3/$N] The pcrecomp toolkit"
if [ ! -f "$TOOLKIT/tools/lift/lift32.py" ]; then
  say "  fetching it into $TOOLKIT"
  git clone --quiet --depth 1 "$PCRECOMP_URL" "$TOOLKIT"
fi
# The newest toolkit feature the game uses (pcrecomp#123); CMakeLists.txt checks the same.
current() { grep -q dos32_vesa_custom "$1/runtime/dos32/dos32.h" 2>/dev/null && grep -q fix_case "$1/runtime/dos32/dos32.c"; }
if ! current "$TOOLKIT"; then
  [ -n "$PCRECOMP" ] && fail "your pcrecomp at $TOOLKIT (PCRECOMP) is older than this game needs: git -C \"$TOOLKIT\" pull"
  if [ "$TOOLKIT" = "$PARENT/pcrecomp" ]; then
    say "  $TOOLKIT is older than this game needs: updating it (git pull)"
    git -C "$TOOLKIT" pull --quiet --ff-only || fail "could not update $TOOLKIT: git -C \"$TOOLKIT\" pull"
  else   # an older setup's ../tools: left as it is, a current one beside it
    say "  $TOOLKIT is older than this game needs: fetching a current one into $PARENT/pcrecomp (the old one is not touched)"
    git clone --quiet --depth 1 "$PCRECOMP_URL" "$PARENT/pcrecomp"
    TOOLKIT=$PARENT/pcrecomp
  fi
  current "$TOOLKIT" || fail "pcrecomp at $TOOLKIT is still older than this game needs: git -C \"$TOOLKIT\" pull"
fi
say "  $TOOLKIT"
export PCRECOMP=$TOOLKIT

# ---- 4. lift + build ------------------------------------------------------------
step "[4/$N] Lifting MAIN.EXE to C, then building (the first lift finds the functions: a few minutes)"
# Every time: lifted C from an older run_lift.py would not match this code (#37). The slow part,
# finding the functions, is kept in analysis/, so a lift again takes seconds.
python3 run_lift.py --exe "$main_exe" || fail "the lift failed."
intro_exe=$(find "$(dirname "$main_exe")" -maxdepth 1 -iname intro.exe | head -n 1)   # the opening film
[ -n "$intro_exe" ] && { python3 run_lift.py --intro --exe "$intro_exe" || fail "the intro's lift failed."; }
cmake -B build-linux -DCMAKE_BUILD_TYPE=Release "-DPCRECOMP=$TOOLKIT" >/dev/null || fail "CMake could not configure the build."
cmake --build build-linux -j "$(nproc 2>/dev/null || sysctl -n hw.ncpu)" || fail "the build failed."
[ -x build-linux/themepark ] || fail "the build finished but build-linux/themepark is missing."

# ---- 5. launcher ----------------------------------------------------------------
printf '#!/bin/sh\ncd "$(dirname "$0")" && exec build-linux/themepark "$@"\n' > themepark.sh
chmod +x themepark.sh
if [ "$(uname)" = Darwin ]; then   # Finder opens a .command in Terminal; a .sh in an editor
  printf '#!/bin/sh
cd "$(dirname "$0")" && exec build-linux/themepark "$@"
' > "Theme Park (recomp).command"
  chmod +x "Theme Park (recomp).command"
fi
step "[5/$N] Ready: ./themepark.sh"
say "Click in the picture to play; F10 for the settings menu (save states, window size, scaling...)."
[ "$(uname)" = Darwin ] || say "Music needs a General MIDI synth on ALSA's sequencer: FluidSynth or TiMidity++ running as a server."

# ---- 6. the browser and the 3DS, if asked for -----------------------------------
# Each in its toolchain's Docker image (pulled the first time only), so nothing
# more is installed here. The lifted C from step 4 is built again for each.
[ -n "$TARGETS" ] || exit 0
step "[6/$N] Other builds: $TARGETS (in Docker; the first pulls its toolchain, 2 to 4 GB)"
command -v docker >/dev/null || fail "these need Docker (https://docs.docker.com/get-docker/); the build for this computer is ready."
docker info >/dev/null 2>&1 || fail "Docker is not running: start it, then run this again (finished steps are quick)."
dock() { docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -e EM_CACHE=/src/builds/.web/.emcache -v "$ROOT:/src" -v "$TOOLKIT:/pcrecomp" -w /src "$1" sh -c "$2"; }
for t in $TARGETS; do
  case $t in
    web)
      say "  the browser (emscripten/emsdk)"
      dock emscripten/emsdk 'for i in 1 2 3; do embuilder build sdl2 >/dev/null && break; done; emcmake cmake -B builds/.web -DPCRECOMP=/pcrecomp >/dev/null && cmake --build builds/.web -j8' || fail "the browser build failed."
      mkdir -p builds/web
      cp builds/.web/themepark.* builds/web/ && cp tools/serve_wasm.py builds/web/
      printf '#!/bin/sh
# Serves the browser build to this computer only (your game is packed in it).
cd "$(dirname "$0")/builds/web" && (sleep 1; xdg-open http://localhost:8000/themepark.html 2>/dev/null || open http://localhost:8000/themepark.html) & exec python3 serve_wasm.py . 8000
' > "Theme Park (browser).sh"
      chmod +x "Theme Park (browser).sh"
      say "  builds/web: ./\"Theme Park (browser).sh\" and open http://localhost:8000/themepark.html in Chrome" ;;
    3ds)
      say "  the 3DS (devkitpro/devkitarm)"
      dock devkitpro/devkitarm 'cmake -B builds/.3ds -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/3DS.cmake -DPCRECOMP=/pcrecomp >/dev/null && cmake --build builds/.3ds -j8' || fail "the 3DS build failed."
      mkdir -p builds/3ds/themepark
      cp builds/.3ds/themepark.3dsx builds/3ds/themepark/ && cp -R game builds/3ds/themepark/
      say "  builds/3ds/themepark: copy that folder to the SD card as sdmc:/3ds/themepark" ;;
    *) say "  $t: not a target here (web, 3ds)" ;;
  esac
done
say "Done."
