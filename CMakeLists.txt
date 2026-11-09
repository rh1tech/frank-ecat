#
# frank-ecat — eCat3 (github.com/Ptr314/ecat3) on RP2350 (FRANK M2)
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
#
# Licensed under the GNU General Public License v3.0 or later.
#

cmake_minimum_required(VERSION 3.13)

# M2: Murmulator 2 / FRANK (PIO HDMI on GPIO 12-19, I2S DAC on 9-11, SD on SPI0,
# PSRAM CS on 47 (RP2350B) or 8 (RP2350A), picked at run time).
set(PICO_BOARD frank_m2_rp2350b)
set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/boards)

set(CPU_SPEED "504" CACHE STRING "CPU clock in MHz: 252, 378, 504")
set(PSRAM_SPEED "168" CACHE STRING "PSRAM max frequency in MHz: 84, 100, 133, 168")
set(PSRAM_RXDELAY "6" CACHE STRING "PSRAM read sampling delay override (empty: derived from the clock)")
set(FLASH_SPEED "126" CACHE STRING "Flash chip max frequency in MHz")
set(FLASH_RXDELAY "5" CACHE STRING "Flash read sampling delay override (empty: derived from the clock)")
# clk_peri (UART, SD card SPI) from clk_sys; 0 keeps the 48 MHz USB PLL
set(PERI_SPEED "168" CACHE STRING "clk_peri in MHz, from clk_sys (0: 48 MHz from the USB PLL)")
set(SD_SPEED "42" CACHE STRING "SD card SPI clock in MHz (clk_peri / an even divider)")

# usb:  native USB is the CDC debug console (no keyboard; the console feeds keys)
# uart: native USB is the keyboard host, the console is UART0 (GPIO 0/1)
set(DEBUG_CONSOLE "usb" CACHE STRING "Debug console: usb or uart")
set_property(CACHE DEBUG_CONSOLE PROPERTY STRINGS usb uart)
if(DEBUG_CONSOLE STREQUAL "uart")
    set(USB_HID_ENABLED ON)
elseif(DEBUG_CONSOLE STREQUAL "usb")
    set(USB_HID_ENABLED OFF)
else()
    message(FATAL_ERROR "DEBUG_CONSOLE must be usb or uart")
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

if(CPU_SPEED GREATER_EQUAL 504)
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_65")
elseif(CPU_SPEED GREATER_EQUAL 378)
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_60")
else()
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_50")
endif()

set(SD_SPI_BUS spi0)
set(SD_PIN_SCK 6)
set(SD_PIN_MOSI 7)
set(SD_PIN_MISO 4)
set(SD_PIN_CS 5)
set(I2S_DATA_PIN 9)
set(I2S_CLK_PIN 10) # and LRCK 11

include(pico_sdk_import.cmake)

project(frank-ecat C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

file(READ "${CMAKE_CURRENT_LIST_DIR}/version.txt" ECAT_VERSION_RAW)
string(STRIP "${ECAT_VERSION_RAW}" ECAT_VERSION_RAW)
string(REGEX REPLACE "[ \t]+" ";" ECAT_VERSION_PARTS "${ECAT_VERSION_RAW}")
list(GET ECAT_VERSION_PARTS 0 ECAT_VERSION_MAJOR)
list(GET ECAT_VERSION_PARTS 1 ECAT_VERSION_MINOR)
set(ECAT_VERSION "${ECAT_VERSION_MAJOR}.${ECAT_VERSION_MINOR}")

# Core 1 runs on its own stack in main RAM (platform_launch_core1): SCRATCH_X
# holds the HDMI line IRQ. 32 KB core 0 stack (STACK_RAM in memmap_ecat.ld).
add_compile_definitions(PICO_CORE1_STACK_SIZE=0 PICO_STACK_SIZE=0x8000 PICO_USE_STACK_GUARDS=1)
# The eCat3 core throws on configuration errors and finds its devices by dynamic_cast
set(PICO_CXX_ENABLE_EXCEPTIONS 1)
set(PICO_CXX_ENABLE_RTTI 1)

pico_sdk_init()

# ---------------------------------------------------------------------
# Drivers (from frank-quake2 and pico-speccy)
# ---------------------------------------------------------------------
add_subdirectory(drivers/fatfs)
add_subdirectory(drivers/sdcard)
add_subdirectory(drivers/usbhid)
add_subdirectory(drivers/audio)

# pico-speccy's HDMI driver, HDMI only (no vga-nextgen). Back-end: 0 PIO, 2 HSTX TMDS encoder
set(HDMI_BACKEND "2" CACHE STRING "HDMI back-end: 0 PIO, 2 HSTX")
add_library(video_hdmi STATIC
    drivers/hdmi/hdmi.c
    drivers/hdmi/hdmi_hstx.c
    drivers/hdmi/hdmi_glue.c
    drivers/graphics/graphics.c
)
target_include_directories(video_hdmi PUBLIC drivers/hdmi drivers/graphics)
target_compile_definitions(video_hdmi PUBLIC HDMI MURM2=1 HDMI_BASE_PIN=12 HDMI_HSTX=${HDMI_BACKEND} CPU_MHZ=${CPU_SPEED} HDMI_AUDIO_FS_IN=32000)
# the line IRQ must stay in SRAM: no loops turned into newlib calls
target_compile_options(video_hdmi PRIVATE -fno-tree-loop-distribute-patterns -O2)
target_link_libraries(video_hdmi PUBLIC pico_stdlib pico_multicore hardware_pio hardware_dma hardware_clocks hardware_gpio)

add_library(drivers STATIC
    drivers/psram_init.c
    drivers/psram_heap.c
    drivers/dlmalloc.c
)
target_include_directories(drivers PUBLIC drivers)
target_link_libraries(drivers PUBLIC pico_stdlib hardware_dma hardware_irq hardware_clocks hardware_gpio)
set_source_files_properties(drivers/dlmalloc.c PROPERTIES COMPILE_DEFINITIONS
    "ONLY_MSPACES=1;HAVE_MMAP=0;HAVE_MORECORE=0;NO_MALLOC_STATS=1;USE_LOCKS=0;ABORT_ON_ASSERT_FAILURE=0;MALLOC_FAILURE_ACTION=;LACKS_SYS_MMAN_H=1;LACKS_UNISTD_H=1;LACKS_SYS_PARAM_H=1"
)
target_compile_definitions(drivers PUBLIC
    PSRAM_MAX_FREQ_MHZ=${PSRAM_SPEED}
    $<$<BOOL:${PSRAM_RXDELAY}>:PSRAM_RXDELAY=${PSRAM_RXDELAY}>
)

set(ECAT_DEFINES
    BOARD_M2
    CPU_CLOCK_MHZ=${CPU_SPEED}
    CPU_VOLTAGE=${CPU_VOLTAGE}
    PERI_CLOCK_MHZ=${PERI_SPEED}
    PSRAM_MAX_FREQ_MHZ=${PSRAM_SPEED}
    $<$<BOOL:${PSRAM_RXDELAY}>:PSRAM_RXDELAY=${PSRAM_RXDELAY}>
    FLASH_MAX_FREQ_MHZ=${FLASH_SPEED}
    $<$<BOOL:${FLASH_RXDELAY}>:FLASH_RXDELAY=${FLASH_RXDELAY}>
    PICO_AUDIO_I2S_DATA_PIN=${I2S_DATA_PIN}
    PICO_AUDIO_I2S_CLOCK_PIN_BASE=${I2S_CLK_PIN}
    ECAT_VERSION="${ECAT_VERSION}"
    ECAT_CONSOLE_USB=$<STREQUAL:${DEBUG_CONSOLE},usb>
)

# Board bring-up, the C heap, sound back-ends: shared by every firmware
add_library(ecat_platform STATIC
    platform/platform.c
    platform/uart_buffered.c
    platform/ecat_malloc.c
    platform/frank_audio.c
)
target_include_directories(ecat_platform PUBLIC platform)
target_compile_definitions(ecat_platform PUBLIC ${ECAT_DEFINES})
target_link_libraries(ecat_platform PUBLIC
    pico_stdlib pico_multicore hardware_clocks hardware_vreg hardware_watchdog
    drivers video_hdmi fatfs sdcard usbhid audio_driver)

# the menus: frank-dino's UI (ui/), over this firmware (ui/ecat_ui.h)
add_library(ecat_ui STATIC
    ui/menu.c
    ui/storage.c
    ui/ui_browser.c
    ui/ui_dialog.c
    ui/ui_font.c
    ui/ui_gfx.c
    ui/ui_nav.c
    ui/ui_render.c
    ui/ui_tree.c
)
target_include_directories(ecat_ui PUBLIC ui)
target_link_libraries(ecat_ui PUBLIC ecat_platform)

# memmap_ecat.ld INCLUDEs ecat_hot.ld (the functions that run from SRAM):
# empty until the emulator core is built, see ecat_hot below
# (in a directory of its own: ld looks in the current one before -L, and a
# family's own list, hot-<family>/, must be found first)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/hot-all)
if(NOT EXISTS ${CMAKE_CURRENT_BINARY_DIR}/hot-all/ecat_hot.ld)
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/hot-all/ecat_hot.ld "/* no hot functions */\n")
endif()

function(ecat_firmware_setup target)
    pico_set_linker_script(${target} ${CMAKE_CURRENT_SOURCE_DIR}/memmap_ecat.ld)
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/memmap_ecat.ld)
    target_link_options(${target} PRIVATE -L${CMAKE_CURRENT_BINARY_DIR}/hot-all
        -Wl,--wrap=panic -Wl,--wrap=__cxa_throw -Wl,--wrap=__cxa_allocate_exception -Wl,--print-memory-usage -Wl,--no-enum-size-warning)
    target_compile_definitions(${target} PRIVATE PICO_MALLOC_PANIC=0)
    if(DEBUG_CONSOLE STREQUAL "usb")
        pico_enable_stdio_usb(${target} 1)
        pico_enable_stdio_uart(${target} 0)
    else()
        pico_enable_stdio_usb(${target} 0)
        pico_enable_stdio_uart(${target} 1)
    endif()
    pico_add_extra_outputs(${target})
endfunction()

# ---------------------------------------------------------------------
# The emulator core
# ---------------------------------------------------------------------
find_package(Python3 REQUIRED COMPONENTS Interpreter)
include(cmake/ecat_core.cmake)

# The emulator's hot functions in SRAM: the linker lines, from the core's objects
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/ecat_hot.stamp
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/hot_sections.py
            ${CMAKE_CURRENT_SOURCE_DIR}/platform/hot_functions.txt
            ${CMAKE_CURRENT_BINARY_DIR}/hot-all/ecat_hot.ld ${CMAKE_NM} $<TARGET_OBJECTS:ecat_core>
    COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_CURRENT_BINARY_DIR}/ecat_hot.stamp
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/platform/hot_functions.txt
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/hot_sections.py $<TARGET_OBJECTS:ecat_core>
    COMMAND_EXPAND_LISTS
    COMMENT "Listing the hot functions for SRAM")
add_custom_target(ecat_hot DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/ecat_hot.stamp)

# ---------------------------------------------------------------------
# Hardware bring-up firmware: video modes, HDMI/I2S sound, PSRAM, SD
# ---------------------------------------------------------------------
add_executable(frank-ecat-diag platform/diag_main.c)
target_link_libraries(frank-ecat-diag ecat_platform)
ecat_firmware_setup(frank-ecat-diag)

# ---------------------------------------------------------------------
# Emulator firmware, one per family of machines
# ---------------------------------------------------------------------
# The host console build (ecat3, ENABLE_HEADLESS) flattens the .ext machines
set(ECAT_HOST_HEADLESS "${CMAKE_CURRENT_SOURCE_DIR}/build-host/eCat3-headless" CACHE FILEPATH
    "eCat3-headless of the host, for --print-config")

# ecat_add_firmware(<family> VIDEO_MODE <n> REFRESH <Hz x 100> SURFACE <w> <h>
#                   MACHINES <cfg|ext>...)
# SURFACE: the largest picture of the family's displays (static SRAM: the HDMI
# line IRQ reads it, and cannot read 800 dots a line out of the PSRAM in time)
function(ecat_add_firmware family)
    cmake_parse_arguments(F "" "VIDEO_MODE;REFRESH" "MACHINES;SURFACE" ${ARGN})
    list(GET F_SURFACE 0 surface_w)
    list(GET F_SURFACE 1 surface_h)
    set(target frank-ecat-${family})
    set(vfs_dir ${CMAKE_CURRENT_BINARY_DIR}/vfs-${family})
    set(machine_files)
    foreach(m ${F_MACHINES})
        list(APPEND machine_files ${CMAKE_CURRENT_SOURCE_DIR}/ecat3/deploy/computers/${m})
    endforeach()
    add_custom_command(
        OUTPUT ${vfs_dir}/vfs_data.S ${vfs_dir}/vfs_table.c
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_vfs.py
                --deploy ${CMAKE_CURRENT_SOURCE_DIR}/ecat3/deploy
                --headless ${ECAT_HOST_HEADLESS}
                --out-dir ${vfs_dir} ${F_MACHINES}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_vfs.py ${machine_files}
        COMMENT "Packing the ${family} machines into the flash VFS")
    add_executable(${target}
        platform/ecat_main.cpp
        platform/frank_keys.cpp
        platform/frank_fs.c
        platform/ecat_atomic.c
        platform/ram_hot.c
        ${vfs_dir}/vfs_data.S
        ${vfs_dir}/vfs_table.c)
    target_include_directories(${target} PRIVATE platform)
    target_compile_definitions(${target} PRIVATE
        ECAT_VIDEO_MODE=${F_VIDEO_MODE} ECAT_REFRESH=${F_REFRESH} ECAT_FAMILY="${family}"
        ECAT_SURFACE_W=${surface_w} ECAT_SURFACE_H=${surface_h})
    target_link_libraries(${target} ecat_ui ecat_platform ecat_core)
    # The family's own hot functions (an ecat_hot.ld found before the shared one)
    set(hot_dir ${CMAKE_CURRENT_BINARY_DIR}/hot-${family})
    file(MAKE_DIRECTORY ${hot_dir})
    if(NOT EXISTS ${hot_dir}/ecat_hot.ld)
        file(WRITE ${hot_dir}/ecat_hot.ld "/* no hot functions */\n")
    endif()
    add_custom_command(
        OUTPUT ${hot_dir}/ecat_hot.stamp
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/hot_sections.py --family ${family}
                ${CMAKE_CURRENT_SOURCE_DIR}/platform/hot_functions.txt
                ${hot_dir}/ecat_hot.ld ${CMAKE_NM} $<TARGET_OBJECTS:ecat_core>
        COMMAND ${CMAKE_COMMAND} -E touch ${hot_dir}/ecat_hot.stamp
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/platform/hot_functions.txt
                ${CMAKE_CURRENT_SOURCE_DIR}/tools/hot_sections.py $<TARGET_OBJECTS:ecat_core>
        COMMAND_EXPAND_LISTS
        COMMENT "Listing the ${family} hot functions for SRAM")
    add_custom_target(ecat_hot_${family} DEPENDS ${hot_dir}/ecat_hot.stamp)
    target_link_options(${target} PRIVATE -L${hot_dir})
    ecat_firmware_setup(${target})
    add_dependencies(${target} ecat_hot_${family})
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS ${hot_dir}/ecat_hot.stamp)
endfunction()

ecat_add_firmware(rk86 VIDEO_MODE 28 REFRESH 5001 SURFACE 480 300 MACHINES
    radio-86rk/Radio-86rk.cfg
    radio-86rk/Spektr001.ext
    mikrosha/Mikrosha.cfg
    apogey/Apogey.cfg)

ecat_add_firmware(agat VIDEO_MODE 28 REFRESH 5001 SURFACE 512 256 MACHINES
    agat/Agat-7.cfg
    agat/Agat-7-840.cfg
    agat/Agat-7-840-pp.ext
    agat/Agat-7-max.ext
    agat/Agat-9.cfg
    agat/Agat-9-pp.ext
    agat/Agat-9-yazs.ext)

ecat_add_firmware(dvk VIDEO_MODE 28 REFRESH 5001 SURFACE 800 286 MACHINES
    dvk/DVK-1.cfg
    dvk/DVK-2.cfg
    dvk/DVK-3.cfg
    dvk/DVK-4.cfg)

ecat_add_firmware(orion VIDEO_MODE 28 REFRESH 5001 SURFACE 384 256 MACHINES
    orion-128/Orion-128-m1.cfg
    orion-128/Orion-128-m3.ext
    orion-128/Orion-128-Z80.cfg)

ecat_add_firmware(irisha VIDEO_MODE 28 REFRESH 5001 SURFACE 640 240 MACHINES
    irisha/Irisha-gmd70.cfg
    irisha/Irisha-kngmd.cfg
    irisha/Dialog.cfg
    irisha/Kaspiy.cfg)

ecat_add_firmware(unior VIDEO_MODE 28 REFRESH 5001 SURFACE 480 300 MACHINES
    unior/Unior.cfg
    unior/Argo.cfg)

ecat_add_firmware(pravetz VIDEO_MODE 28 REFRESH 5001 SURFACE 560 224 MACHINES
    pravetz/Pravetz-82.cfg
    pravetz/Pravetz-8M.ext
    pravetz/Pravetz-8A.cfg
    pravetz/Pravetz-8A-full.ext
    pravetz/Pravetz-8C.cfg
    pravetz/Pravetz-8D.cfg)

ecat_add_firmware(bk VIDEO_MODE 28 REFRESH 5001 SURFACE 512 256 MACHINES
    bk/BK-0010.cfg
    bk/BK-0010-01.cfg
    bk/BK-0010-01-fdd.ext
    bk/BK-0010-01-a16m.ext
    bk/BK-0010-01-smk512.ext
    bk/BK-0011M.cfg
    bk/BK-0011M-fdd.ext
    bk/BK-0011M-a16m.ext
    bk/BK-0011M-smk512.ext
    bk/BK-0011M-mstd.ext)

ecat_add_firmware(uknc VIDEO_MODE 28 REFRESH 5001 SURFACE 640 288 MACHINES
    uknc/UKNC.cfg
    uknc/UKNC-031.ext
    uknc/UKNC-basic.ext)

ecat_add_firmware(e85 VIDEO_MODE 28 REFRESH 5001 SURFACE 1024 256 MACHINES
    e85/MS0585.cfg
    e85/MS0585-1702.ext
    e85/MS0585-NMD.ext)
