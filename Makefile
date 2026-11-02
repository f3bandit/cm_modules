# cm_modules. Copyright (C) 2026 f3bandit. GPL-3.0-or-later (see LICENSE).
#   make test     builds and runs the host tests
#   make arm      cmjobd, cmmod, cmremote, cmnvme and cmtrimfs for the SuperStation (static, armhf)
CC      ?= cc
ARMCC   ?= arm-linux-gnueabihf-gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11 -D_DEFAULT_SOURCE -D_GNU_SOURCE
INC      = -Iinclude -Ivendor
LIB      = src/cmmod.c src/cmwifi.c src/cmbt.c vendor/mcformat.c vendor/mclib.c

all: test

test: tests/test_parse.c src/cmmod.c
	$(CC) $(CFLAGS) $(INC) -o test_parse tests/test_parse.c src/cmmod.c && ./test_parse
	$(CC) $(CFLAGS) $(INC) -o test_memcard tests/test_memcard.c vendor/mcformat.c vendor/mclib.c && ./test_memcard
	$(CC) -O1 -g -std=gnu11 -fsanitize=address,undefined $(INC) -o test_cmmod tools/cmmod.c $(LIB) -lpthread && sh tests/test_wifi.sh ./test_cmmod && sh tests/test_bt.sh ./test_cmmod
	$(CC) $(CFLAGS) -o test_trimfs tools/cmtrimfs.c && sh tests/test_trimfs.sh ./test_trimfs
	$(CC) -O1 -g -std=gnu11 -fsanitize=address,undefined $(INC) -DCMREMOTE_VERSION='"test"' -o test_remote $(REMOTE) -lpthread -lm && sh remote/tests/test_remote.sh ./test_remote

REMOTE  = remote/cmremote.c remote/cmlib.c remote/jobs.c remote/ua.c remote/tools.c remote/ctrl.c remote/vtry.c remote/extras.c remote/dash.c remote/sha256.c remote/vendor/cJSON.c src/cmmod.c src/cmwifi.c src/cmbt.c vendor/mcformat.c
REMOTE_VERSION = 0.1.0

arm: build/cmjobd build/cmmod build/cmremote build/cmnvme build/cmtrimfs

build/cmremote: $(REMOTE) remote/cmlib.h remote/jobs.h remote/ua.h remote/tools.h remote/ctrl.h include/cmmod.h
	mkdir -p build && $(ARMCC) -O2 -std=gnu11 $(INC) -DCMREMOTE_VERSION='"$(REMOTE_VERSION)"' -static -s -o $@ $(REMOTE) -lpthread -lm


build/cmjobd: tools/cmjobd.c $(LIB)
	mkdir -p build && $(ARMCC) $(CFLAGS) $(INC) -static -s -o $@ tools/cmjobd.c $(LIB) -lpthread

build/cmtrimfs: tools/cmtrimfs.c
	mkdir -p build && $(ARMCC) $(CFLAGS) -static -s -o $@ tools/cmtrimfs.c

build/cmnvme: tools/cmnvme.c
	mkdir -p build && $(ARMCC) $(CFLAGS) -static -s -o $@ tools/cmnvme.c

build/cmmod: tools/cmmod.c $(LIB)
	mkdir -p build && $(ARMCC) $(CFLAGS) $(INC) -static -s -o $@ tools/cmmod.c $(LIB) -lpthread

clean:
	rm -rf build test_parse test_memcard test_remote test_cmmod test_trimfs

.PHONY: all test arm clean
