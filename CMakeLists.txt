cmake_minimum_required(VERSION 3.20)
project(themepark C)

# pcrecomp checkout: the lifter's runtime header and the dos32 host.
set(PCRECOMP ${CMAKE_SOURCE_DIR}/../tools CACHE PATH "pcrecomp checkout")
if(NOT EXISTS ${PCRECOMP}/runtime/dos32/dos32.c)
  message(FATAL_ERROR "pcrecomp at ${PCRECOMP} has no runtime/dos32 (needs pcrecomp with the LE front end); set -DPCRECOMP=")
endif()

file(GLOB GEN ${CMAKE_SOURCE_DIR}/src/recomp/gen/recomp_*.c)
if(NOT GEN)
  message(FATAL_ERROR "src/recomp/gen is empty: run `py -3 run_lift.py` first")
endif()

# Static CRT, so the exe runs on a machine with no VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

add_executable(themepark
  ${GEN}
  src/recomp/gen/image_data.c
  ${PCRECOMP}/runtime/dos32/dos32.c
  src/runtime/main.c
  src/runtime/present.c
  src/runtime/present_d3d.c
  src/runtime/game.c)
target_include_directories(themepark PRIVATE
  ${PCRECOMP}/runtime/recomp32 ${PCRECOMP}/runtime/dos32 src/recomp/gen src/runtime)
target_compile_definitions(themepark PRIVATE _CRT_SECURE_NO_WARNINGS RECOMP_YIELD_EVERY=256u)

# The lifted C: no warnings (there are hundreds of thousands of harmless ones),
# and wrap-around arithmetic, as the CPU does it.
set_source_files_properties(${GEN} src/recomp/gen/image_data.c PROPERTIES COMPILE_OPTIONS "/w;-fno-strict-aliasing;-fwrapv;/O1")
target_link_libraries(themepark PRIVATE winmm user32 gdi32 d3d11 dxgi dxguid)
