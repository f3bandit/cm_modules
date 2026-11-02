# ss1_cm_modules. Copyright (C) 2026 f3bandit. GPL-3.0-or-later (see LICENSE).
#   make test     builds and runs the host tests
#   make arm      ss1jobd and ss1mod for the SuperStation (static, armhf)
CC      ?= cc
ARMCC   ?= arm-linux-gnueabihf-gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11 -D_DEFAULT_SOURCE -D_GNU_SOURCE
INC      = -Iinclude -Ivendor
LIB      = src/ss1mod.c vendor/mcformat.c vendor/mclib.c

all: test

test: tests/test_parse.c src/ss1mod.c
	$(CC) $(CFLAGS) $(INC) -o test_parse tests/test_parse.c src/ss1mod.c && ./test_parse
	$(CC) $(CFLAGS) $(INC) -o test_memcard tests/test_memcard.c vendor/mcformat.c vendor/mclib.c && ./test_memcard

arm: build/ss1jobd build/ss1mod

build/ss1jobd: tools/ss1jobd.c $(LIB)
	mkdir -p build && $(ARMCC) $(CFLAGS) $(INC) -static -s -o $@ tools/ss1jobd.c $(LIB)

build/ss1mod: tools/ss1mod.c $(LIB)
	mkdir -p build && $(ARMCC) $(CFLAGS) $(INC) -static -s -o $@ tools/ss1mod.c $(LIB)

clean:
	rm -rf build test_parse test_memcard

.PHONY: all test arm clean
