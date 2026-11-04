/* ps1_memcard_lib: the memory card library.
 * Copyright (C) 2026 f3bandit. PolyForm Noncommercial 1.0.0 (see LICENSE.md).
 * The memory card manager: f3bandit's idea and design.
 *
 *   <dir>/<card name>/<YYYY-MM-DD_HHMMSS>.mcd          one folder a card, one file a version (raw 128 KB)
 *   <dir>/<card name>/card.json                         {"created":<unix time>,"real":true}
 *
 * A card that's read is matched to a card in the library by the saves on it (the file names in its
 * directory): common saves / all saves of both, 0.5 or more is the same card. A version
 * that's the same as the card's latest isn't kept twice. Backups from before the library (loose .mcd
 * files in <dir>) are still listed and can be put back. */
#include "mclib.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CARD_BYTES (128 * 1024)

static int mkdirs(const char *dir)
{
	char d[1024];
	snprintf(d, sizeof(d), "%s", dir);
	for (char *p = d + 1; *p; p++) {
		if (*p == '/') {
			*p = 0;
			if (mkdir(d, 0755) != 0 && errno != EEXIST) return -1;
			*p = '/';
		}
	}
	if (mkdir(d, 0755) != 0 && errno != EEXIST) return -1;
	return 0;
}

static int dir_exists(const char *p)
{
	struct stat st;
	return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static char mclib_root[512] = "/media/fat/memcards";

/* where the library lives (a folder of card folders); set it once at start-up */
void mclib_set_dir(const char *dir)
{
	snprintf(mclib_root, sizeof(mclib_root), "%s", dir);
}

const char *mclib_dir(void)
{
	return mclib_root;
}

int mclib_safe_name(const char *in, char *out, size_t n)
{
	size_t k = 0;
	const char *p = in ? in : "";
	while (*p == ' ') p++;
	for (; *p && k + 1 < n && k < 60; p++) {
		unsigned char c = (unsigned char)*p;
		if (c < 32 || strchr("/\\:*?\"<>|", c)) continue;      /* what Windows can't have either */
		out[k++] = (char)c;
	}
	while (k && (out[k - 1] == ' ' || out[k - 1] == '.')) k--;
	out[k] = 0;
	return k && strcmp(out, ".") && strcmp(out, "..") ? 0 : -1;
}

/* the save file names on a card (directory frames 1-15 that start a save): up to 15, "" if none */
static int saves(const unsigned char *img, char names[15][21])
{
	if (img[0] != 'M' || img[1] != 'C') return 0;
	int n = 0;
	for (int b = 1; b <= 15; b++) {
		const unsigned char *f = img + b * 128;
		if (f[0] != 0x51) continue;
		int k = 0;
		for (int i = 0x0A; i < 0x1E && f[i] && k < 20; i++) names[n][k++] = (char)f[i];
		while (k && names[n][k - 1] == ' ') k--;
		names[n][k] = 0;
		n++;
	}
	return n;
}

static double looks_alike(const unsigned char *a, const unsigned char *b)
{
	char na[15][21], nb[15][21];
	int ca = saves(a, na), cb = saves(b, nb), common = 0, all = 0;
	/* distinct names of a, then b's against them */
	for (int i = 0; i < ca; i++) {
		int dup = 0;
		for (int j = 0; j < i; j++) if (!strcmp(na[i], na[j])) dup = 1;
		if (!dup) all++;
	}
	for (int i = 0; i < cb; i++) {
		int in = 0;
		for (int j = 0; j < ca; j++) if (!strcmp(nb[i], na[j])) in = 1;
		if (in) common++;
		else all++;
	}
	return all ? (double)common / all : 0;
}

static int is_version(const char *f)
{
	size_t n = strlen(f);
	return n > 4 && f[0] != '.' && !strcasecmp(f + n - 4, ".mcd");
}

/* the newest version of a card (sorted names: the date is the name); 0 if none */
static int latest(const char *card, char *ver, size_t n)
{
	char dir[700];
	snprintf(dir, sizeof(dir), "%s/%s", mclib_dir(), card);
	DIR *d = opendir(dir);
	if (!d) return 0;
	struct dirent *e;
	int found = 0;
	ver[0] = 0;
	while ((e = readdir(d))) {
		if (!is_version(e->d_name)) continue;
		if (!found || strcmp(e->d_name, ver) > 0) snprintf(ver, n, "%s", e->d_name);
		found = 1;
	}
	closedir(d);
	return found;
}

static int load(const char *path, unsigned char *buf)
{
	FILE *f = fopen(path, "rb");
	if (!f) return -1;
	int ok = fread(buf, 1, CARD_BYTES, f) == CARD_BYTES;
	fclose(f);
	return ok ? 0 : -1;
}

double mclib_recognise(const unsigned char *img, char *card, size_t n)
{
	card[0] = 0;
	char tmp[15][21];
	if (!saves(img, tmp)) return 0;          /* an empty card looks like any other empty card */
	DIR *d = opendir(mclib_dir());
	if (!d) return 0;
	unsigned char *other = malloc(CARD_BYTES);
	double best = 0;
	struct dirent *e;
	while (other && (e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		char ver[256], path[1400];
		if (!latest(e->d_name, ver, sizeof(ver))) continue;
		snprintf(path, sizeof(path), "%s/%s/%s", mclib_dir(), e->d_name, ver);
		if (load(path, other)) continue;
		double s = looks_alike(img, other);
		if (s > best) { best = s; snprintf(card, n, "%s", e->d_name); }
	}
	closedir(d);
	free(other);
	return best;
}

int mclib_exists(const char *card)
{
	char dir[700];
	snprintf(dir, sizeof(dir), "%s/%s", mclib_dir(), card);
	return dir_exists(dir);
}

int mclib_store(const char *card, const unsigned char *img, char *rel, size_t rn, int *same)
{
	char dir[700], path[1200], tmp[1300], ver[64];
	*same = 0;
	snprintf(dir, sizeof(dir), "%s/%s", mclib_dir(), card);
	if (mkdirs(dir)) return -1;
	snprintf(path, sizeof(path), "%s/card.json", dir);
	if (access(path, F_OK)) {
		FILE *f = fopen(path, "w");
		if (f) { fprintf(f, "{\"created\":%ld,\"real\":true}", (long)time(NULL)); fclose(f); }
	}
	/* the same as the latest version: nothing new to keep */
	char last[256];
	if (latest(card, last, sizeof(last))) {
		unsigned char *old = malloc(CARD_BYTES);
		snprintf(path, sizeof(path), "%s/%s", dir, last);
		int eq = old && !load(path, old) && !memcmp(old, img, CARD_BYTES);
		free(old);
		if (eq) { snprintf(rel, rn, "%s/%s", card, last); *same = 1; return 0; }
	}
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	strftime(ver, sizeof(ver), "%Y-%m-%d_%H%M%S", &tm);
	char name[96];
	for (int i = 1; i < 100; i++) {
		if (i == 1) snprintf(name, sizeof(name), "%s.mcd", ver);
		else snprintf(name, sizeof(name), "%s_%d.mcd", ver, i);
		snprintf(path, sizeof(path), "%s/%s", dir, name);
		if (access(path, F_OK)) break;
	}
	snprintf(tmp, sizeof(tmp), "%s.part", path);
	FILE *f = fopen(tmp, "wb");
	if (!f) return -1;
	int ok = fwrite(img, 1, CARD_BYTES, f) == CARD_BYTES;
	ok = (fflush(f) == 0) && ok;
	ok = (fsync(fileno(f)) == 0) && ok;
	ok = (fclose(f) == 0) && ok;
	if (!ok || rename(tmp, path)) { unlink(tmp); return -1; }
	snprintf(rel, rn, "%s/%s", card, name);
	return 0;
}

int mclib_valid(const char *rel)
{
	if (!rel || !*rel || rel[0] == '.' || rel[0] == '/' || strstr(rel, "..") || strchr(rel, '\\')) return 0;
	const char *s = strchr(rel, '/');
	if (s && strchr(s + 1, '/')) return 0;                 /* card/version.mcd, or a loose old file */
	return is_version(s ? s + 1 : rel) && strlen(rel) < 200;
}

typedef struct { char rel[200]; long size; time_t mt; } ent_t;
static int by_time(const void *a, const void *b)
{
	const ent_t *x = a, *y = b;
	return x->mt < y->mt ? 1 : x->mt > y->mt ? -1 : strcmp(y->rel, x->rel);
}

static void add(ent_t **e, size_t *n, size_t *cap, const char *rel, const char *path)
{
	struct stat sb;
	if (stat(path, &sb) || !S_ISREG(sb.st_mode) || sb.st_size != CARD_BYTES) return;
	if (*n == *cap) {
		ent_t *ne = realloc(*e, (*cap *= 2) * sizeof(**e));
		if (!ne) return;
		*e = ne;
	}
	snprintf((*e)[*n].rel, sizeof((*e)[*n].rel), "%s", rel);
	(*e)[*n].size = (long)sb.st_size;
	(*e)[*n].mt = sb.st_mtime;
	(*n)++;
}

char *mclib_list(void)
{
	size_t cap = 32, n = 0;
	ent_t *e = malloc(cap * sizeof(*e));
	if (!e) return NULL;
	DIR *d = opendir(mclib_dir());
	struct dirent *de;
	while (d && (de = readdir(d))) {
		if (de->d_name[0] == '.') continue;
		char p[800];
		snprintf(p, sizeof(p), "%s/%s", mclib_dir(), de->d_name);
		if (dir_exists(p)) {
			DIR *c = opendir(p);
			struct dirent *ve;
			while (c && (ve = readdir(c))) {
				if (!is_version(ve->d_name)) continue;
				char rel[400], vp[1200];
				snprintf(rel, sizeof(rel), "%s/%s", de->d_name, ve->d_name);
				snprintf(vp, sizeof(vp), "%s/%s", p, ve->d_name);
				if (strlen(rel) < 200) add(&e, &n, &cap, rel, vp);
			}
			if (c) closedir(c);
		} else if (is_version(de->d_name) && strlen(de->d_name) < 200) {
			add(&e, &n, &cap, de->d_name, p);
		}
	}
	if (d) closedir(d);
	if (n) qsort(e, n, sizeof(*e), by_time);
	char *out = malloc(n * 240 + 1);
	if (out) {
		size_t k = 0;
		out[0] = 0;
		for (size_t i = 0; i < n; i++) k += (size_t)sprintf(out + k, "%s\t%ld\t%ld\n", e[i].rel, e[i].size, (long)e[i].mt);
	}
	free(e);
	return out;
}

void mclib_suggest(char *out, size_t n)
{
	for (int i = 1; i < 1000; i++) {
		snprintf(out, n, "Memory card %d", i);
		if (!mclib_exists(out)) return;
	}
}

/* a version of a card into buf: ver "" or NULL for the newest; its file name in got (may be NULL) */
int mclib_load(const char *card, const char *ver, unsigned char *buf, char *got, size_t gn)
{
	char v[256], path[1200];
	if (!card || !*card || strchr(card, '/') || strstr(card, "..")) return -1;
	if (ver && *ver) {
		if (strchr(ver, '/') || strstr(ver, "..")) return -1;
		snprintf(v, sizeof(v), "%s%s", ver, strstr(ver, ".mcd") ? "" : ".mcd");
	} else if (!latest(card, v, sizeof(v))) return -1;
	snprintf(path, sizeof(path), "%s/%s/%s", mclib_dir(), card, v);
	if (load(path, buf)) return -1;
	if (got) snprintf(got, gn, "%s", v);
	return 0;
}

/* a card's versions, oldest first ("2026-10-23_221500.mcd"), n of them; caller frees */
char **mclib_versions(const char *card, int *n)
{
	char d[700];
	*n = 0;
	snprintf(d, sizeof(d), "%s/%s", mclib_dir(), card);
	DIR *dd = opendir(d);
	if (!dd) return NULL;
	char **v = NULL;
	struct dirent *e;
	while ((e = readdir(dd))) {
		if (!is_version(e->d_name)) continue;
		char **nv = realloc(v, sizeof(char *) * (size_t)(*n + 1));
		if (!nv) break;
		v = nv;
		v[(*n)++] = strdup(e->d_name);
	}
	closedir(dd);
	for (int i = 0; i < *n; i++)
		for (int j = i + 1; j < *n; j++)
			if (strcmp(v[i], v[j]) > 0) { char *t = v[i]; v[i] = v[j]; v[j] = t; }
	return v;
}

/* "real": a card read from a port (card.json), not one made here */
int mclib_real(const char *card)
{
	char p[800], b[200] = "";
	snprintf(p, sizeof(p), "%s/%s/card.json", mclib_dir(), card);
	FILE *f = fopen(p, "r");
	if (!f) return 0;
	if (!fgets(b, sizeof(b), f)) b[0] = 0;
	fclose(f);
	return strstr(b, "\"real\":true") != NULL;
}

void mclib_mark_made(const char *card)
{
	char p[800];
	snprintf(p, sizeof(p), "%s/%s/card.json", mclib_dir(), card);
	FILE *f = fopen(p, "w");
	if (f) { fprintf(f, "{\"created\":%ld,\"real\":false}", (long)time(NULL)); fclose(f); }
}
