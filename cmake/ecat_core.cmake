#
# frank-ecat — the eCat3 core (ecat3/src) for the RP2350
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# ecat_core: an OBJECT library of the upstream core with ECAT_EMBEDDED and
# ECAT_SINGLE_THREAD, from upstream's own source list
# (ecat3/src/emulator_sources.cmake) minus the host audio back-ends (the firmware
# has its own, ecat_create_audio_driver()) and the screen cache. Plus
# dsk_tools_core.
#

set(ECAT3_SRC ${CMAKE_CURRENT_LIST_DIR}/../ecat3/src)

include(${ECAT3_SRC}/emulator_sources.cmake)   # ECAT_CORE_SOURCES, absolute paths
set(ECAT_CORE_FW_SOURCES ${ECAT_CORE_SOURCES})
list(FILTER ECAT_CORE_FW_SOURCES INCLUDE REGEX "\\.(cpp|c)$")
list(FILTER ECAT_CORE_FW_SOURCES EXCLUDE REGEX
    "(audio_driver_miniaudio|audio_driver_sdl|miniaudio\\.c|/cache\\.cpp)")

set(DSK_TOOLS_DIR ${ECAT3_SRC}/libs/dsk_tools)
set(DSK_TOOLS_CORE_SOURCES
    ${DSK_TOOLS_DIR}/src/disk_codecs.cpp
    ${DSK_TOOLS_DIR}/src/hfe.cpp
    ${DSK_TOOLS_DIR}/src/track_formats.cpp
    ${DSK_TOOLS_DIR}/src/utils.cpp
    ${DSK_TOOLS_DIR}/src/errors.cpp
    ${DSK_TOOLS_DIR}/src/host_helpers.cpp
    ${DSK_TOOLS_DIR}/src/charmaps.cpp
)

# globals.h, as upstream configures it
file(STRINGS "${ECAT3_SRC}/../VERSION" ECAT3_VERSION LIMIT_COUNT 1)
string(STRIP "${ECAT3_VERSION}" ECAT3_VERSION)
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)" _v "${ECAT3_VERSION}")
set(PROJECT_NAME_SAVED ${PROJECT_NAME})
set(PROJECT_NAME "eCat3")
set(PROJECT_VERSION_MAJOR ${CMAKE_MATCH_1})
set(PROJECT_VERSION_MINOR ${CMAKE_MATCH_2})
set(PROJECT_VERSION_PATCH ${CMAKE_MATCH_3})
set(ECAT_VERSION ${ECAT3_VERSION})
configure_file(${ECAT3_SRC}/globals.h.in ${CMAKE_CURRENT_BINARY_DIR}/ecat3_gen/globals.h @ONLY NEWLINE_STYLE LF)
set(PROJECT_NAME ${PROJECT_NAME_SAVED})

add_library(ecat_core OBJECT ${ECAT_CORE_FW_SOURCES} ${DSK_TOOLS_CORE_SOURCES})
target_include_directories(ecat_core PUBLIC
    ${CMAKE_CURRENT_BINARY_DIR}/ecat3_gen
    ${ECAT3_SRC}
    ${DSK_TOOLS_DIR}/include
    ${DSK_TOOLS_DIR}/src
)
target_compile_definitions(ecat_core PUBLIC ECAT_EMBEDDED ECAT_SINGLE_THREAD FORCE_STD_THREADING USE_MINI_INI)
target_compile_options(ecat_core PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>
    -O2 -ffunction-sections -fdata-sections
    # objects may live in PSRAM, where LDREX/STREX never succeed: the atomics are
    # calls to platform/ecat_atomic.c (interrupts off, plain read-modify-write)
    -fno-inline-atomics
    -Wno-psabi -Wno-unused-parameter -Wno-deprecated-declarations)
# no pico libraries here: their INTERFACE sources would be compiled into the
# object library and again into the firmware

# The PDP-11 processors (the ДВК) at -O3: their interpreter is most of a
# core at the speeds these machines run
set_source_files_properties(
    ${ECAT3_SRC}/emulator/devices/cpu/pdp11core.cpp
    ${ECAT3_SRC}/emulator/devices/cpu/k1801vm1.cpp
    PROPERTIES COMPILE_OPTIONS "-O3")
