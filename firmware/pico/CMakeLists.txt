# Raspberry Pi Pico build of the CHIP-8 firmware.
#
#   export PICO_SDK_PATH=/path/to/pico-sdk
#   mkdir build && cd build
#   cmake ..                                  # or: cmake -DROM=../../../compiler/examples/fib.nib ..
#   make
#
# That produces chip8_pico.uf2. Hold BOOTSEL while plugging the Pico in and
# copy the file onto the drive that appears.
#
# ROM is the program baked into the firmware: a .asm file (assembled with
# chip8asm.py) or a .nib file (compiled with nibc.py).

cmake_minimum_required(VERSION 3.13)

set(ROM "${CMAKE_CURRENT_LIST_DIR}/../../compiler/examples/move.asm"
    CACHE FILEPATH "Program to embed (.asm or .nib)")
set(CYCLES_PER_FRAME 12 CACHE STRING "CHIP-8 instructions per 60 Hz frame (12 is about 720 per second)")

include(pico_sdk_import.cmake)
project(chip8_pico C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

# ROM -> C array (build/rom.h), using the same tools as the desktop build.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
get_filename_component(ROM_EXT "${ROM}" LAST_EXT)
set(COMPILER_DIR "${CMAKE_CURRENT_LIST_DIR}/../../compiler")
if(ROM_EXT STREQUAL ".nib")
    set(ROM_TOOL "${COMPILER_DIR}/nibc.py")
elseif(ROM_EXT STREQUAL ".asm")
    set(ROM_TOOL "${COMPILER_DIR}/chip8asm.py")
else()
    message(FATAL_ERROR "ROM must be a .asm or .nib file, got: ${ROM}")
endif()
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/rom.h
    COMMAND ${Python3_EXECUTABLE} ${ROM_TOOL} ${ROM} --format c -o ${CMAKE_CURRENT_BINARY_DIR}/rom.h
    DEPENDS ${ROM} ${ROM_TOOL} ${COMPILER_DIR}/chip8asm.py
    COMMENT "Building ROM from ${ROM}"
)

add_executable(chip8_pico
    ../src/main.cpp
    ../../cpu-emulator/src/chip8.cpp
    pico_platform.cpp
    oled_frame.cpp
    keypad_matrix.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/rom.h
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
target_link_libraries(chip8_pico pico_stdlib hardware_i2c hardware_gpio)

pico_add_extra_outputs(chip8_pico)   # also writes chip8_pico.uf2
