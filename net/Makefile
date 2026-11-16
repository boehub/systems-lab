CC      ?= gcc
CFLAGS  ?= -std=gnu11 -Wall -Wextra -O2
SRC     := src/pcap.c src/decode.c src/filter.c src/format.c src/flows.c src/live.c

pktscope: $(SRC) src/main.c $(wildcard src/*.h)
	$(CC) $(CFLAGS) -o $@ src/main.c $(SRC)

test_unit: tests/test_unit.c tests/vectors.h $(SRC) $(wildcard src/*.h)
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_unit.c $(SRC)

# Unit tests, then end-to-end tests that run the real program on generated captures.
# The end-to-end tests need Python 3; the cross-checks also use scapy if installed.
test: pktscope test_unit
	./test_unit
	python3 -m unittest discover -s tests

# The same tests with memory-error and undefined-behaviour checking compiled in.
sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS="-std=gnu11 -Wall -Wextra -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined" test
	$(MAKE) clean

clean:
	rm -f pktscope test_unit

.PHONY: test sanitize clean
