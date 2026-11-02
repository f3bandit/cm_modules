/* selftest: a blank card, a save imported, listed, exported (byte for byte the same) and removed;
 * the library: stored, recognised, listed and loaded back.
 * Copyright (C) 2026 f3bandit. GPL-3.0-or-later (see LICENSE); Retro Remake: see LICENSE-RETRO-REMAKE.md. */
#include "mcformat.h"
#include "mclib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); fails++; } } while (0)

int main(int argc, char **argv)
{
	static unsigned char img[MC_SIZE], mcs[128 + 2 * MC_BLOCK], out[128 + 15 * MC_BLOCK];
	const char *err = "";
	mc_blank(img);
	CHECK(mc_formatted(img));
	mc_card_t c;
	CHECK(mc_parse(img, &c) == 0 && c.formatted && c.nsaves == 0 && c.free == 15);

	/* a 2-block save as .mcs: its directory frame, then its blocks ("SC", 16 colours, a title) */
	memset(mcs, 0, sizeof(mcs));
	mcs[0] = 0x51;
	memcpy(mcs + 0x0A, "BASLUS-00067DRACULA", 19);
	unsigned char *b = mcs + 128;
	b[0] = 'S'; b[1] = 'C'; b[2] = 0x11; b[3] = 2;
	for (int i = 0; i < 2 * MC_BLOCK - 128; i++) b[128 + i] = (unsigned char)(i * 7);
	CHECK(mc_import(img, mcs, sizeof(mcs), &err) == 0);
	CHECK(mc_parse(img, &c) == 0 && c.nsaves == 1 && c.free == 13);
	CHECK(c.nsaves == 1 && c.saves[0].nblocks == 2 && !strcmp(c.saves[0].name, "BASLUS-00067DRACULA"));
	CHECK(c.nsaves == 1 && !strcmp(c.saves[0].product, "SLUS-00067"));
	CHECK(mc_import(img, mcs, sizeof(mcs), &err) != 0);               /* the same save twice */

	size_t n = 0;
	CHECK(mc_export(img, c.saves[0].slot, out, &n, &err) == 0 && n == sizeof(mcs));
	CHECK(n == sizeof(mcs) && !memcmp(out + 128, mcs + 128, 2 * MC_BLOCK));
	unsigned char rgba[16 * 16 * 4];
	mc_icon(img, c.saves[0].slot, rgba);

	/* the library */
	char dir[] = "/tmp/ps1mc_selftestXXXXXX";
	CHECK(mkdtemp(dir) != NULL);
	mclib_set_dir(dir);
	char rel[256]; int same = 0;
	CHECK(mclib_store("My card", img, rel, sizeof(rel), &same) == 0 && !same);
	CHECK(mclib_store("My card", img, rel, sizeof(rel), &same) == 0 && same);      /* not kept twice */
	char card[128] = "";
	CHECK(mclib_recognise(img, card, sizeof(card)) >= 0.5 && !strcmp(card, "My card"));
	static unsigned char back[MC_SIZE]; char got[256];
	CHECK(mclib_load("My card", "", back, got, sizeof(got)) == 0 && !memcmp(back, img, MC_SIZE));
	char *list = mclib_list();
	CHECK(list && strstr(list, "My card/"));
	free(list);

	CHECK(mc_remove(img, c.saves[0].slot, &err) == 0);
	CHECK(mc_parse(img, &c) == 0 && c.nsaves == 0 && c.free == 15);

	printf(fails ? "%d failed\n" : "all passed\n", fails);
	(void)argc; (void)argv;
	return fails != 0;
}
