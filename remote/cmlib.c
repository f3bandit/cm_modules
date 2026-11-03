/* cmremote: Console Mode's game library for the apps.
 * Copyright (C) 2026 f3bandit. GPL-3.0-or-later (see ../LICENSE); Retro Remake: see ../LICENSE-RETRO-REMAKE.md.
 *
 * Read the way Console Mode 1.2.5 reads it, nothing of its own kept:
 *   systems   themeconfig/section_groups/<group>.ini: [NAME] romExts, romDirs (each /media/fat/... folder is
 *             also looked for on the USB/NVMe drives, as Console Mode does)
 *   games     the files in those folders, with Console Mode's rule for folders: a folder with one disc image,
 *             or nothing but numbered discs, is one game under the folder's name; any other folder holds
 *             games of its own (folderHoldsOneGame, src/FileManager.cpp)
 *   box art   <the game's folder>/media/<title>.png, else <the system's folder>/media/<title>.png
 *   details   gamelist.xml in the system's folder (EmulationStation's format; Console Mode reads it with
 *             Settings > Game Library > Import gamelist.xml)
 *   favourites, history   gamelist.ini and playtime/history.tsv (read only: Console Mode rewrites them)
 * Ids are a hash of where the game is, so they stay the same from one scan to the next. */
#define _GNU_SOURCE
#include "cmlib.h"
#include "sha256.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_FAILURE_USERMSG
#include "vendor/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "vendor/stb_image_write.h"

const char *cm_dir = "/media/fat/ConsoleMode";
const char *cm_fat = "/media/fat";
const char *cm_drives[] = { "/media/fat", "/media/usb0", "/media/usb1", "/media/usb2", "/media/usb3", "/media/usb4",
                            "/media/usb5", "/media/usb6", "/media/usb7", NULL };

#define SCAN_MAX_AGE 600      /* seconds a scan is used for, unless asked to look again */
#define ART_WIDTH 512         /* Console Mode's own box art is 512 wide */
#define ART_MAX_PIXELS (40L * 1000 * 1000)

enum { CAT_DISC, CAT_CARTRIDGE, CAT_HANDHELD, CAT_ARCADE, CAT_COMPUTER };
static const char *CAT_NAMES[] = { "Disc", "Cartridge", "Handheld", "Arcade", "Computer" };

typedef struct {
	char *name;               /* Console Mode's name, e.g. "GAMEBOY", "PLAYSTATION" */
	char *exts;               /* ",.zip,.gb," lower case, commas around each */
	char **dirs;              /* romDirs */
	int ndirs;
	int cat;
	char *group;              /* the section_groups file it's in: "Console", "Handheld"... (Console Mode's Load Game list) */
	int games;
} sys_t;

typedef struct {
	long id;
	int sys;
	char *title;
	char *file;               /* the file to start */
	char *unit;               /* the game as Console Mode lists it: the file, or a disc set's folder */
	char *root;               /* the system's folder it's in (media/, gamelist.xml) */
} game_t;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static sys_t *systems;
static int nsys;
static game_t *games;
static int ngames, cap;
static time_t scanned_at;
static double generation;
static uint32_t content_hash;

static cJSON *ok(void) { cJSON *o = cJSON_CreateObject(); cJSON_AddBoolToObject(o, "ok", 1); return o; }
static cJSON *fail(const char *msg)
{
	cJSON *o = cJSON_CreateObject();
	cJSON_AddBoolToObject(o, "ok", 0);
	cJSON_AddStringToObject(o, "error", msg);
	return o;
}

long cm_game_id(const char *path)
{
	uint32_t h = 2166136261u;
	for (const unsigned char *c = (const unsigned char *)path; *c; c++) { h ^= *c; h *= 16777619u; }
	h &= 0x7fffffff;
	return h ? (long)h : 1;
}

static char *trim(char *s)
{
	while (isspace((unsigned char)*s)) s++;
	char *e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
	return s;
}

static const char *ext_of(const char *name)
{
	const char *d = strrchr(name, '.');
	return d && d != name ? d : "";
}

static int has_ext(const sys_t *s, const char *name)
{
	const char *x = ext_of(name);
	if (!*x || strlen(x) > 15) return 0;
	char k[20];
	snprintf(k, sizeof(k), ",%s,", x);
	for (char *c = k; *c; c++) *c = (char)tolower((unsigned char)*c);
	return strstr(s->exts, k) != NULL;
}

static void strip_ext(const char *name, char *out, size_t n);

/* ---------- systems: the section_groups ini files ---------- */

static void free_systems(void)
{
	for (int i = 0; i < nsys; i++) {
		free(systems[i].name);
		free(systems[i].exts);
		free(systems[i].group);
		for (int k = 0; k < systems[i].ndirs; k++) free(systems[i].dirs[k]);
		free(systems[i].dirs);
	}
	free(systems);
	systems = NULL;
	nsys = 0;
}

static int group_cat(const char *file)
{
	if (!strncasecmp(file, "Handheld", 8)) return CAT_HANDHELD;
	if (!strncasecmp(file, "Arcade", 6)) return CAT_ARCADE;
	if (!strncasecmp(file, "Computer", 8)) return CAT_COMPUTER;
	if (!strncasecmp(file, "Ports", 5)) return CAT_COMPUTER;
	return CAT_CARTRIDGE;
}

static void load_group(const char *path, int cat, const char *group)
{
	FILE *f = fopen(path, "r");
	if (!f) return;
	char line[4096];
	sys_t *cur = NULL;
	while (fgets(line, sizeof(line), f)) {
		char *s = trim(line);
		if (*s == '[') {
			char *e = strchr(s, ']');
			if (!e) continue;
			*e = 0;
			cur = NULL;
			if (!strcasecmp(s + 1, "CONSOLES")) continue;
			sys_t *n = realloc(systems, (size_t)(nsys + 1) * sizeof(*systems));
			if (!n) continue;
			systems = n;
			cur = &systems[nsys++];
			memset(cur, 0, sizeof(*cur));
			cur->name = strdup(s + 1);
			cur->exts = strdup(",");
			cur->cat = cat;
			cur->group = strdup(group);
			continue;
		}
		char *eq = strchr(s, '=');
		if (!cur || !eq) continue;
		*eq = 0;
		char *k = trim(s), *v = trim(eq + 1);
		if (!strcasecmp(k, "romExts")) {
			free(cur->exts);
			size_t n = strlen(v) + 3;
			cur->exts = malloc(n);
			snprintf(cur->exts, n, ",%s,", v);
			for (char *c = cur->exts; *c; c++) { *c = (char)tolower((unsigned char)*c); if (*c == ' ') *c = ','; }
			if (strstr(cur->exts, ",.cue,") || strstr(cur->exts, ",.chd,")) if (cur->cat == CAT_CARTRIDGE) cur->cat = CAT_DISC;
		} else if (!strcasecmp(k, "romDirs")) {
			char *save = NULL;
			for (char *d = strtok_r(v, ",", &save); d; d = strtok_r(NULL, ",", &save)) {
				d = trim(d);
				if (!*d) continue;
				char **nd = realloc(cur->dirs, (size_t)(cur->ndirs + 1) * sizeof(char *));
				if (!nd) break;
				cur->dirs = nd;
				cur->dirs[cur->ndirs++] = strdup(d);
			}
		}
	}
	fclose(f);
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void load_systems(void)
{
	free_systems();
	char dir[600];
	snprintf(dir, sizeof(dir), "%s/themeconfig/section_groups", cm_dir);
	DIR *d = opendir(dir);
	if (!d) return;
	char *names[32];
	int n = 0;
	struct dirent *e;
	while ((e = readdir(d)) && n < 32)
		if (e->d_name[0] != '.' && !strcasecmp(ext_of(e->d_name), ".ini")) names[n++] = strdup(e->d_name);
	closedir(d);
	qsort(names, (size_t)n, sizeof(char *), cmp_str);
	for (int i = 0; i < n; i++) {
		char p[900];
		snprintf(p, sizeof(p), "%s/%s", dir, names[i]);
		char g[256];
		strip_ext(names[i], g, sizeof(g));
		load_group(p, group_cat(names[i]), g);
		free(names[i]);
	}
}

/* ---------- games ---------- */

typedef struct { char *p; int dir; } ent_t;       /* relative to the system's folder */
typedef struct { ent_t *v; int n, cap; } ents_t;

static void ents_add(ents_t *e, const char *p, int dir)
{
	if (e->n == e->cap) {
		int nc = e->cap ? e->cap * 2 : 256;
		ent_t *nv = realloc(e->v, (size_t)nc * sizeof(ent_t));
		if (!nv) return;
		e->v = nv;
		e->cap = nc;
	}
	e->v[e->n].p = strdup(p);
	e->v[e->n].dir = dir;
	e->n++;
}

static int skip_dir(const char *name)
{
	return name[0] == '.' || !strcasecmp(name, "bios") || !strcasecmp(name, "media");
}

static void walk(const char *root, const char *rel, int depth, ents_t *out)
{
	char p[1400];
	snprintf(p, sizeof(p), "%s%s%s", root, *rel ? "/" : "", rel);
	DIR *d = opendir(p);
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		char r[1400];
		snprintf(r, sizeof(r), "%s%s%s", rel, *rel ? "/" : "", e->d_name);
		int isdir = e->d_type == DT_DIR;
		if (e->d_type == DT_UNKNOWN || e->d_type == DT_LNK) {
			char full[2900];
			struct stat sb;
			snprintf(full, sizeof(full), "%s/%s", p, e->d_name);
			if (stat(full, &sb)) continue;
			isdir = S_ISDIR(sb.st_mode);
		}
		if (isdir) {
			if (skip_dir(e->d_name)) continue;
			ents_add(out, r, 1);
			if (depth < 6) walk(root, r, depth + 1, out);
		} else ents_add(out, r, 0);
	}
	closedir(d);
}

static const char *base_of(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }

static int disc_tag(const char *name)
{
	char l[512];
	snprintf(l, sizeof(l), "%s", name);
	for (char *c = l; *c; c++) *c = (char)tolower((unsigned char)*c);
	return strstr(l, "(disc") || strstr(l, "(disk") || strstr(l, "(cd ");
}

static int disc_ext(const char *x)
{
	static const char *D[] = { ".cue", ".chd", ".iso", ".ccd", ".gdi", ".cdi", ".pbp", ".cso", ".mds", NULL };
	for (int i = 0; D[i]; i++) if (!strcasecmp(x, D[i])) return 1;
	return 0;
}

static int track_ext(const char *x) { return !strcasecmp(x, ".bin") || !strcasecmp(x, ".img") || !strcasecmp(x, ".m3u"); }

/* the parent folder of a relative path, into out ("" for the system's own) */
static void parent_of(const char *p, char *out, size_t n)
{
	const char *s = strrchr(p, '/');
	snprintf(out, n, "%.*s", s ? (int)(s - p) : 0, p);
}

static int is_game(const sys_t *s, const char *p) { return base_of(p)[0] != '.' && has_ext(s, p); }

/* Console Mode's folderHoldsOneGame */
static int one_game(const sys_t *s, const ents_t *e, const char *d)
{
	size_t dl = strlen(d);
	int launch = 0, untagged = 0, disc = 0;
	for (int i = 0; i < e->n; i++) {
		char par[1400];
		parent_of(e->v[i].p, par, sizeof(par));
		if (strcmp(par, d)) continue;
		if (e->v[i].dir) { if (!skip_dir(e->v[i].p + dl + 1)) return 0; continue; }
		if (!is_game(s, e->v[i].p)) continue;
		const char *x = ext_of(e->v[i].p);
		if (track_ext(x)) continue;
		launch++;
		if (!disc_tag(base_of(e->v[i].p))) untagged++;
		if (disc_ext(x)) disc = 1;
	}
	if (launch == 1) return disc;
	return launch > 0 && untagged == 0;
}

static int cmp_ent(const void *a, const void *b) { return strcmp(((const ent_t *)a)->p, ((const ent_t *)b)->p); }

static void add_game(int sys, const char *root, const char *title, const char *file_rel, const char *unit_rel)
{
	if (ngames == cap) {
		int nc = cap ? cap * 2 : 4096;
		game_t *ng = realloc(games, (size_t)nc * sizeof(game_t));
		if (!ng) return;
		games = ng;
		cap = nc;
	}
	game_t *g = &games[ngames];
	char p[2900];
	g->sys = sys;
	char t[512];
	snprintf(t, sizeof(t), "%s", title);
	if (!strcasecmp(ext_of(file_rel), ".mra")) {
		/* an arcade game: Console Mode shows its name without the "(World 900227)" after it */
		char *pb;
		while ((pb = strrchr(t, '(')) && pb > t && pb[-1] == ' ' && strchr(pb, ')')) { pb[-1] = 0; }
	}
	g->title = strdup(t);
	snprintf(p, sizeof(p), "%s/%s", root, file_rel);
	g->file = strdup(p);
	snprintf(p, sizeof(p), "%s/%s", root, unit_rel);
	g->unit = strdup(p);
	g->root = strdup(root);
	g->id = cm_game_id(g->unit);
	ngames++;
	content_hash = (content_hash ^ (uint32_t)g->id) * 16777619u;
}

static void strip_ext(const char *name, char *out, size_t n)
{
	snprintf(out, n, "%s", name);
	char *d = strrchr(out, '.');
	if (d && d != out) *d = 0;
}

static void units(int si, const char *root, ents_t *e, const char *d)
{
	const sys_t *s = &systems[si];
	size_t dl = strlen(d);
	/* files first, then folders, each in name order (entries are sorted) */
	for (int pass = 0; pass < 2; pass++)
		for (int i = 0; i < e->n; i++) {
			char par[1400];
			parent_of(e->v[i].p, par, sizeof(par));
			if (strcmp(par, d) || e->v[i].dir != pass) continue;
			const char *p = e->v[i].p;
			if (!pass) {
				if (!is_game(s, p)) continue;
				char t[512];
				strip_ext(base_of(p), t, sizeof(t));
				add_game(si, root, t, p, p);
				continue;
			}
			if (skip_dir(p + (dl ? dl + 1 : 0))) continue;
			if (!one_game(s, e, p)) { units(si, root, e, p); continue; }
			/* a disc set: started from its first .cue/.chd, else its first file */
			const char *file = NULL;
			size_t pl = strlen(p);
			for (int k = 0; k < e->n; k++) {
				const char *f = e->v[k].p;
				if (e->v[k].dir || strncmp(f, p, pl) || f[pl] != '/' || strchr(f + pl + 1, '/') || !is_game(s, f)) continue;
				const char *x = ext_of(f), *cx = file ? ext_of(file) : "";
				if (!file || ((!strcasecmp(x, ".cue") || !strcasecmp(x, ".chd")) && strcasecmp(cx, ".cue") && strcasecmp(cx, ".chd"))) file = f;
			}
			if (file) add_game(si, root, base_of(p), file, p);
		}
}

static void free_games(void)
{
	for (int i = 0; i < ngames; i++) { free(games[i].title); free(games[i].file); free(games[i].unit); free(games[i].root); }
	ngames = 0;
}

static int is_dir(const char *p) { struct stat sb; return !stat(p, &sb) && S_ISDIR(sb.st_mode); }

static int cmp_game(const void *a, const void *b)
{
	const game_t *x = a, *y = b;
	if (x->sys != y->sys) return x->sys - y->sys;
	int c = strcasecmp(x->title, y->title);
	return c ? c : strcmp(x->unit, y->unit);
}

static void scan_system(int si)
{
	sys_t *s = &systems[si];
	int start = ngames;
	for (int k = 0; k < s->ndirs; k++) {
		const char *d = s->dirs[k];
		/* romDirs name the SD card as /media/fat; the same folder is looked for on every drive */
		const char *SD = "/media/fat";
		size_t fl = strlen(SD);
		char rel[1024];
		snprintf(rel, sizeof(rel), "%s", strncmp(d, SD, fl) ? d : d + fl);
		size_t rl = strlen(rel);
		while (rl > 1 && rel[rl - 1] == '/') rel[--rl] = 0;
		/* a file (an .mgl, a core): one game */
		if (strncmp(d, SD, fl) || d[strlen(d) - 1] != '/') {
			char full[1400];
			snprintf(full, sizeof(full), "%s%s", strncmp(d, SD, fl) ? "" : cm_fat, rel);
			struct stat sb;
			if (!stat(full, &sb) && S_ISREG(sb.st_mode)) {
				char root[1400], t[512];
				parent_of(full, root, sizeof(root));
				strip_ext(base_of(full), t, sizeof(t));
				add_game(si, root, t, base_of(full), base_of(full));
			}
			continue;
		}
		for (int dv = 0; cm_drives[dv]; dv++) {
			char root[1400];
			snprintf(root, sizeof(root), "%s%s", cm_drives[dv], rel);
			if (!is_dir(root)) continue;
			ents_t e = { 0 };
			walk(root, "", 0, &e);
			qsort(e.v, (size_t)e.n, sizeof(ent_t), cmp_ent);
			units(si, root, &e, "");
			for (int i = 0; i < e.n; i++) free(e.v[i].p);
			free(e.v);
		}
	}
	s->games = ngames - start;
}

static void scan(void)
{
	uint32_t before = content_hash;
	free_games();
	content_hash = 2166136261u;
	load_systems();
	int arcade = 0;
	for (int si = 0; si < nsys; si++) {
		scan_system(si);
		if (systems[si].cat == CAT_ARCADE) arcade += systems[si].games;
	}
	/* no arcade collections sorted by platform (_Arcade/_Organized): Console Mode lists _Arcade itself, as
	   one system called Arcade */
	char ad[700];
	snprintf(ad, sizeof(ad), "%s/_Arcade", cm_fat);
	if (!arcade && is_dir(ad)) {
		sys_t *n = realloc(systems, (size_t)(nsys + 1) * sizeof(*systems));
		if (n) {
			systems = n;
			sys_t *a = &systems[nsys++];
			memset(a, 0, sizeof(*a));
			a->name = strdup("Arcade");
			a->exts = strdup(",.mra,");
			a->cat = CAT_ARCADE;
			a->group = strdup("Arcade");
			a->dirs = malloc(sizeof(char *));
			a->dirs[0] = strdup("/media/fat/_Arcade/");
			a->ndirs = 1;
			scan_system(nsys - 1);
		}
	}
	qsort(games, (size_t)ngames, sizeof(game_t), cmp_game);
	scanned_at = time(NULL);
	if (content_hash != before) generation++;
}

static void fresh(void)
{
	if (!scanned_at || time(NULL) - scanned_at > SCAN_MAX_AGE) scan();
}

static game_t *find_game(long id)
{
	for (int i = 0; i < ngames; i++) if (games[i].id == id) return &games[i];
	return NULL;
}

/* ---------- box art ---------- */

/* arcade games: Console Mode keeps their art in /media/fat/media, named after the .mra file (with -BG
   backgrounds beside it and its optimized/ copies), not in the system's media/ folder */
static int arcade_art(const game_t *g, char *dir, size_t dn, char *name, size_t nn)
{
	if (systems[g->sys].cat != CAT_ARCADE) return 0;
	const char *b = base_of(g->unit), *dot = strrchr(b, '.');
	snprintf(dir, dn, "%s/media", cm_fat);
	snprintf(name, nn, "%.*s", dot ? (int)(dot - b) : (int)strlen(b), b);
	return 1;
}

static int art_file(const game_t *g, char *out, size_t n)
{
	char dir[1400], nm[600];
	if (arcade_art(g, dir, sizeof(dir), nm, sizeof(nm))) {
		snprintf(out, n, "%s/%s.png", dir, nm);
		if (!access(out, R_OK)) return 0;
	}
	parent_of(g->unit, dir, sizeof(dir));
	snprintf(out, n, "%s/media/%s.png", dir, g->title);
	if (!access(out, R_OK)) return 0;
	snprintf(out, n, "%s/media/%s.png", g->root, g->title);
	return access(out, R_OK) ? -1 : 0;
}

/* ---------- details: gamelist.xml ---------- */

static char *read_file(const char *p, size_t *len)
{
	FILE *f = fopen(p, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *b = n >= 0 && n < 64L * 1024 * 1024 ? malloc((size_t)n + 1) : NULL;
	if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
	fclose(f);
	if (b) { b[n] = 0; if (len) *len = (size_t)n; }
	return b;
}

static void xml_unescape(const char *s, size_t n, char *out, size_t on)
{
	size_t j = 0;
	for (size_t i = 0; i < n && j + 1 < on;) {
		if (s[i] == '&') {
			static const struct { const char *e; char c; } E[] = { { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' }, { "&apos;", '\'' } };
			int hit = 0;
			for (int k = 0; k < 5; k++) {
				size_t el = strlen(E[k].e);
				if (i + el <= n && !strncmp(s + i, E[k].e, el)) { out[j++] = E[k].c; i += el; hit = 1; break; }
			}
			if (hit) continue;
			if (i + 2 < n && s[i + 1] == '#') {
				char *end;
				long c = s[i + 2] == 'x' ? strtol(s + i + 3, &end, 16) : strtol(s + i + 2, &end, 10);
				if (*end == ';' && c > 0 && c < 0x110000) {
					if (c < 0x80) out[j++] = (char)c;
					else if (c < 0x800 && j + 2 < on) { out[j++] = (char)(0xc0 | (c >> 6)); out[j++] = (char)(0x80 | (c & 0x3f)); }
					else if (c < 0x10000 && j + 3 < on) { out[j++] = (char)(0xe0 | (c >> 12)); out[j++] = (char)(0x80 | ((c >> 6) & 0x3f)); out[j++] = (char)(0x80 | (c & 0x3f)); }
					else if (j + 4 < on) { out[j++] = (char)(0xf0 | (c >> 18)); out[j++] = (char)(0x80 | ((c >> 12) & 0x3f)); out[j++] = (char)(0x80 | ((c >> 6) & 0x3f)); out[j++] = (char)(0x80 | (c & 0x3f)); }
					i = (size_t)(end - s) + 1;
					continue;
				}
			}
		}
		out[j++] = s[i++];
	}
	out[j] = 0;
}

static char *xml_escape(const char *s)
{
	size_t n = 0;
	for (const char *c = s; *c; c++) n += *c == '&' ? 5 : (*c == '<' || *c == '>') ? 4 : *c == '"' ? 6 : 1;
	char *o = malloc(n + 1), *w = o;
	for (const char *c = s; *c; c++) {
		if (*c == '&') { memcpy(w, "&amp;", 5); w += 5; }
		else if (*c == '<') { memcpy(w, "&lt;", 4); w += 4; }
		else if (*c == '>') { memcpy(w, "&gt;", 4); w += 4; }
		else if (*c == '"') { memcpy(w, "&quot;", 6); w += 6; }
		else *w++ = *c;
	}
	*w = 0;
	return o;
}

/* a tag's text inside [b, e): 1 found */
static int tag_text(const char *b, const char *e, const char *tag, char *out, size_t on)
{
	char open[40], close_[40];
	snprintf(open, sizeof(open), "<%s>", tag);
	snprintf(close_, sizeof(close_), "</%s>", tag);
	const char *s = memmem(b, (size_t)(e - b), open, strlen(open));
	if (!s) return 0;
	s += strlen(open);
	const char *t = memmem(s, (size_t)(e - s), close_, strlen(close_));
	if (!t) return 0;
	xml_unescape(s, (size_t)(t - s), out, on);
	return 1;
}

/* the <game> block for a game in its system's gamelist.xml: [*b, *e) in xml, or 0 */
static int find_block(const char *xml, const game_t *g, const char **b, const char **e)
{
	const char *rel = g->unit + strlen(g->root) + 1;
	const char *frel = g->file + strlen(g->root) + 1;
	for (const char *p = xml; (p = strstr(p, "<game")) != NULL;) {
		if (p[5] != '>' && p[5] != ' ' && p[5] != '\t' && p[5] != '\n' && p[5] != '\r') { p += 5; continue; }
		const char *end = strstr(p, "</game>");
		if (!end) return 0;
		end += 7;
		char path[1400];
		if (tag_text(p, end, "path", path, sizeof(path))) {
			const char *q = path;
			if (!strncmp(q, "./", 2)) q += 2;
			size_t ql = strlen(q);
			while (ql && q[ql - 1] == '/') ql--;
			if ((strlen(rel) == ql && !strncmp(q, rel, ql)) || (strlen(frel) == ql && !strncmp(q, frel, ql))) {
				*b = p;
				*e = end;
				return 1;
			}
		}
		p = end;
	}
	return 0;
}

typedef struct { const char *key, *tag; } field_t;
static const field_t FIELDS[] = {
	{ "title", "name" }, { "description", "desc" }, { "year", "releasedate" }, { "developer", "developer" },
	{ "publisher", "publisher" }, { "genre", "genre" }, { "players", "players" }, { "region", "region" }, { NULL, NULL },
};

static char *gamelist_path(const game_t *g)
{
	char p[1500];
	snprintf(p, sizeof(p), "%s/gamelist.xml", g->root);
	return strdup(p);
}

/* the details from gamelist.xml into g; 1 when it has a description */
static int add_details(const game_t *g, cJSON *o, const char *xml)
{
	const char *b, *e;
	int found = xml && find_block(xml, g, &b, &e);
	int desc = 0;
	for (int i = 0; FIELDS[i].key; i++) {
		if (!strcmp(FIELDS[i].key, "title")) continue;
		char v[8192];
		if (found && tag_text(b, e, FIELDS[i].tag, v, sizeof(v)) && *trim(v)) {
			char *t = trim(v);
			if (!strcmp(FIELDS[i].key, "year")) t[strlen(t) > 4 ? 4 : strlen(t)] = 0;
			if (!strcmp(FIELDS[i].key, "description")) desc = 1;
			if (o) cJSON_AddStringToObject(o, FIELDS[i].key, t);
		} else if (o) cJSON_AddNullToObject(o, FIELDS[i].key);
	}
	if (o) {
		char name[1024];
		if (found && tag_text(b, e, "name", name, sizeof(name)) && *trim(name)) cJSON_AddStringToObject(o, "official_name", trim(name));
		else cJSON_AddNullToObject(o, "official_name");
	}
	return desc;
}

/* ---------- favourites and history (read only) ---------- */

static char *favorites_text(void)
{
	char p[700];
	snprintf(p, sizeof(p), "%s/gamelist.ini", cm_dir);
	char *t = read_file(p, NULL);
	if (!t) return NULL;
	/* the FAVORITES part only: the lines before [HISTORY] */
	char *h = strstr(t, "[HISTORY]");
	if (h) *h = 0;
	return t;
}

static int is_favorite(const char *fav, const game_t *g)
{
	return fav && (strstr(fav, g->unit) || strstr(fav, g->file));
}

typedef struct { long count, seconds, last; } play_t;

static void play_of(const game_t *g, play_t *pl)
{
	memset(pl, 0, sizeof(*pl));
	char p[700], line[2000];
	snprintf(p, sizeof(p), "%s/playtime/history.tsv", cm_dir);
	FILE *f = fopen(p, "r");
	if (!f) return;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		char *t1 = strchr(line, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL, *t3 = t2 ? strchr(t2 + 1, '\t') : NULL;
		if (!t3 || line[0] == '#') continue;
		if (strcmp(t3 + 1, g->file) && strcmp(t3 + 1, g->unit)) continue;
		pl->count++;
		pl->seconds += atol(t1 + 1);
		long at = atol(line);
		if (at > pl->last) pl->last = at;
	}
	fclose(f);
}

/* the most recently played game's path, from history.tsv */
static int last_played(char *out, size_t n)
{
	char p[700], line[2000];
	snprintf(p, sizeof(p), "%s/playtime/history.tsv", cm_dir);
	FILE *f = fopen(p, "r");
	if (!f) return -1;
	long best = -1;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		char *t3 = NULL, *t = line;
		for (int k = 0; k < 3 && t; k++) t = strchr(t + (k ? 1 : 0), '\t');
		t3 = t;
		if (!t3 || line[0] == '#') continue;
		long at = atol(line);
		if (at >= best) { best = at; snprintf(out, n, "%s", t3 + 1); }
	}
	fclose(f);
	return best < 0 ? -1 : 0;
}

/* ---------- replies ---------- */

static cJSON *game_json(const game_t *g)
{
	cJSON *o = cJSON_CreateObject();
	const sys_t *s = &systems[g->sys];
	char *gp = gamelist_path(g), *xml = read_file(gp, NULL), *fav = favorites_text();
	free(gp);
	cJSON_AddNumberToObject(o, "id", (double)g->id);
	cJSON_AddStringToObject(o, "system", s->name);
	cJSON_AddStringToObject(o, "system_name", s->name);
	cJSON_AddNumberToObject(o, "category", s->cat);
	/* the title: gamelist.xml's name when it has one, else the file's (as Console Mode shows it) */
	cJSON_AddStringToObject(o, "title", g->title);
	add_details(g, o, xml);
	char art[1500];
	if (!art_file(g, art, sizeof(art))) cJSON_AddStringToObject(o, "art", art);
	else cJSON_AddNullToObject(o, "art");
	cJSON_AddBoolToObject(o, "favorite", is_favorite(fav, g));
	play_t pl;
	play_of(g, &pl);
	cJSON_AddNumberToObject(o, "play_count", (double)pl.count);
	cJSON_AddNumberToObject(o, "play_seconds", (double)pl.seconds);
	if (pl.last) cJSON_AddNumberToObject(o, "last_played", (double)pl.last);
	else cJSON_AddNullToObject(o, "last_played");
	cJSON_AddNullToObject(o, "based_on");
	cJSON_AddStringToObject(o, "scrape", art_file(g, art, sizeof(art)) ? "ready" : "found");
	cJSON *files = cJSON_AddArrayToObject(o, "files");
	cJSON *f = cJSON_CreateObject();
	cJSON_AddStringToObject(f, "path", g->file);
	cJSON_AddStringToObject(f, "where", !strncmp(g->file, cm_fat, strlen(cm_fat)) ? "SD card" : "USB");
	struct stat sb;
	cJSON_AddNumberToObject(f, "size", stat(g->file, &sb) ? 0 : (double)sb.st_size);
	cJSON_AddBoolToObject(f, "online", !access(g->file, R_OK));
	cJSON_AddNullToObject(f, "crc32");
	cJSON_AddNullToObject(f, "serial");
	cJSON_AddItemToArray(files, f);
	free(xml);
	free(fav);
	return o;
}

static int str_contains_ci(const char *h, const char *n)
{
	size_t nl = strlen(n);
	for (; *h; h++) if (!strncasecmp(h, n, nl)) return 1;
	return 0;
}

static cJSON *cmd_status(void)
{
	cJSON *o = ok();
	char v[64] = "", p[700];
	snprintf(p, sizeof(p), "%s/.cm_latest_seen", cm_dir);
	FILE *f = fopen(p, "r");
	if (f) { if (fgets(v, sizeof(v), f)) v[strcspn(v, "\r\n")] = 0; fclose(f); }
	cJSON_AddStringToObject(o, "version", CMREMOTE_VERSION);
	cJSON_AddStringToObject(o, "frontend", "Console Mode");
	cJSON_AddStringToObject(o, "frontend_version", v);
	cJSON_AddBoolToObject(o, "indexing", 0);
	cJSON_AddBoolToObject(o, "identifying", 0);
	cJSON_AddNumberToObject(o, "generation", generation);
	cJSON_AddNumberToObject(o, "games", ngames);
	return o;
}

static cJSON *cmd_categories(void)
{
	cJSON *o = ok(), *arr = cJSON_AddArrayToObject(o, "categories");
	for (int c = 0; c < 5; c++) {
		int n = 0;
		for (int i = 0; i < nsys; i++) if (systems[i].cat == c) n += systems[i].games;
		cJSON *x = cJSON_CreateObject();
		cJSON_AddNumberToObject(x, "id", c);
		cJSON_AddStringToObject(x, "name", CAT_NAMES[c]);
		cJSON_AddNumberToObject(x, "games", n);
		cJSON_AddItemToArray(arr, x);
	}
	return o;
}

static int can_launch(const game_t *g, char *rbf, size_t n, char *type, int *index, int *delay);

static cJSON *cmd_systems(cJSON *req)
{
	cJSON *cat = cJSON_GetObjectItem(req, "category");
	const char *grp = cJSON_GetStringValue(cJSON_GetObjectItem(req, "group"));
	int all = cJSON_IsTrue(cJSON_GetObjectItem(req, "all"));
	cJSON *o = ok(), *arr = cJSON_AddArrayToObject(o, "systems");
	for (int i = 0; i < nsys; i++) {
		if (cJSON_IsNumber(cat) && systems[i].cat != cat->valueint) continue;
		if (grp && strcasecmp(systems[i].group ? systems[i].group : "", grp)) continue;
		if (!systems[i].games && !all) continue;
		cJSON *x = cJSON_CreateObject();
		cJSON_AddStringToObject(x, "id", systems[i].name);
		cJSON_AddStringToObject(x, "name", systems[i].name);
		cJSON_AddNumberToObject(x, "category", systems[i].cat);
		cJSON_AddStringToObject(x, "group", systems[i].group ? systems[i].group : "");
		cJSON_AddNumberToObject(x, "games", systems[i].games);
		int core = 0;
		for (int k = 0; k < ngames && !core; k++)
			if (games[k].sys == i) { char r[600], t; int ix, d; core = !can_launch(&games[k], r, sizeof(r), &t, &ix, &d); break; }
		cJSON_AddBoolToObject(x, "core", core);
		cJSON_AddItemToArray(arr, x);
	}
	return o;
}

static cJSON *cmd_games(cJSON *req)
{
	const char *sys = cJSON_GetStringValue(cJSON_GetObjectItem(req, "system"));
	const char *filter = cJSON_GetStringValue(cJSON_GetObjectItem(req, "filter"));
	const char *q = cJSON_GetStringValue(cJSON_GetObjectItem(req, "q"));
	if (filter && !*filter) filter = NULL;
	if (q && !*q) q = NULL;
	if (!sys && !filter && !q) return fail("system missing");
	cJSON *off = cJSON_GetObjectItem(req, "offset"), *lim = cJSON_GetObjectItem(req, "limit");
	int offset = cJSON_IsNumber(off) ? off->valueint : 0, limit = cJSON_IsNumber(lim) ? lim->valueint : 100000;
	if (filter && strcmp(filter, "missing_art") && strcmp(filter, "missing_details") && strcmp(filter, "favorites") &&
	    strcmp(filter, "recent") && strcmp(filter, "not_found") && strcmp(filter, "linked") && strcmp(filter, "not_game") &&
	    strcmp(filter, "unidentified"))
		return fail("filter: missing_art, missing_details, favorites or recent");
	char *fav = favorites_text();
	/* gamelist.xml per system folder, read once for this request */
	char *xroot = NULL, *xml = NULL;
	cJSON *o = ok(), *arr = cJSON_CreateArray();
	int total = 0;
	for (int i = 0; i < ngames; i++) {
		const game_t *g = &games[i];
		if (sys && strcasecmp(systems[g->sys].name, sys)) continue;
		if (q && !str_contains_ci(g->title, q)) continue;
		char art[1500];
		int has_art = !art_file(g, art, sizeof(art));
		if (!xroot || strcmp(xroot, g->root)) {
			free(xroot);
			free(xml);
			xroot = strdup(g->root);
			char *gp = gamelist_path(g);
			xml = read_file(gp, NULL);
			free(gp);
		}
		int details = add_details(g, NULL, xml);
		int favorite = is_favorite(fav, g);
		if (filter) {
			if (!strcmp(filter, "missing_art") && has_art) continue;
			if (!strcmp(filter, "missing_details") && details) continue;
			if (!strcmp(filter, "favorites") && !favorite) continue;
			if (!strcmp(filter, "recent")) { play_t pl; play_of(g, &pl); if (!pl.last) continue; }
			/* Console Mode doesn't keep these: nothing is in them */
			if (!strcmp(filter, "not_found") || !strcmp(filter, "linked") || !strcmp(filter, "not_game") || !strcmp(filter, "unidentified")) continue;
		}
		total++;
		if (total <= offset || cJSON_GetArraySize(arr) >= limit) continue;
		cJSON *x = cJSON_CreateObject();
		cJSON_AddNumberToObject(x, "id", (double)g->id);
		cJSON_AddStringToObject(x, "title", g->title);
		cJSON_AddBoolToObject(x, "favorite", favorite);
		cJSON_AddBoolToObject(x, "linked", 0);
		cJSON_AddStringToObject(x, "system", systems[g->sys].name);
		cJSON_AddBoolToObject(x, "art", has_art);
		cJSON_AddBoolToObject(x, "details", details);
		cJSON_AddStringToObject(x, "scrape", has_art ? "found" : "ready");
		cJSON_AddItemToArray(arr, x);
	}
	free(xroot);
	free(xml);
	free(fav);
	cJSON_AddNumberToObject(o, "total", total);
	cJSON_AddItemToObject(o, "games", arr);
	return o;
}

static long req_id(cJSON *req)
{
	cJSON *id = cJSON_GetObjectItem(req, "id");
	return cJSON_IsNumber(id) ? (long)id->valuedouble : -1;
}

static cJSON *cmd_game(long id)
{
	game_t *g = find_game(id);
	if (!g) return fail("no such game");
	cJSON *o = ok();
	cJSON_AddItemToObject(o, "game", game_json(g));
	return o;
}

static cJSON *cmd_last(void)
{
	char p[1500];
	cJSON *o = ok();
	game_t *g = NULL;
	if (!last_played(p, sizeof(p)))
		for (int i = 0; i < ngames && !g; i++) if (!strcmp(games[i].file, p) || !strcmp(games[i].unit, p)) g = &games[i];
	if (g) cJSON_AddItemToObject(o, "game", game_json(g));
	else cJSON_AddNullToObject(o, "game");
	return o;
}

/* Console Mode's Load Game list: the section groups, in its order, with how many systems and games each has */
static cJSON *cmd_groups(cJSON *req)
{
	int all = cJSON_IsTrue(cJSON_GetObjectItem(req, "all"));
	cJSON *o = ok(), *arr = cJSON_AddArrayToObject(o, "groups");
	for (int i = 0; i < nsys; i++) {
		const char *g = systems[i].group ? systems[i].group : "";
		int seen = 0;
		for (int k = 0; k < i && !seen; k++) seen = !strcmp(systems[k].group ? systems[k].group : "", g);
		if (seen) continue;
		int ns = 0, ng = 0;
		for (int k = i; k < nsys; k++)
			if (!strcmp(systems[k].group ? systems[k].group : "", g) && (systems[k].games || all)) { ns++; ng += systems[k].games; }
		if (!ns) continue;
		cJSON *x = cJSON_CreateObject();
		cJSON_AddStringToObject(x, "name", g);
		cJSON_AddNumberToObject(x, "systems", ns);
		cJSON_AddNumberToObject(x, "games", ng);
		cJSON_AddItemToArray(arr, x);
	}
	return o;
}

typedef struct { char *name; int games; } dir_t;
static int cmp_dir(const void *a, const void *b) { return strcasecmp(((const dir_t *)a)->name, ((const dir_t *)b)->name); }

/* a system's folder as Console Mode lists it: its folders first ([DIR]), then its games; games in
   folders on several drives are listed together */
static cJSON *cmd_browse(cJSON *req)
{
	const char *sys = cJSON_GetStringValue(cJSON_GetObjectItem(req, "system"));
	const char *path = cJSON_GetStringValue(cJSON_GetObjectItem(req, "path"));
	if (!sys) return fail("system missing");
	if (!path) path = "";
	size_t pl = strlen(path);
	dir_t *dirs = NULL;
	int nd = 0;
	cJSON *o = ok(), *games_ = cJSON_CreateArray();
	int found = 0;
	char *fav = favorites_text();
	for (int i = 0; i < ngames; i++) {
		const game_t *g = &games[i];
		if (strcasecmp(systems[g->sys].name, sys)) continue;
		found = 1;
		const char *rel = g->unit + strlen(g->root);
		if (*rel == '/') rel++;
		if (pl && (strncmp(rel, path, pl) || rel[pl] != '/')) continue;
		const char *rest = rel + (pl ? pl + 1 : 0);
		const char *sl = strchr(rest, '/');
		if (sl) {
			/* in a folder below: that folder, once */
			size_t nl = (size_t)(sl - rest);
			int k;
			for (k = 0; k < nd; k++) if (strlen(dirs[k].name) == nl && !strncmp(dirs[k].name, rest, nl)) break;
			if (k == nd) {
				dir_t *n2 = realloc(dirs, (size_t)(nd + 1) * sizeof(dir_t));
				if (!n2) continue;
				dirs = n2;
				dirs[nd].name = strndup(rest, nl);
				dirs[nd].games = 0;
				nd++;
			}
			dirs[k].games++;
			continue;
		}
		char art[1500];
		cJSON *x = cJSON_CreateObject();
		cJSON_AddStringToObject(x, "type", "game");
		cJSON_AddNumberToObject(x, "id", (double)g->id);
		cJSON_AddStringToObject(x, "title", g->title);
		cJSON_AddBoolToObject(x, "art", !art_file(g, art, sizeof(art)));
		cJSON_AddBoolToObject(x, "favorite", is_favorite(fav, g));
		cJSON_AddItemToArray(games_, x);
	}
	free(fav);
	if (!found) { cJSON_Delete(games_); cJSON_Delete(o); for (int k = 0; k < nd; k++) free(dirs[k].name); free(dirs); return fail("no such system"); }
	if (nd) qsort(dirs, (size_t)nd, sizeof(dir_t), cmp_dir);
	cJSON *arr = cJSON_AddArrayToObject(o, "entries");
	for (int k = 0; k < nd; k++) {
		cJSON *x = cJSON_CreateObject();
		char p[1400];
		snprintf(p, sizeof(p), "%s%s%s", path, pl ? "/" : "", dirs[k].name);
		cJSON_AddStringToObject(x, "type", "dir");
		/* at the top, Console Mode leaves the "_" off a folder's name (_alternatives) */
		cJSON_AddStringToObject(x, "title", !pl && dirs[k].name[0] == '_' && dirs[k].name[1] ? dirs[k].name + 1 : dirs[k].name);
		cJSON_AddStringToObject(x, "path", p);
		cJSON_AddNumberToObject(x, "games", dirs[k].games);
		cJSON_AddItemToArray(arr, x);
		free(dirs[k].name);
	}
	free(dirs);
	cJSON *it;
	while ((it = cJSON_DetachItemFromArray(games_, 0))) cJSON_AddItemToArray(arr, it);
	cJSON_Delete(games_);
	cJSON_AddStringToObject(o, "system", sys);
	cJSON_AddStringToObject(o, "path", path);
	cJSON_AddNumberToObject(o, "total", cJSON_GetArraySize(arr));
	return o;
}

/* ---------- edit: gamelist.xml ---------- */

static int write_atomic(const char *path, const char *data, size_t n)
{
	char tmp[1600];
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	FILE *f = fopen(tmp, "wb");
	if (!f) return -1;
	int bad = fwrite(data, 1, n, f) != n;
	bad |= fflush(f) != 0 || fsync(fileno(f)) != 0;
	bad |= fclose(f) != 0;
	if (bad || rename(tmp, path)) { unlink(tmp); return -1; }
	return 0;
}

typedef struct { char *s; size_t n, cap; } buf_t;
static void bput(buf_t *b, const char *s, size_t n)
{
	if (b->n + n + 1 > b->cap) {
		size_t nc = (b->n + n + 1) * 2;
		char *ns = realloc(b->s, nc);
		if (!ns) return;
		b->s = ns;
		b->cap = nc;
	}
	memcpy(b->s + b->n, s, n);
	b->n += n;
	b->s[b->n] = 0;
}
static void bputs(buf_t *b, const char *s) { bput(b, s, strlen(s)); }

/* the block with one tag set (or removed when v is NULL/empty) */
static char *block_set(const char *blk, const char *tag, const char *v)
{
	char open[40], close_[40];
	snprintf(open, sizeof(open), "<%s>", tag);
	snprintf(close_, sizeof(close_), "</%s>", tag);
	buf_t b = { 0 };
	const char *s = strstr(blk, open), *t = s ? strstr(s, close_) : NULL;
	char *ev = v && *v ? xml_escape(v) : NULL;
	if (s && t) {
		/* the whole line it's on goes when it's removed */
		const char *ls = s;
		while (ls > blk && (ls[-1] == ' ' || ls[-1] == '\t')) ls--;
		const char *le = t + strlen(close_);
		if (!ev) { if (*le == '\r') le++; if (*le == '\n') le++; bput(&b, blk, (size_t)(ls - blk)); bputs(&b, le); }
		else { bput(&b, blk, (size_t)(s - blk)); bputs(&b, open); bputs(&b, ev); bputs(&b, t); }
	} else if (ev) {
		const char *end = strstr(blk, "</game>");
		bput(&b, blk, (size_t)(end - blk));
		/* one indent deeper than </game>'s */
		while (b.n && (b.s[b.n - 1] == ' ' || b.s[b.n - 1] == '\t')) b.n--;
		if (b.n && b.s[b.n - 1] != '\n') bputs(&b, "\n");
		bputs(&b, "    ");
		bputs(&b, open);
		bputs(&b, ev);
		bputs(&b, close_);
		bputs(&b, "\n  ");
		bputs(&b, end);
	} else bputs(&b, blk);
	free(ev);
	return b.s;
}

static cJSON *cmd_edit(cJSON *req)
{
	game_t *g = find_game(req_id(req));
	if (!g) return fail("no such game");
	cJSON *set = cJSON_GetObjectItem(req, "set");
	if (!cJSON_IsObject(set)) return fail("set: the details to change");
	cJSON *t = cJSON_GetObjectItem(set, "title");
	if (t && (!cJSON_IsString(t) || !*t->valuestring)) return fail("the title can't be empty");
	char *gp = gamelist_path(g);
	size_t len = 0;
	char *xml = read_file(gp, &len);
	if (!xml) xml = strdup("<?xml version=\"1.0\"?>\n<gameList>\n</gameList>\n");
	const char *b, *e;
	char *blk;
	if (find_block(xml, g, &b, &e)) blk = strndup(b, (size_t)(e - b));
	else {
		const char *rel = g->unit + strlen(g->root) + 1;
		char *er = xml_escape(rel), *et = xml_escape(g->title);
		size_t n = strlen(er) + strlen(et) + 80;
		blk = malloc(n);
		snprintf(blk, n, "<game>\n    <path>./%s</path>\n    <name>%s</name>\n  </game>", er, et);
		free(er);
		free(et);
		b = e = NULL;
	}
	for (int i = 0; FIELDS[i].key; i++) {
		cJSON *v = cJSON_GetObjectItem(set, FIELDS[i].key);
		if (!v) continue;
		const char *s = cJSON_IsString(v) ? v->valuestring : NULL;
		char date[32];
		if (s && !strcmp(FIELDS[i].key, "year") && *s) {
			if (strlen(s) != 4 || !isdigit((unsigned char)s[0]) || !isdigit((unsigned char)s[3])) { free(blk); free(xml); free(gp); return fail("year: four digits"); }
			snprintf(date, sizeof(date), "%s0101T000000", s);
			s = date;
		}
		char *nb = block_set(blk, FIELDS[i].tag, s);
		free(blk);
		blk = nb;
	}
	buf_t out = { 0 };
	if (b) { bput(&out, xml, (size_t)(b - xml)); bputs(&out, blk); bputs(&out, e); }
	else {
		char *end = strstr(xml, "</gameList>");
		if (!end) { free(blk); free(xml); free(gp); return fail("gamelist.xml isn't one this can change"); }
		bput(&out, xml, (size_t)(end - xml));
		bputs(&out, "  ");
		bputs(&out, blk);
		bputs(&out, "\n");
		bputs(&out, end);
	}
	int bad = write_atomic(gp, out.s, out.n);
	free(out.s);
	free(blk);
	free(xml);
	free(gp);
	if (bad) return fail("couldn't save gamelist.xml");
	generation++;
	cJSON *o = ok();
	cJSON_AddItemToObject(o, "game", game_json(g));
	return o;
}

/* ---------- starting a game: an MGL, for the systems whose core slot is known ---------- */

typedef struct { const char *folders, *rbf; char type; int index, delay; } core_t;
/* MiSTer cores by games folder  */
static const core_t CORES[] = {
	{ "PSX", "_Console/PSX", 's', 1, 1 }, { "Saturn", "_Console/Saturn", 's', 0, 1 }, { "MegaCD", "_Console/MegaCD", 's', 0, 1 },
	{ "TGFX16-CD", "_Console/TurboGrafx16", 's', 0, 1 }, { "NES", "_Console/NES", 'f', 1, 1 }, { "SNES", "_Console/SNES", 'f', 0, 1 },
	{ "MegaDrive|Genesis", "_Console/MegaDrive|_Console/Genesis", 'f', 1, 1 }, { "N64", "_Console/N64", 'f', 1, 1 },
	{ "SMS", "_Console/SMS", 'f', 1, 1 }, { "TGFX16", "_Console/TurboGrafx16", 'f', 1, 1 },
	{ "ATARI7800|ATARI2600", "_Console/Atari7800", 'f', 1, 1 }, { "GAMEBOY|GBC", "_Console/Gameboy", 'f', 1, 1 },
	{ "GBA", "_Console/GBA", 'f', 1, 1 }, { "GameGear", "_Console/SMS", 'f', 2, 1 }, { "WonderSwan", "_Console/WonderSwan", 'f', 1, 1 },
	{ "AtariLynx", "_Console/AtariLynx", 'f', 1, 1 }, { "C64", "_Computer/C64", 'f', 1, 1 }, { "MSX", "_Computer/MSX", 'f', 1, 1 },
	{ "ATARI800", "_Computer/Atari800", 'f', 1, 1 }, { NULL, NULL, 0, 0, 0 },
};

static int core_file(const char *rbf, char *out, size_t n)
{
	char dir[700], base[128], best[256] = "";
	const char *slash = strrchr(rbf, '/');
	snprintf(dir, sizeof(dir), "%s/%.*s", cm_fat, slash ? (int)(slash - rbf) : 0, rbf);
	snprintf(base, sizeof(base), "%s", slash ? slash + 1 : rbf);
	DIR *d = opendir(dir);
	if (!d) return -1;
	struct dirent *e;
	size_t bl = strlen(base);
	while ((e = readdir(d)))
		if (!strncasecmp(e->d_name, base, bl) && (e->d_name[bl] == '_' || e->d_name[bl] == '.') && !strcasecmp(ext_of(e->d_name), ".rbf") && strcmp(e->d_name, best) > 0)
			snprintf(best, sizeof(best), "%s", e->d_name);
	closedir(d);
	if (!best[0]) return -1;
	snprintf(out, n, "%s/%s", dir, best);
	return 0;
}

/* 0 and the core when this game can be started from here; an .mgl/.mra/.rbf starts as it is (rbf empty) */
static int can_launch(const game_t *g, char *rbf, size_t n, char *type, int *index, int *delay)
{
	const char *x = ext_of(g->file);
	if (!strcasecmp(x, ".mgl") || !strcasecmp(x, ".mra") || !strcasecmp(x, ".rbf")) { rbf[0] = 0; return 0; }
	/* the games folder: the part after /games/ */
	const char *gs = strstr(g->root, "/games/");
	if (!gs) return -1;
	char folder[128];
	snprintf(folder, sizeof(folder), "%s", gs + 7);
	folder[strcspn(folder, "/")] = 0;
	for (int i = 0; CORES[i].folders; i++) {
		char fl[64], *save = NULL;
		snprintf(fl, sizeof(fl), "%s", CORES[i].folders);
		int hit = 0;
		for (char *f = strtok_r(fl, "|", &save); f && !hit; f = strtok_r(NULL, "|", &save)) hit = !strcasecmp(f, folder);
		if (!hit) continue;
		char rb[100], *s2 = NULL;
		snprintf(rb, sizeof(rb), "%s", CORES[i].rbf);
		for (char *r = strtok_r(rb, "|", &s2); r; r = strtok_r(NULL, "|", &s2))
			if (!core_file(r, rbf, n)) { *type = CORES[i].type; *index = CORES[i].index; *delay = CORES[i].delay; return 0; }
	}
	return -1;
}

static int mister_cmd(const char *cmd)
{
	int fd = open("/dev/MiSTer_cmd", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) return -1;
	ssize_t w = write(fd, cmd, strlen(cmd));
	close(fd);
	return w == (ssize_t)strlen(cmd) ? 0 : -1;
}

static cJSON *cmd_launch(long id)
{
	game_t *g = find_game(id);
	if (!g) return fail("no such game");
	char rbf[700], type = 'f';
	int index = 0, delay = 1;
	if (can_launch(g, rbf, sizeof(rbf), &type, &index, &delay)) return fail("start this one from Console Mode: its core isn't known here");
	char cmd[1600];
	if (!rbf[0]) snprintf(cmd, sizeof(cmd), "load_core %s\n", g->file);
	else {
		const char *mgl = "/tmp/cmremote_launch.mgl";
		char *ep = xml_escape(g->file);
		/* MiSTer wants the rbf without /media/fat/ */
		const char *rel = !strncmp(rbf, cm_fat, strlen(cm_fat)) ? rbf + strlen(cm_fat) + 1 : rbf;
		char r2[700];
		snprintf(r2, sizeof(r2), "%s", rel);
		char *dot = strrchr(r2, '.');
		if (dot) *dot = 0;
		FILE *f = fopen(mgl, "w");
		if (!f) { free(ep); return fail("couldn't write the MGL"); }
		fprintf(f, "<mistergamedescription>\n\t<rbf>%s</rbf>\n\t<file delay=\"%d\" type=\"%c\" index=\"%d\" path=\"%s\"/>\n</mistergamedescription>\n",
		        r2, delay, type, index, ep);
		fclose(f);
		free(ep);
		snprintf(cmd, sizeof(cmd), "load_core %s\n", mgl);
	}
	if (mister_cmd(cmd)) return fail("MiSTer didn't take the command");
	return ok();
}

/* ---------- box art from the apps ---------- */

int cm_art_path(long id, char *out, size_t n)
{
	pthread_mutex_lock(&mu);
	fresh();
	game_t *g = find_game(id);
	int r = g ? art_file(g, out, n) : -1;
	pthread_mutex_unlock(&mu);
	return r;
}

cJSON *cm_art_set(long id, const char *src)
{
	int w, h, n;
	if (!stbi_info(src, &w, &h, &n) || w < 1 || h < 1 || (long)w * h > ART_MAX_PIXELS) return fail("that isn't an image this can read");
	unsigned char *px = stbi_load(src, &w, &h, &n, 3);
	if (!px) return fail("that isn't an image this can read");
	int nw = w > ART_WIDTH ? ART_WIDTH : w, nh = (int)((long)h * nw / w);
	if (nh < 1) nh = 1;
	unsigned char *o = px;
	if (nw != w) {
		/* box filter down to the new size */
		o = malloc((size_t)nw * nh * 3);
		if (!o) { stbi_image_free(px); return fail("out of memory"); }
		for (int y = 0; y < nh; y++) {
			int y0 = (int)((long)y * h / nh), y1 = (int)((long)(y + 1) * h / nh);
			if (y1 <= y0) y1 = y0 + 1;
			for (int x = 0; x < nw; x++) {
				int x0 = (int)((long)x * w / nw), x1 = (int)((long)(x + 1) * w / nw);
				if (x1 <= x0) x1 = x0 + 1;
				long s[3] = { 0, 0, 0 }, cnt = 0;
				for (int yy = y0; yy < y1; yy++)
					for (int xx = x0; xx < x1; xx++) { const unsigned char *p = px + ((size_t)yy * w + xx) * 3; s[0] += p[0]; s[1] += p[1]; s[2] += p[2]; cnt++; }
				unsigned char *q = o + ((size_t)y * nw + x) * 3;
				for (int c = 0; c < 3; c++) q[c] = (unsigned char)(s[c] / cnt);
			}
		}
	}
	pthread_mutex_lock(&mu);
	fresh();
	game_t *g = find_game(id);
	cJSON *r = NULL;
	if (!g) r = fail("no such game");
	else {
		char dir[1500], dst[1700], tmp[1800], opt[1800], nm[600];
		snprintf(dir, sizeof(dir), "%s/media", g->root);
		snprintf(nm, sizeof(nm), "%s", g->title);
		arcade_art(g, dir, sizeof(dir), nm, sizeof(nm));
		mkdir(dir, 0755);
		snprintf(dst, sizeof(dst), "%s/%s.png", dir, nm);
		snprintf(tmp, sizeof(tmp), "%s.new.png", dst);
		if (!stbi_write_png(tmp, nw, nh, 3, o, nw * 3) || rename(tmp, dst)) { unlink(tmp); r = fail("couldn't save the box art"); }
		else {
			/* Console Mode's optimised copy of the old art: gone, so it makes one of the new */
			snprintf(opt, sizeof(opt), "%s/optimized/%s.png", dir, nm);
			unlink(opt);
			generation++;
			r = ok();
			cJSON_AddItemToObject(r, "game", game_json(g));
		}
	}
	pthread_mutex_unlock(&mu);
	if (o != px) free(o);
	stbi_image_free(px);
	return r;
}

/* Console Mode's own font (themeconfig/resources): "regular" or "bold" */
int cm_font_path(const char *which, char *out, size_t n)
{
	const char *f = !strcmp(which, "bold") ? "Akrobat-Bold.ttf" : !strcmp(which, "regular") ? "Akrobat-SemiBold.ttf" : NULL;
	if (!f) return -1;
	snprintf(out, n, "%s/themeconfig/resources/%s", cm_dir, f);
	return access(out, R_OK) ? -1 : 0;
}

/* ---------- box art lookups from an app: what's waiting, and taking art back out ----------
 * {cmd:"scrape_todo", [system], [id], [all], offset, limit}: the games without box art (all: every game),
 * each with what the app needs to look it up: the file, its size and time (for the app's own memory of
 * {cmd:"ident"}), whether it's a disc or an arcade game, and which details gamelist.xml has already
 * (those are never changed by a lookup). */
static cJSON *cmd_scrape_todo(cJSON *req)
{
	const char *sys = cJSON_GetStringValue(cJSON_GetObjectItem(req, "system"));
	if (sys && !*sys) sys = NULL;
	long only = req_id(req);
	int all = cJSON_IsTrue(cJSON_GetObjectItem(req, "all"));
	cJSON *off = cJSON_GetObjectItem(req, "offset"), *lim = cJSON_GetObjectItem(req, "limit");
	int offset = cJSON_IsNumber(off) ? off->valueint : 0, limit = cJSON_IsNumber(lim) ? lim->valueint : 400;
	char *xroot = NULL, *xml = NULL;
	cJSON *o = ok(), *arr = cJSON_CreateArray();
	int total = 0;
	for (int i = 0; i < ngames; i++) {
		const game_t *g = &games[i];
		if (only > 0 && g->id != only) continue;
		if (sys && strcasecmp(systems[g->sys].name, sys)) continue;
		char art[1500];
		int has_art = !art_file(g, art, sizeof(art));
		if (has_art && !all && only <= 0) continue;
		total++;
		if (total <= offset || cJSON_GetArraySize(arr) >= limit) continue;
		if (!xroot || strcmp(xroot, g->root)) {
			free(xroot);
			free(xml);
			xroot = strdup(g->root);
			char *gp = gamelist_path(g);
			xml = read_file(gp, NULL);
			free(gp);
		}
		cJSON *x = cJSON_CreateObject(), *d = cJSON_CreateObject();
		cJSON_AddNumberToObject(x, "id", (double)g->id);
		cJSON_AddStringToObject(x, "system", systems[g->sys].name);
		cJSON_AddNumberToObject(x, "category", systems[g->sys].cat);
		cJSON_AddStringToObject(x, "title", g->title);
		cJSON_AddStringToObject(x, "file", g->file);
		struct stat sb;
		int st = stat(g->file, &sb);
		cJSON_AddNumberToObject(x, "size", st ? 0 : (double)sb.st_size);
		cJSON_AddNumberToObject(x, "mtime", st ? 0 : (double)sb.st_mtime);
		cJSON_AddBoolToObject(x, "has_art", has_art);
		add_details(g, d, xml);
		cJSON *has = cJSON_AddArrayToObject(x, "has");
		for (cJSON *it = d->child; it; it = it->next)
			if (cJSON_IsString(it) && *it->valuestring && strcmp(it->string, "official_name")) cJSON_AddItemToArray(has, cJSON_CreateString(it->string));
		cJSON_Delete(d);
		cJSON_AddItemToArray(arr, x);
	}
	free(xroot);
	free(xml);
	cJSON_AddNumberToObject(o, "total", total);
	cJSON_AddItemToObject(o, "games", arr);
	return o;
}

/* ---------- box art Update All already put on the drives: its Game Artwork databases (chipster6502) ----------
   Update All › Extra content › Game Artwork DBs installs <drive>/docs/<system>/Artwork/<the No-Intro name>.jpg
   (arcade: <the MAME set name>.jpg), a 2D box, a 3D box or a box with a screenshot as picked there. A game
   without box art whose file name is there gets it as Console Mode's own PNG (cm_art_set), with no download.
   Only Artwork/ is used (never Titles/ or Screenshots/).

   {cmd:"art_docs"[, system, offset, limit]}   the next games without box art (limit, default 60): each matched
       one gets its art; {set, looked, next (the offset for the next call), left, dirs (Artwork folders found)} */

static void normkey(const char *s, char *o, size_t n, int loose)
{
	size_t k = 0;
	int depth = 0;
	for (; *s && k + 1 < n; s++) {
		unsigned char c = (unsigned char)*s;
		if (loose && (c == '(' || c == '[')) { depth++; continue; }
		if (loose && (c == ')' || c == ']')) { if (depth) depth--; continue; }
		if (depth) continue;
		if (isalnum(c)) o[k++] = (char)tolower(c);
	}
	o[k] = 0;
}

/* the docs/ folders that hold this system's artwork, by Console Mode's system name (and a few extensions) */
static const char *const DOCS_ALIAS[][2] = {
	{ "genesis", "Genesis" }, { "megadrive", "Genesis" }, { "genesis32x", "S32X" }, { "sega32x", "S32X" }, { "32x", "S32X" },
	{ "mastersystem", "SMS" }, { "sms", "SMS" }, { "gamegear", "GameGear" }, { "sg1000", "SG-1000" },
	{ "megacd", "MegaCD" }, { "segacd", "MegaCD" }, { "saturn", "Saturn" },
	{ "nes", "NES" }, { "famicom", "NES" }, { "famicomdisksystem", "FDS" }, { "fds", "FDS" },
	{ "snes", "SNES" }, { "superfamicom", "SNES" }, { "satellaview", "Satellaview" },
	{ "nintendo64", "N64" }, { "n64", "N64" }, { "virtualboy", "VirtualBoy" },
	{ "gameboy", "GAMEBOY" }, { "supergameboy", "GAMEBOY" }, { "gameboycolor", "GBC" }, { "gameboyadvance", "GBA" }, { "gba", "GBA" },
	{ "playstation", "PSX" }, { "psx", "PSX" },
	{ "neogeocd", "NeoGeo-CD" }, { "neogeomvsaes", "NEOGEO" }, { "neogeo", "NEOGEO" },
	{ "neogeopocket", "NeoGeoPocket" }, { "neogeopocketcolor", "NeoGeoPocket-Color" },
	{ "turbografx16", "TGFX16" }, { "pcengine", "TGFX16" }, { "supergrafx", "SuperGrafx" },
	{ "turbografx16cd", "TGFX16-CD" }, { "pcenginecd", "TGFX16-CD" },
	{ "jaguar", "Jaguar" }, { "atari2600", "Atari2600" }, { "atari5200", "ATARI5200" }, { "atari7800", "ATARI7800" },
	{ "atarilynx", "AtariLynx" }, { "lynx", "AtariLynx" }, { "colecovision", "Coleco" }, { "coleco", "Coleco" },
	{ "intellivision", "Intellivision" }, { "odyssey2", "ODYSSEY2" }, { "vectrex", "VECTREX" },
	{ "wonderswan", "WonderSwan" }, { "wonderswancolor", "WonderSwanColor" }, { "3do", "3DO" }, { "cdi", "CD-i" },
	{ "amigacd32", "AmigaCD32" }, { "arcade", "Arcade" },
};

typedef struct { char *key, *loose, *path; } dent_t;
typedef struct { char dir[700]; time_t mt; dent_t *e; int n; } dindex_t;
static dindex_t dix[24];
static int ndix;

static dindex_t *docs_index(const char *dir)
{
	struct stat sb;
	if (stat(dir, &sb) || !S_ISDIR(sb.st_mode)) return NULL;
	for (int i = 0; i < ndix; i++)
		if (!strcmp(dix[i].dir, dir)) {
			if (dix[i].mt == sb.st_mtime) return &dix[i];
			for (int j = 0; j < dix[i].n; j++) { free(dix[i].e[j].key); free(dix[i].e[j].loose); free(dix[i].e[j].path); }
			free(dix[i].e);
			memmove(&dix[i], &dix[i + 1], (size_t)(ndix - i - 1) * sizeof(dix[0]));
			ndix--;
			break;
		}
	if (ndix == (int)(sizeof(dix) / sizeof(dix[0]))) {   /* full: the oldest goes */
		for (int j = 0; j < dix[0].n; j++) { free(dix[0].e[j].key); free(dix[0].e[j].loose); free(dix[0].e[j].path); }
		free(dix[0].e);
		memmove(&dix[0], &dix[1], (size_t)(ndix - 1) * sizeof(dix[0]));
		ndix--;
	}
	DIR *d = opendir(dir);
	if (!d) return NULL;
	dindex_t *x = &dix[ndix++];
	memset(x, 0, sizeof(*x));
	snprintf(x->dir, sizeof(x->dir), "%s", dir);
	x->mt = sb.st_mtime;
	int cap2 = 0;
	struct dirent *e;
	while ((e = readdir(d))) {
		const char *nm = e->d_name, *dot = strrchr(nm, '.');
		if (nm[0] == '.' || !dot || (strcasecmp(dot, ".jpg") && strcasecmp(dot, ".png") && strcasecmp(dot, ".jpeg"))) continue;
		if (x->n == cap2) { cap2 = cap2 ? cap2 * 2 : 256; dent_t *ne = realloc(x->e, (size_t)cap2 * sizeof(*ne)); if (!ne) break; x->e = ne; }
		char base[600], k[600], l[600], full[1400];
		snprintf(base, sizeof(base), "%.*s", (int)(dot - nm), nm);
		normkey(base, k, sizeof(k), 0);
		normkey(base, l, sizeof(l), 1);
		snprintf(full, sizeof(full), "%s/%s", dir, nm);
		x->e[x->n].key = strdup(k); x->e[x->n].loose = strdup(l); x->e[x->n].path = strdup(full);
		x->n++;
	}
	closedir(d);
	return x;
}

/* the picture for one name: the same name (case and punctuation aside); else, only when it's the one picture
   with those title words, the name without its (region) tags */
static const char *docs_find(dindex_t *x, const char *name)
{
	char k[600], l[600];
	normkey(name, k, sizeof(k), 0);
	if (!*k) return NULL;
	for (int i = 0; i < x->n; i++) if (!strcmp(x->e[i].key, k)) return x->e[i].path;
	normkey(name, l, sizeof(l), 1);
	if (!*l) return NULL;
	const char *hit = NULL;
	for (int i = 0; i < x->n; i++)
		if (!strcmp(x->e[i].loose, l)) { if (hit) return NULL; hit = x->e[i].path; }
	return hit;
}

static void mra_set(const char *path, char *out, size_t n)
{
	out[0] = 0;
	FILE *f = fopen(path, "r");
	if (!f) return;
	char buf[8192];
	size_t r = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[r] = 0;
	char *p = strstr(buf, "<setname>"), *e = p ? strstr(p, "</setname>") : NULL;
	if (p && e && e - p - 9 > 0 && (size_t)(e - p - 9) < n) snprintf(out, n, "%.*s", (int)(e - p - 9), p + 9);
}

/* "Final Fantasy VII (USA) (Disc 1)" -> "Final Fantasy VII (USA)" */
static void strip_disc(char *s)
{
	char *p;
	while ((p = strcasestr(s, " (Disc ")) || (p = strcasestr(s, " (Track "))) {
		char *e = strchr(p + 1, ')');
		if (!e) break;
		memmove(p, e + 1, strlen(e + 1) + 1);
	}
}

typedef struct { long id; char sys[80]; char unit[1200]; char file[1200]; int arcade; } cand_t;

static cJSON *cmd_art_docs(cJSON *req)
{
	const char *sys = cJSON_GetStringValue(cJSON_GetObjectItem(req, "system"));
	if (sys && !*sys) sys = NULL;
	cJSON *off = cJSON_GetObjectItem(req, "offset"), *lim = cJSON_GetObjectItem(req, "limit");
	int offset = cJSON_IsNumber(off) ? off->valueint : 0, limit = cJSON_IsNumber(lim) ? lim->valueint : 60;
	if (offset < 0) offset = 0;
	if (limit < 1 || limit > 400) limit = 60;
	cand_t *c = calloc((size_t)limit, sizeof(*c));
	if (!c) return fail("out of memory");
	int nc = 0, missing = 0;
	pthread_mutex_lock(&mu);
	fresh();
	for (int i = 0; i < ngames; i++) {
		const game_t *g = &games[i];
		if (sys && strcasecmp(systems[g->sys].name, sys)) continue;
		char art[1500];
		if (!art_file(g, art, sizeof(art))) continue;
		missing++;
		if (missing <= offset || nc >= limit) continue;
		c[nc].id = g->id;
		snprintf(c[nc].sys, sizeof(c[nc].sys), "%s", systems[g->sys].name);
		snprintf(c[nc].unit, sizeof(c[nc].unit), "%s", g->unit);
		snprintf(c[nc].file, sizeof(c[nc].file), "%s", g->file);
		c[nc].arcade = systems[g->sys].cat == CAT_ARCADE;
		nc++;
	}
	pthread_mutex_unlock(&mu);
	int set = 0;
	cJSON *dirs = cJSON_CreateArray();
	for (int i = 0; i < nc; i++) {
		char ns[100];
		normkey(c[i].sys, ns, sizeof(ns), 0);
		const char *ext = strrchr(base_of(c[i].file), '.');
		const char *want[4] = { 0 };
		int nw = 0;
		if (ext && !strcasecmp(ext, ".fds")) want[nw++] = "FDS";
		if (ext && !strcasecmp(ext, ".bs")) want[nw++] = "Satellaview";
		if (ext && !strcasecmp(ext, ".sgx")) want[nw++] = "SuperGrafx";
		if (ext && !strcasecmp(ext, ".gbc")) want[nw++] = "GBC";
		for (size_t a = 0; a < sizeof(DOCS_ALIAS) / sizeof(DOCS_ALIAS[0]) && nw < 4; a++)
			if (!strcmp(DOCS_ALIAS[a][0], ns)) want[nw++] = DOCS_ALIAS[a][1];
		if (!nw) continue;
		/* the names to look for: the file's, a disc set's folder or sheet, the arcade game's set name */
		char names[3][700];
		int nn = 0;
		if (c[i].arcade) { mra_set(c[i].file, names[nn], sizeof(names[0])); if (names[nn][0]) nn++; }
		const char *b = base_of(c[i].file), *dot = strrchr(b, '.');
		snprintf(names[nn], sizeof(names[0]), "%.*s", dot ? (int)(dot - b) : (int)strlen(b), b);
		strip_disc(names[nn]);
		nn++;
		if (strcmp(c[i].unit, c[i].file)) {
			b = base_of(c[i].unit); dot = strrchr(b, '.');
			snprintf(names[nn], sizeof(names[0]), "%.*s", dot && strlen(dot) <= 5 ? (int)(dot - b) : (int)strlen(b), b);
			strip_disc(names[nn]);
			nn++;
		}
		const char *hit = NULL;
		for (int w = 0; w < nw && !hit; w++)
			for (int dv = 0; cm_drives[dv] && !hit; dv++) {
				char dir[700];
				snprintf(dir, sizeof(dir), "%s/docs/%s/Artwork", cm_drives[dv], want[w]);
				dindex_t *x = docs_index(dir);
				if (!x) continue;
				int seen = 0;
				for (int k = 0; k < cJSON_GetArraySize(dirs); k++) if (!strcmp(cJSON_GetArrayItem(dirs, k)->valuestring, dir)) seen = 1;
				if (!seen) cJSON_AddItemToArray(dirs, cJSON_CreateString(dir));
				/* arcade: only the set name (the art is named by it); others: the names in order */
				for (int k = 0; k < (c[i].arcade && nn > 1 ? 1 : nn) && !hit; k++) hit = docs_find(x, names[k]);
			}
		if (!hit) continue;
		char src[1500];
		snprintf(src, sizeof(src), "%s", hit);   /* the index can change under cm_art_set's rescan */
		cJSON *r = cm_art_set(c[i].id, src);
		if (r && cJSON_IsTrue(cJSON_GetObjectItem(r, "ok"))) set++;
		cJSON_Delete(r);
	}
	free(c);
	cJSON *o = ok();
	cJSON_AddNumberToObject(o, "set", set);
	cJSON_AddNumberToObject(o, "looked", nc);
	cJSON_AddNumberToObject(o, "next", offset + nc - set);
	cJSON_AddNumberToObject(o, "left", missing - offset - nc > 0 ? missing - offset - nc : 0);
	cJSON_AddItemToObject(o, "dirs", dirs);
	return o;
}

/* {cmd:"art_remove", id}: the box art an app put in (an undone match), and Console Mode's optimised copy */
static cJSON *cmd_art_remove(long id)
{
	game_t *g = find_game(id);
	if (!g) return fail("no such game");
	char dir[1400], p[1700];
	const char *roots[2] = { dir, g->root };
	parent_of(g->unit, dir, sizeof(dir));
	int gone = 0;
	char ad[600], an[600];
	if (arcade_art(g, ad, sizeof(ad), an, sizeof(an))) {
		snprintf(p, sizeof(p), "%s/%s.png", ad, an);
		if (!unlink(p)) gone++;
		snprintf(p, sizeof(p), "%s/optimized/%s.png", ad, an);
		unlink(p);
	}
	for (int i = 0; i < 2; i++) {
		snprintf(p, sizeof(p), "%s/media/%s.png", roots[i], g->title);
		if (!unlink(p)) gone++;
		snprintf(p, sizeof(p), "%s/media/optimized/%s.png", roots[i], g->title);
		unlink(p);
	}
	generation++;
	cJSON *o = ok();
	cJSON_AddBoolToObject(o, "removed", gone > 0);
	return o;
}

/* the file and system of a game, for {cmd:"ident"} (tools.c): 0 when found */
int cm_game_file(long id, char *file, size_t fn, char *sys, size_t sn, int *cat)
{
	pthread_mutex_lock(&mu);
	fresh();
	game_t *g = find_game(id);
	if (g) {
		snprintf(file, fn, "%s", g->file);
		snprintf(sys, sn, "%s", systems[g->sys].name);
		*cat = systems[g->sys].cat;
	}
	pthread_mutex_unlock(&mu);
	return g ? 0 : -1;
}

/* ---------- the commands ---------- */

cJSON *cm_api(cJSON *req)
{
	const char *cmd = cJSON_GetStringValue(cJSON_GetObjectItem(req, "cmd"));
	if (!cmd) return fail("send a JSON command: {\"cmd\":...}");
	if (!strcmp(cmd, "art_docs")) {   /* takes the lock itself, only while it reads the list (cm_art_set takes it too) */
		static pthread_mutex_t dmu = PTHREAD_MUTEX_INITIALIZER;
		pthread_mutex_lock(&dmu);
		cJSON *r = cmd_art_docs(req);
		pthread_mutex_unlock(&dmu);
		return r;
	}
	pthread_mutex_lock(&mu);
	cJSON *out;
	if (!strcmp(cmd, "rescan")) { scan(); out = ok(); }
	else {
		fresh();
		if (!strcmp(cmd, "ping")) out = ok();
		else if (!strcmp(cmd, "status")) out = cmd_status();
		else if (!strcmp(cmd, "gen")) { out = ok(); cJSON_AddNumberToObject(out, "generation", generation); }
		else if (!strcmp(cmd, "categories")) out = cmd_categories();
		else if (!strcmp(cmd, "groups")) out = cmd_groups(req);
		else if (!strcmp(cmd, "browse")) out = cmd_browse(req);
		else if (!strcmp(cmd, "systems")) out = cmd_systems(req);
		else if (!strcmp(cmd, "games")) out = cmd_games(req);
		else if (!strcmp(cmd, "game")) out = cmd_game(req_id(req));
		else if (!strcmp(cmd, "last")) out = cmd_last();
		else if (!strcmp(cmd, "edit")) out = cmd_edit(req);
		else if (!strcmp(cmd, "scrape_todo")) out = cmd_scrape_todo(req);
		else if (!strcmp(cmd, "art_remove")) out = cmd_art_remove(req_id(req));
		else if (!strcmp(cmd, "launch")) out = cmd_launch(req_id(req));
		else if (!strcmp(cmd, "favorite")) out = fail("favourites are set in Console Mode itself");
		else out = fail("Console Mode doesn't do this from an app");
	}
	pthread_mutex_unlock(&mu);
	return out;
}
