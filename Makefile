# ss1_cm_modules. Copyright (C) 2026 f3bandit. GPL-3.0-or-later (see LICENSE).
#   make test     builds and runs the host tests
#   make arm      ss1jobd, ss1mod and ss1remote for the SuperStation (static, armhf)
CC      ?= cc
ARMCC   ?= arm-linux-gnueabihf-gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11 -D_DEFAULT_SOURCE -D_GNU_SOURCE
INC      = -Iinclude -Ivendor
LIB      = src/ss1mod.c vendor/mcformat.c vendor/mclib.c

all: test

test: tests/test_parse.c src/ss1mod.c
	$(CC) $(CFLAGS) $(INC) -o test_parse tests/test_parse.c src/ss1mod.c && ./test_parse
	$(CC) $(CFLAGS) $(INC) -o test_memcard tests/test_memcard.c vendor/mcformat.c vendor/mclib.c && ./test_memcard
	$(CC) -O1 -g -std=gnu11 -fsanitize=address,undefined $(INC) -DSS1REMOTE_VERSION='"test"' -o test_remote $(REMOTE) -lpthread -lm && sh remote/tests/test_remote.sh ./test_remote

REMOTE  = remote/ss1remote.c remote/cmlib.c remote/jobs.c remote/ua.c remote/tools.c remote/dash.c remote/sha256.c remote/vendor/cJSON.c src/ss1mod.c vendor/mcformat.c
REMOTE_VERSION = 0.1.0

arm: build/ss1jobd build/ss1mod build/ss1remote

build/ss1remote: $(REMOTE) remote/cmlib.h remote/jobs.h remote/ua.h remote/tools.h include/ss1mod.h
	mkdir -p build && $(ARMCC) -O2 -std=gnu11 $(INC) -DSS1REMOTE_VERSION='"$(REMOTE_VERSION)"' -static -s -o $@ $(REMOTE) -lpthread -lm


build/ss1jobd: tools/ss1jobd.c $(LIB)
	mkdir -p build && $(ARMCC) $(CFLAGS) $(INC) -static -s -o $@ tools/ss1jobd.c $(LIB)

build/ss1mod: tools/ss1mod.c $(LIB)
	mkdir -p build && $(ARMCC) $(CFLAGS) $(INC) -static -s -o $@ tools/ss1mod.c $(LIB)

clean:
	rm -rf build test_parse test_memcard test_remote

.PHONY: all test arm clean
