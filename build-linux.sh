#!/bin/sh
# Configure and build the native Linux program, build-linux/themepark: the
# lifted C (src/recomp/gen) on pcrecomp's DOS host, with SDL2 for the window,
# sound and pad. setup.sh does this and the lift; this rebuilds after a change.
#
#   ./build-linux.sh               # then: build-linux/themepark
#
# Needs gcc, cmake, SDL2's development files (and ALSA's, for music; setup.sh
# lists the packages) and the toolkit: PCRECOMP, else ../tools.
set -e
cd "$(dirname "$0")"
cmake -B build-linux -DCMAKE_BUILD_TYPE=Release ${PCRECOMP:+"-DPCRECOMP=$PCRECOMP"}
cmake --build build-linux -j "$(nproc)"
