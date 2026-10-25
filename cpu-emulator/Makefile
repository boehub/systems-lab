CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2

SRC := $(wildcard src/*.cpp)

chip8: $(SRC) $(wildcard src/*.h)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC)

clean:
	rm -f chip8

.PHONY: clean
