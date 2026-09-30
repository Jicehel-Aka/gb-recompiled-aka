# ============================================================================
# Frontend PC (SDL2) : compile gb_recompiled_pc avec le même code d'émulation que la console.
#
# Linux : cmake -S pc -B build-pc && cmake --build build-pc   (libsdl2-dev requis)   Windows : MSYS2/MinGW, voir .github/workflows/build-pc.yml
# Projet : gb-recompiled-aka (Gamebuino AKA). Auteur : Jicehel. Voir README.md.
# ============================================================================
cmake_minimum_required(VERSION 3.16)
project(gb_recompiled_pc C)

set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(GBRT_DIR ${CMAKE_CURRENT_LIST_DIR}/../components/gbrt_aka)
file(GLOB GBRT_RUNTIME_SRCS ${GBRT_DIR}/runtime/src/*.c)

find_package(SDL2 REQUIRED)

add_executable(gb_recompiled_pc
  main_pc.c
  pc_ui.c
  ${GBRT_DIR}/src/gbrt_aka.c
  ${GBRT_DIR}/src/gbrt_browser.c
  ${GBRT_DIR}/src/gbrt_zip.c
  ${GBRT_RUNTIME_SRCS})

target_include_directories(gb_recompiled_pc PRIVATE
  ${GBRT_DIR}/include ${GBRT_DIR}/runtime/include)

if(TARGET SDL2::SDL2)
  target_link_libraries(gb_recompiled_pc PRIVATE SDL2::SDL2)
else()
  target_include_directories(gb_recompiled_pc PRIVATE ${SDL2_INCLUDE_DIRS})
  target_link_libraries(gb_recompiled_pc PRIVATE ${SDL2_LIBRARIES})
endif()

if(NOT MSVC)
  target_compile_options(gb_recompiled_pc PRIVATE -O2 -Wno-unused-function)
  target_link_libraries(gb_recompiled_pc PRIVATE m)
endif()
if(MINGW)
  target_link_options(gb_recompiled_pc PRIVATE -static-libgcc)
endif()
