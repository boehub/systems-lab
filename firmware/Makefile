CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2

CORE := ../cpu-emulator/src
ASM  := ../compiler/chip8asm.py
ROM  := ../compiler/examples/zero.asm

# The full pipeline: assembly -> C header -> firmware run loop.
# Change ROM above to embed a different program.
build/rom.h: $(ROM) $(ASM)
	mkdir -p build
	python3 $(ASM) $(ROM) --format c -o $@

build/host_demo: src/main.cpp host/host_platform.cpp $(CORE)/chip8.cpp build/rom.h
	$(CXX) $(CXXFLAGS) -Isrc -I$(CORE) -Ibuild -o $@ \
		src/main.cpp host/host_platform.cpp $(CORE)/chip8.cpp

# Run the firmware loop on this computer (terminal output).
run: build/host_demo
	./build/host_demo

clean:
	rm -rf build

.PHONY: run clean
