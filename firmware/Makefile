CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2

CORE  := ../cpu-emulator/src
TOOLS := tools/make_roms.py ../compiler/chip8asm.py $(wildcard ../compiler/nibble/*.py)

# The programs built into the firmware, in menu order (.asm, .nib or .ch8).
# One program runs straight away; several give a menu.
ROMS ?= ../compiler/examples/zero.asm

# The full pipeline: programs -> C tables -> firmware run loop.
# (roms.list remembers which programs were used, so changing ROMS rebuilds.)
build/roms.list: FORCE
	@mkdir -p build
	@echo "$(ROMS)" | cmp -s - $@ || echo "$(ROMS)" > $@

build/roms.h: build/roms.list $(ROMS) $(TOOLS)
	python3 tools/make_roms.py -o $@ $(ROMS)

SRCS := src/main.cpp src/menu.cpp src/text.cpp host/host_platform.cpp $(CORE)/chip8.cpp

build/host_demo: $(SRCS) src/*.h $(CORE)/chip8.h build/roms.h
	$(CXX) $(CXXFLAGS) -Isrc -I$(CORE) -Ibuild -o $@ $(SRCS)

# Run the firmware loop on this computer (terminal output).
run: build/host_demo
	./build/host_demo

# Show the program menu on this computer (the host has no keys, so it just
# draws the menu for a couple of seconds).
menu:
	$(MAKE) run ROMS="../compiler/examples/move.asm ../compiler/examples/counter.nib ../compiler/examples/fib.nib ../compiler/examples/primes.nib ../compiler/examples/sum.asm ../compiler/examples/zeros.asm"

# Unit tests for the hardware-independent logic: OLED frame packing, button
# matrix, debouncing, the menu, text, frame timing, and the ROM table tool.
test: tests/test_pico_logic.cpp tests/test_ui_logic.cpp pico/oled_frame.cpp pico/keypad_matrix.cpp src/menu.cpp src/text.cpp
	mkdir -p build
	$(CXX) $(CXXFLAGS) -Ipico -o build/test_pico_logic tests/test_pico_logic.cpp pico/oled_frame.cpp pico/keypad_matrix.cpp
	./build/test_pico_logic
	$(CXX) $(CXXFLAGS) -Isrc -o build/test_ui_logic tests/test_ui_logic.cpp src/menu.cpp src/text.cpp
	./build/test_ui_logic
	python3 tests/test_make_roms.py

clean:
	rm -rf build

.PHONY: run menu test clean FORCE
