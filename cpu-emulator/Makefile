CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2

SRC := $(wildcard src/*.cpp)

chip8: $(SRC) $(wildcard src/*.h)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC)

# Unit tests: the core, then the front-end logic (no framework needed).
test: tests/test_core.cpp tests/test_frontend.cpp src/chip8.cpp src/frontend.cpp src/chip8.h src/frontend.h
	$(CXX) $(CXXFLAGS) -Isrc -o test_core tests/test_core.cpp src/chip8.cpp
	./test_core
	$(CXX) $(CXXFLAGS) -Isrc -o test_frontend tests/test_frontend.cpp src/frontend.cpp src/chip8.cpp
	./test_frontend

# End-to-end tests that drive the real program through a pseudo-terminal
# (keyboard input, real-time timing, terminal restore). Needs Python 3.
test-interactive: chip8
	python3 tests/test_interactive.py

# Compare against the community CHIP-8 test suite (downloads three ROMs the
# first time; they are cached, and not stored in this repository).
conformance: chip8
	python3 tests/conformance.py

# The core must stay portable: no exceptions/RTTI, and no I/O headers.
# This is what lets the same code run on a microcontroller.
check-core:
	$(CXX) $(CXXFLAGS) -fno-exceptions -fno-rtti -c src/chip8.cpp -o /dev/null
	@! grep -nE '#include <(iostream|fstream|cstdio|stdio.h|vector|string|sstream)>' src/chip8.h src/chip8.cpp
	@echo "core is I/O-free"

clean:
	rm -f chip8 test_core test_frontend

.PHONY: clean check-core test test-interactive conformance
