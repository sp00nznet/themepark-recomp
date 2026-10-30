cmake_minimum_required(VERSION 3.20)
project(themepark C)

# pcrecomp checkout: the lifter's runtime header and the dos32 host.
set(PCRECOMP ${CMAKE_SOURCE_DIR}/../tools CACHE PATH "pcrecomp checkout")
if(NOT EXISTS ${PCRECOMP}/runtime/dos32/dos32.c)
  message(FATAL_ERROR "pcrecomp at ${PCRECOMP} has no runtime/dos32 (needs pcrecomp with the LE front end); set -DPCRECOMP=")
endif()

file(STRINGS ${PCRECOMP}/runtime/dos32/dos32.h HAS_STATES REGEX "dos32_state_save")
if(NOT HAS_STATES)
  message(FATAL_ERROR "pcrecomp at ${PCRECOMP} is older than this game needs (pcrecomp#77): git -C ${PCRECOMP} pull")
endif()

file(STRINGS ${PCRECOMP}/runtime/dos32/dos32.h HAS_VESA_CUSTOM REGEX "dos32_vesa_custom")
if(NOT HAS_VESA_CUSTOM)
  message(FATAL_ERROR "pcrecomp at ${PCRECOMP} is older than this game needs (dos32_vesa_custom, pcrecomp#123): git -C ${PCRECOMP} pull, or run Setup again: it fetches a current one")
endif()

file(STRINGS ${PCRECOMP}/runtime/dos32/dos32.c HAS_POSIX REGEX "fix_case")
if(NOT WIN32 AND NOT HAS_POSIX)
  message(FATAL_ERROR "pcrecomp at ${PCRECOMP} is older than this game needs on Linux (pcrecomp#84): git -C ${PCRECOMP} pull")
endif()

file(GLOB GEN ${CMAKE_SOURCE_DIR}/src/recomp/gen/recomp_*.c)
if(NOT GEN)
  message(FATAL_ERROR "src/recomp/gen is empty: run `py -3 run_lift.py` first")
endif()

# The opening film (INTRO.EXE, `run_lift.py --intro`): linked in beside the
# game when it has been lifted and pcrecomp can run two programs (pcrecomp#122).
file(GLOB GEN_INTRO ${CMAKE_SOURCE_DIR}/src/recomp/gen_intro/recomp_*.c)
file(STRINGS ${PCRECOMP}/runtime/dos32/dos32.h HAS_RERUN REGEX "dispatch_count")
if(GEN_INTRO AND HAS_RERUN)
  list(APPEND GEN ${GEN_INTRO} ${CMAKE_SOURCE_DIR}/src/recomp/gen_intro/image_data.c)
  set(INTRO ON)
elseif(GEN_INTRO)
  message(WARNING "pcrecomp at ${PCRECOMP} cannot run the intro before the game (pcrecomp#122): building without it")
endif()

set(HOST
  ${PCRECOMP}/runtime/dos32/dos32.c
  src/runtime/main.c
  src/runtime/game.c
  src/runtime/audio.c
  src/runtime/sound.c
  src/runtime/music.c
  src/runtime/net.c
  src/runtime/net_rules.c
  src/runtime/rules.c
  src/runtime/pad.c)

if(WIN32)
  # Static CRT, so the exe runs on a machine with no VC++ redistributable.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
  add_executable(themepark ${GEN} src/recomp/gen/image_data.c ${HOST}
    src/runtime/present.c src/runtime/present_d3d.c)
  target_compile_definitions(themepark PRIVATE _CRT_SECURE_NO_WARNINGS)
  # The lifted C: no warnings (there are hundreds of thousands of harmless ones),
  # and wrap-around arithmetic, as the CPU does it.
  set_source_files_properties(${GEN} src/recomp/gen/image_data.c PROPERTIES COMPILE_OPTIONS "/w;-fno-strict-aliasing;-fwrapv;/O1")
  target_link_libraries(themepark PRIVATE winmm user32 gdi32 d3d11 dxgi dxguid ws2_32)
elseif(NINTENDO_3DS)
  # The 3DS (devkitARM's 3DS.cmake): libctru straight, no SDL (present_3ds.c,
  # audio.c's ndsp). 24 MB of guest memory; the game folder is read from
  # the SD card, beside the .3dsx (sdmc:/3ds/themepark/game).
  list(REMOVE_ITEM HOST src/runtime/pad.c)
  add_executable(themepark ${GEN} src/recomp/gen/image_data.c ${HOST}
    src/runtime/present_3ds.c src/runtime/compat.c)
  set_source_files_properties(${GEN} src/recomp/gen/image_data.c PROPERTIES COMPILE_OPTIONS "-w;-fno-strict-aliasing;-fwrapv;-O1;-foptimize-sibling-calls")
  target_compile_definitions(themepark PRIVATE DOS32_SMALL_HOST DOS32_MEM=0x01800000u)
  target_link_libraries(themepark PRIVATE ctru m)
  ctr_generate_smdh(themepark.smdh NAME "Theme Park" DESCRIPTION "Bullfrog, 1994; recompiled" AUTHOR "pcrecomp")
  ctr_create_3dsx(themepark SMDH themepark.smdh)
elseif(EMSCRIPTEN)
  # The browser (emcmake cmake): Emscripten's SDL2, the game thread a worker
  # (so the page must be served cross-origin isolated), guest memory in one
  # block (DOS32_SMALL_HOST), and the game folder packed beside the wasm:
  # -DTP_GAME_DIR=game (its GAME and THEME.CD). No music (it needs a MIDI synth) and no network.
  file(STRINGS ${PCRECOMP}/runtime/recomp32/recomp_types.h HAS_TAIL REGEX "RECOMP_TAIL_CALL")
  file(STRINGS ${PCRECOMP}/runtime/dos32/dos32.c HAS_SMALL REGEX "DOS32_SMALL_HOST")
  if(NOT HAS_TAIL OR NOT HAS_SMALL)
    message(FATAL_ERROR "pcrecomp at ${PCRECOMP} cannot build for the browser (pcrecomp#157): git -C ${PCRECOMP} pull")
  endif()
  set(TP_GAME_DIR ${CMAKE_SOURCE_DIR}/game CACHE PATH "the game folder (GAME and THEME.CD) to pack")
  set(CMAKE_EXECUTABLE_SUFFIX .html)
  add_executable(themepark ${GEN} src/recomp/gen/image_data.c ${HOST}
    src/runtime/present_sdl.c src/runtime/compat.c)
  set_source_files_properties(${GEN} src/recomp/gen/image_data.c PROPERTIES COMPILE_OPTIONS "-w;-fno-strict-aliasing;-fwrapv;-O1")
  target_compile_options(themepark PRIVATE -sUSE_SDL=2 -pthread -mtail-call)
  target_compile_definitions(themepark PRIVATE DOS32_SMALL_HOST)
  target_link_options(themepark PRIVATE -sUSE_SDL=2 -pthread -mtail-call -sPTHREAD_POOL_SIZE=4
    -sINITIAL_MEMORY=512MB -sSTACK_SIZE=4MB -sEXIT_RUNTIME=0
    "SHELL:--preload-file ${TP_GAME_DIR}/GAME@/game/GAME" "SHELL:--preload-file ${TP_GAME_DIR}/THEME.CD@/game/THEME.CD")
else()
  # Linux: gcc (or clang) and SDL2 for the window, sound and pad; ALSA's
  # sequencer, if its headers are there, for the music (sound.c).
  if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
  endif()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
  pkg_check_modules(ALSA IMPORTED_TARGET alsa)
  add_executable(themepark ${GEN} src/recomp/gen/image_data.c ${HOST}
    src/runtime/present_sdl.c src/runtime/compat.c)
  set_source_files_properties(${GEN} src/recomp/gen/image_data.c PROPERTIES COMPILE_OPTIONS "-w;-fno-strict-aliasing;-fwrapv;-O1;-foptimize-sibling-calls")
  target_link_libraries(themepark PRIVATE PkgConfig::SDL2 m pthread)
  if(ALSA_FOUND)
    target_compile_definitions(themepark PRIVATE HAVE_ALSA)
    target_link_libraries(themepark PRIVATE PkgConfig::ALSA)
  else()
    message(STATUS "ALSA's headers not found (libasound2-dev): the build will play no music")
  endif()
endif()

target_include_directories(themepark PRIVATE
  ${PCRECOMP}/runtime/recomp32 ${PCRECOMP}/runtime/dos32 src/recomp/gen src/runtime)
target_compile_definitions(themepark PRIVATE RECOMP_YIELD_EVERY=256u)
if(INTRO)
  target_compile_definitions(themepark PRIVATE HAVE_INTRO)
endif()

# A debugging build (-DTP_TRACE=ON, in a build dir of its own): every lifted
# function counts its entries, and TP_TRACE=T0,T1 prints which ran between
# those guest seconds (src/runtime/trace.c). Slower; never for release.
option(TP_TRACE "count lifted function entries (debugging)" OFF)
if(TP_TRACE)
  target_sources(themepark PRIVATE src/runtime/trace.c)
  target_compile_definitions(themepark PRIVATE RECOMP_TRACE)
endif()
