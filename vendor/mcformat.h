/* ps1_memcard_lib: PlayStation memory card images (mcformat.c).
 * Copyright (C) 2026 f3bandit. PolyForm Noncommercial 1.0.0 (see LICENSE.md). */
#ifndef PS1MC_MCFORMAT_H
#define PS1MC_MCFORMAT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MC_SIZE (128 * 1024)
#define MC_BLOCK 8192

typedef struct {
	int slot;               /* first block (1-15) */
	int blocks[15], nblocks;
	char name[32];          /* file name on the card, e.g. BASLUS-00067DRACULA */
	char product[16];       /* SLUS-00067 */
	char region[8];         /* USA, Europe, Japan */
	char title[96];
	int broken;
} mc_save_t;

typedef struct {
	int formatted, free;
	mc_save_t saves[15];
	int nsaves;
} mc_card_t;

int mc_formatted(const unsigned char *img);
void mc_blank(unsigned char *img);
int mc_parse(const unsigned char *img, mc_card_t *c);
void mc_icon(const unsigned char *img, int slot, unsigned char rgba[16 * 16 * 4]);
/* one save as .mcs (its directory entry, then its blocks): out holds 128 + 15 * 8192 bytes */
int mc_export(const unsigned char *img, int slot, unsigned char *out, size_t *n, const char **err);
int mc_import(unsigned char *img, const unsigned char *mcs, size_t len, const char **err);
int mc_remove(unsigned char *img, int slot, const char **err);

#ifdef __cplusplus
}
#endif

#endif
