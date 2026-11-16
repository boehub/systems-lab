CC      ?= gcc
CFLAGS  ?= -std=gnu11 -Wall -Wextra -O2

msh: src/msh.c src/parse.c src/parse.h
	$(CC) $(CFLAGS) -o $@ src/msh.c src/parse.c

test_parse: tests/test_parse.c src/parse.c src/parse.h
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_parse.c src/parse.c

# Unit tests for the parser, then end-to-end tests that run the real shell.
test: msh test_parse
	./test_parse
	python3 tests/test_shell.py

# The same tests with memory-error and leak checking compiled in.
sanitize:
	$(CC) $(CFLAGS) -fsanitize=address,undefined -g -Isrc -o test_parse tests/test_parse.c src/parse.c
	./test_parse
	$(CC) $(CFLAGS) -fsanitize=address,undefined -g -o msh src/msh.c src/parse.c
	python3 tests/test_shell.py
	rm -f msh test_parse

clean:
	rm -f msh test_parse

.PHONY: test sanitize clean
