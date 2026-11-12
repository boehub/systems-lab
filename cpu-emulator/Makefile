CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2

SRC := $(wildcard src/*.cpp)

chip8: $(SRC) $(wildcard src/*.h)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC)

# Unit tests for the core (no framework needed).
test: tests/test_core.cpp src/chip8.cpp src/chip8.h
	$(CXX) $(CXXFLAGS) -Isrc -o test_core tests/test_core.cpp src/chip8.cpp
	./test_core

# The core must stay portable: no exceptions/RTTI, and no I/O headers.
# This is what lets the same code run on a microcontroller.
check-core:
	$(CXX) $(CXXFLAGS) -fno-exceptions -fno-rtti -c src/chip8.cpp -o /dev/null
	@! grep -nE '#include <(iostream|fstream|cstdio|stdio.h|vector|string|sstream)>' src/chip8.h src/chip8.cpp
	@echo "core is I/O-free"

clean:
	rm -f chip8 test_core

.PHONY: clean check-core test
