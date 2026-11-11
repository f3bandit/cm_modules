/* selftest: a blank card, a save imported, listed, exported (byte for byte the same) and removed;
 * the library: stored, recognised, listed and loaded back.
 * Copyright (C) 2026 f3bandit. All rights reserved; licensed to Retro Remake (see LICENSE.md). */
#include "mcformat.h"
#include "mclib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

	/* deleting: a version (a card keeps at least one), then the card; each only when confirmed */
	sleep(1);                                                                      /* versions are named by the second */
	CHECK(mclib_store("My card", img, rel, sizeof(rel), &same) == 0 && !same);
	int nv = 0; char **v = mclib_versions("My card", &nv);
	CHECK(nv == 2);
	const char *why = NULL;
	CHECK(mclib_delete_version("My card", v[0], NULL, &why) < 0 && !strcmp(why, "not confirmed"));
	CHECK(mclib_delete_version("My card", v[0], v[1], &why) < 0);
	CHECK(mclib_delete_version("My card", "../x.mcd", "../x.mcd", &why) < 0);
	CHECK(mclib_delete_version("My card", v[0], v[0], &why) == 0);
	CHECK(mclib_delete_version("My card", v[1], v[1], &why) < 0 && strstr(why, "at least one"));
	for (int i = 0; i < nv; i++) free(v[i]);
	free(v);
	CHECK(mclib_delete_card("My card", "my card", &why) < 0 && mclib_exists("My card"));
	CHECK(mclib_delete_card("../tmp", "../tmp", &why) < 0);
	CHECK(mclib_delete_card("My card", "My card", &why) == 0 && !mclib_exists("My card"));
	CHECK(mclib_delete_card("My card", "My card", &why) < 0 && !strcmp(why, "no such card"));
	rmdir(dir);

	printf(fails ? "%d failed\n" : "all passed\n", fails);
	(void)argc; (void)argv;
	return fails != 0;
}
