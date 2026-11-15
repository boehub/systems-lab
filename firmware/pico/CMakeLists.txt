# Raspberry Pi Pico build of the CHIP-8 firmware.
#
#   export PICO_SDK_PATH=/path/to/pico-sdk
#   mkdir build && cd build
#   cmake ..                 # builds in the example programs and a menu to choose
#   make
#
# That produces chip8_pico.uf2. Hold BOOTSEL while plugging the Pico in and
# copy the file onto the drive that appears.
#
# ROMS is the list of programs built into the firmware, in menu order: .asm
# files (assembled with chip8asm.py), .nib files (compiled with nibc.py) or
# ready-made .ch8 ROMs. One program runs straight away; several give a menu.
#   cmake -DROMS="/path/a.asm;/path/b.nib" ..

cmake_minimum_required(VERSION 3.13)

set(EXAMPLES "${CMAKE_CURRENT_LIST_DIR}/../../compiler/examples")
set(ROMS
    "${EXAMPLES}/move.asm" "${EXAMPLES}/counter.nib" "${EXAMPLES}/fib.nib"
    "${EXAMPLES}/primes.nib" "${EXAMPLES}/sum.asm" "${EXAMPLES}/zeros.asm"
    CACHE STRING "Programs to build in, in menu order (.asm, .nib or .ch8)")
set(CYCLES_PER_FRAME 12 CACHE STRING "CHIP-8 instructions per 60 Hz frame (12 is about 720 per second)")

include(pico_sdk_import.cmake)
project(chip8_pico C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

# Programs -> C tables (build/roms.h), with the same tools as the desktop build.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(COMPILER_DIR "${CMAKE_CURRENT_LIST_DIR}/../../compiler")
file(GLOB COMPILER_SOURCES "${COMPILER_DIR}/nibble/*.py")
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/roms.h
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/../tools/make_roms.py
            -o ${CMAKE_CURRENT_BINARY_DIR}/roms.h ${ROMS}
    DEPENDS ${ROMS} ${CMAKE_CURRENT_LIST_DIR}/../tools/make_roms.py
            ${COMPILER_DIR}/chip8asm.py ${COMPILER_SOURCES}
    COMMENT "Building the program table"
)

add_executable(chip8_pico
    ../src/main.cpp
    ../src/menu.cpp
    ../src/text.cpp
    ../../cpu-emulator/src/chip8.cpp
    pico_platform.cpp
    oled_frame.cpp
    keypad_matrix.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/roms.h
)
target_include_directories(chip8_pico PRIVATE
    ../src
    ../../cpu-emulator/src
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_LIST_DIR}
)
target_compile_definitions(chip8_pico PRIVATE CHIP8_CYCLES_PER_FRAME=${CYCLES_PER_FRAME})
# No exceptions/RTTI/guarded statics: the core doesn't use them, and leaving them
# out keeps the firmware small.
target_compile_options(chip8_pico PRIVATE -Wall -Wextra
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti -fno-threadsafe-statics>)
target_link_libraries(chip8_pico pico_stdlib pico_rand hardware_i2c hardware_gpio)

pico_add_extra_outputs(chip8_pico)   # also writes chip8_pico.uf2
