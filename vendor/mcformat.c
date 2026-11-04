/* ps1_memcard_lib: PlayStation memory card images (raw 128 KB, MiSTer's .mcd / .sav), 
 * read and changed the same way everywhere.
 * Copyright (C) 2026 f3bandit. PolyForm Noncommercial 1.0.0 (see LICENSE.md); Retro Remake: see LICENSE-RETRO-REMAKE.md.
 * The memory card manager: f3bandit's idea and design.
 *
 * A card is 16 blocks of 8 KB; each block is 64 frames of 128 bytes. Block 0 is the card's own
 * bookkeeping: frame 0 the "MC" header, frames 1-15 one directory entry per block 1-15 (state, size,
 * next block, file name), 16-35 the broken sector list, 63 a copy of the header. Every frame of block 0
 * ends with the XOR of its first 127 bytes. A save starts in a block whose entry says 0x51, continues
 * through 0x52 blocks and ends with 0x53; each entry points to the next block (0xFFFF: the last). 0xA0
 * is free, 0xA1-0xA3 deleted. A save's first block starts with "SC", its title (Shift-JIS) and its
 * 16x16 icon in 16 colours. */
#include "mcformat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAME 128

static unsigned char *dir(unsigned char *img, int b) { return img + b * FRAME; }
static const unsigned char *cdir(const unsigned char *img, int b) { return img + b * FRAME; }
static unsigned u16(const unsigned char *p) { return p[0] | p[1] << 8; }
static unsigned u32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }
static void put16(unsigned char *p, unsigned v) { p[0] = v & 255; p[1] = v >> 8 & 255; }
static void put32(unsigned char *p, unsigned v) { p[0] = v & 255; p[1] = v >> 8 & 255; p[2] = v >> 16 & 255; p[3] = v >> 24 & 255; }

static void checksum(unsigned char *f)
{
	unsigned char c = 0;
	for (int i = 0; i < 127; i++) c ^= f[i];
	f[127] = c;
}

static int is_free(unsigned char st) { return st == 0xA0 || (st >= 0xA1 && st <= 0xA3); }

int mc_formatted(const unsigned char *img) { return img[0] == 'M' && img[1] == 'C'; }

void mc_blank(unsigned char *img)
{
	memset(img, 0, MC_SIZE);
	img[0] = 'M';
	img[1] = 'C';
	checksum(img);
	for (int b = 1; b <= 15; b++) {
		unsigned char *f = dir(img, b);
		f[0] = 0xA0;
		f[8] = f[9] = 0xFF;
		checksum(f);
	}
	for (int fr = 16; fr <= 35; fr++) {
		unsigned char *f = img + fr * FRAME;
		put32(f, 0xFFFFFFFF);
		f[8] = f[9] = 0xFF;
		checksum(f);
	}
	memcpy(img + 63 * FRAME, img, FRAME);
}

static void cstr(const unsigned char *b, int n, char *out, size_t on)
{
	int k = 0;
	while (k < n && b[k]) k++;
	while (k > 0 && b[k - 1] == ' ') k--;
	int s = 0;
	while (s < k && b[s] == ' ') s++;
	snprintf(out, on, "%.*s", k - s, (const char *)b + s);
	for (char *p = out; *p; p++) if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7e) *p = '?';
}

/* Shift-JIS title to plain text: full-width letters and digits become normal ones, Japanese "?" */
static void sjis(const unsigned char *b, int n, char *out, size_t on)
{
	static const struct { unsigned v; char c; } P[] = {
		{ 0x8140, ' ' }, { 0x8141, ',' }, { 0x8142, '.' }, { 0x8143, ',' }, { 0x8144, '.' }, { 0x8145, '-' }, { 0x8146, ':' },
		{ 0x8147, ';' }, { 0x8148, '?' }, { 0x8149, '!' }, { 0x814B, '\'' }, { 0x814C, '\'' }, { 0x814F, '^' }, { 0x8151, '_' },
		{ 0x815B, '-' }, { 0x815C, '-' }, { 0x815D, '-' }, { 0x815E, '/' }, { 0x815F, '\\' }, { 0x8160, '~' }, { 0x8162, '|' },
		{ 0x8165, '\'' }, { 0x8166, '\'' }, { 0x8167, '"' }, { 0x8168, '"' }, { 0x8169, '(' }, { 0x816A, ')' }, { 0x816D, '[' },
		{ 0x816E, ']' }, { 0x816F, '{' }, { 0x8170, '}' }, { 0x817B, '+' }, { 0x817C, '-' }, { 0x8181, '=' }, { 0x8183, '<' },
		{ 0x8184, '>' }, { 0x818F, '$' }, { 0x8193, '%' }, { 0x8194, '#' }, { 0x8195, '&' }, { 0x8196, '*' }, { 0x8197, '@' }, { 0x8190, '$' },
	};
	char t[160];
	size_t k = 0;
	for (int i = 0; i < n && b[i] && k + 1 < sizeof(t); i++) {
		unsigned char c = b[i];
		if (c < 0x80) { t[k++] = (char)(c >= 0x20 ? c : ' '); continue; }
		if (c >= 0xA1 && c <= 0xDF) { t[k++] = '?'; continue; }
		if (i + 1 >= n) break;
		unsigned v = (unsigned)c << 8 | b[++i];
		char o = '?';
		if (v >= 0x824F && v <= 0x8258) o = (char)('0' + v - 0x824F);
		else if (v >= 0x8260 && v <= 0x8279) o = (char)('A' + v - 0x8260);
		else if (v >= 0x8281 && v <= 0x829A) o = (char)('a' + v - 0x8281);
		else for (size_t p = 0; p < sizeof(P) / sizeof(P[0]); p++) if (P[p].v == v) { o = P[p].c; break; }
		t[k++] = o;
	}
	t[k] = 0;
	/* spaces squeezed, "???" runs shortened */
	char u[160];
	size_t j = 0;
	int sp = 1;
	for (size_t i = 0; t[i] && j + 1 < sizeof(u); i++) {
		if (t[i] == ' ') { if (!sp) u[j++] = ' '; sp = 1; continue; }
		if (t[i] == '?' && j >= 2 && u[j - 1] == '?' && u[j - 2] == '?') continue;
		u[j++] = t[i];
		sp = 0;
	}
	while (j && u[j - 1] == ' ') j--;
	u[j] = 0;
	if (on) { size_t l = strlen(u) < on - 1 ? strlen(u) : on - 1; memcpy(out, u, l); out[l] = 0; }
}

int mc_parse(const unsigned char *img, mc_card_t *c)
{
	memset(c, 0, sizeof(*c));
	c->formatted = mc_formatted(img);
	if (!c->formatted) return 0;
	for (int b = 1; b <= 15; b++) if (is_free(cdir(img, b)[0])) c->free++;
	for (int b = 1; b <= 15; b++) {
		const unsigned char *f = cdir(img, b);
		if (f[0] != 0x51) continue;
		mc_save_t *s = &c->saves[c->nsaves++];
		s->slot = b;
		cstr(f + 0x0A, 20, s->name, sizeof(s->name));
		if (strlen(s->name) >= 12) {
			memcpy(s->product, s->name + 2, 10);
			s->product[10] = 0;
			strcpy(s->region, s->name[1] == 'A' ? "USA" : s->name[1] == 'E' ? "Europe" : s->name[1] == 'I' ? "Japan" : "");
		}
		int seen[16] = { 0 };
		for (int cur = b;;) {
			if (cur < 1 || cur > 15 || seen[cur]) { s->broken = 1; break; }
			/* past the first block, only a middle (0x52) or last (0x53) block of a save: a damaged
			   card's link into another save's block stops here */
			if (cur != b && cdir(img, cur)[0] != 0x52 && cdir(img, cur)[0] != 0x53) { s->broken = 1; break; }
			seen[cur] = 1;
			s->blocks[s->nblocks++] = cur;
			unsigned next = u16(cdir(img, cur) + 8);
			if (next == 0xFFFF) break;
			cur = (int)next + 1;
		}
		if (u32(f + 4) != (unsigned)s->nblocks * MC_BLOCK) s->broken = 1;
		const unsigned char *data = img + b * MC_BLOCK;
		if (data[0] == 'S' && data[1] == 'C') sjis(data + 4, 64, s->title, sizeof(s->title));
		if (!s->title[0]) memcpy(s->title, s->name, sizeof(s->name));   /* (name is the shorter) */
	}
	return 0;
}

void mc_icon(const unsigned char *img, int slot, unsigned char rgba[16 * 16 * 4])
{
	const unsigned char *block = img + slot * MC_BLOCK;
	memset(rgba, 0, 16 * 16 * 4);
	if (block[0] != 'S' || block[1] != 'C') return;
	unsigned char pal[16][4];
	for (int i = 0; i < 16; i++) {
		unsigned v = u16(block + 0x60 + i * 2);
		unsigned r = v & 31, g = v >> 5 & 31, b = v >> 10 & 31;
		pal[i][0] = (unsigned char)(r << 3 | r >> 2);
		pal[i][1] = (unsigned char)(g << 3 | g >> 2);
		pal[i][2] = (unsigned char)(b << 3 | b >> 2);
		pal[i][3] = v ? 255 : 0;
	}
	for (int y = 0; y < 16; y++)
		for (int x = 0; x < 16; x++) {
			unsigned v = block[0x80 + y * 8 + x / 2];
			if (x & 1) v >>= 4;
			memcpy(rgba + (y * 16 + x) * 4, pal[v & 15], 4);
		}
}

static const mc_save_t *find(const mc_card_t *c, int slot)
{
	for (int i = 0; i < c->nsaves; i++) if (c->saves[i].slot == slot) return &c->saves[i];
	return NULL;
}

int mc_export(const unsigned char *img, int slot, unsigned char *out, size_t *n, const char **err)
{
	mc_card_t c;
	mc_parse(img, &c);
	const mc_save_t *s = find(&c, slot);
	if (!s) { *err = "no save starts in that block"; return -1; }
	if (s->broken) { *err = "this save's blocks are broken on the card"; return -1; }
	memcpy(out, cdir(img, slot), FRAME);
	put16(out + 8, 0xFFFF);
	checksum(out);
	for (int i = 0; i < s->nblocks; i++) memcpy(out + FRAME + i * MC_BLOCK, img + s->blocks[i] * MC_BLOCK, MC_BLOCK);
	*n = FRAME + (size_t)s->nblocks * MC_BLOCK;
	return 0;
}

int mc_import(unsigned char *img, const unsigned char *mcs, size_t len, const char **err)
{
	if (!mc_formatted(img)) { *err = "the card isn't formatted"; return -1; }
	if (len < FRAME + MC_BLOCK || (len - FRAME) % MC_BLOCK) { *err = "not a save file (.mcs)"; return -1; }
	int n = (int)((len - FRAME) / MC_BLOCK);
	char name[32];
	cstr(mcs + 0x0A, 20, name, sizeof(name));
	if (!name[0]) { *err = "the save has no name"; return -1; }
	mc_card_t c;
	mc_parse(img, &c);
	for (int i = 0; i < c.nsaves; i++) if (!strcmp(c.saves[i].name, name)) { *err = "the card already has this save"; return -1; }
	int fr[15], nf = 0;
	for (int b = 1; b <= 15 && nf < n; b++) if (is_free(cdir(img, b)[0])) fr[nf++] = b;
	if (nf < n) { *err = "not enough free blocks on the card"; return -1; }
	for (int i = 0; i < n; i++) {
		unsigned char *f = dir(img, fr[i]);
		memset(f, 0, FRAME);
		if (i == 0) {
			f[0] = 0x51;
			put32(f + 4, (unsigned)n * MC_BLOCK);
			memcpy(f + 0x0A, mcs + 0x0A, 20);
		} else f[0] = i == n - 1 ? 0x53 : 0x52;
		put16(f + 8, i == n - 1 ? 0xFFFF : (unsigned)(fr[i + 1] - 1));
		checksum(f);
		memcpy(img + fr[i] * MC_BLOCK, mcs + FRAME + i * MC_BLOCK, MC_BLOCK);
	}
	return 0;
}

int mc_remove(unsigned char *img, int slot, const char **err)
{
	mc_card_t c;
	mc_parse(img, &c);
	const mc_save_t *s = find(&c, slot);
	if (!s) { *err = "no save starts in that block"; return -1; }
	for (int i = 0; i < s->nblocks; i++) {
		/* on a damaged card two saves' links can lead into the same block: that one is kept */
		int shared = 0;
		for (int j = 0; j < c.nsaves && !shared; j++)
			if (&c.saves[j] != s) for (int k = 0; k < c.saves[j].nblocks; k++) if (c.saves[j].blocks[k] == s->blocks[i]) shared = 1;
		if (shared) continue;
		unsigned char *f = dir(img, s->blocks[i]);
		memset(f, 0, FRAME);
		f[0] = 0xA0;
		f[8] = f[9] = 0xFF;
		checksum(f);
		memset(img + s->blocks[i] * MC_BLOCK, 0, MC_BLOCK);
	}
	return 0;
}
