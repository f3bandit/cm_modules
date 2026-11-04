/* cmremote: controllers for the apps (USB and Bluetooth pads, sticks, fight pads): what's connected, each one's
 * buttons and sticks live, which layout to draw, what MiSTer maps them with, and backups of the mapping files
 * (backups/controllers). The same controller database lookups and automatic layout picks on the TV and the PC.
 * Copyright (C) 2026 f3bandit. PolyForm Noncommercial 1.0.0 (see ../LICENSE.md).
 * The controller tester: f3bandit's idea and design.
 *
 * MiSTer keeps button mappings in <fat>/config/inputs:
 *   input_<vid>_<pid>[_hash][_m]_v3.map           the menu's (and every core's, unless it has its own)
 *   <CORE>_input_<vid>_<pid>[_m]_v3.map            one core's own
 *   [<CORE>_]advanced_input_*, *_jk.map, kbd_*     other kinds, kept with the controller's files
 * Without a mapping MiSTer uses SDL's controller database in <fat>/linux/gamecontrollerdb
 * (gamecontrollerdb_user.txt wins). Nothing here writes a mapping: MiSTer's own code does, through
 * Console Mode's remap or MiSTer's own Define joystick buttons; this only backs the files up, removes them (reset)
 * or puts them back. */
#include "ctrl.h"
#include "jobs.h"
#include "cmlib.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <linux/input.h>
#include <math.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

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
	return mkdir(d, 0755) != 0 && errno != EEXIST ? -1 : 0;
}
static int dir_exists(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }

#define MAXDEV 12
#define NBITS(x) ((((x) - 1) / (8 * sizeof(unsigned long))) + 1)
#define TESTBIT(b, a) (((a)[(b) / (8 * sizeof(unsigned long))] >> ((b) % (8 * sizeof(unsigned long)))) & 1UL)

typedef struct { char kind; int code, half, inv; } bind_t;     /* kind: b key, a axis, h hat, 0 none */

/* the layout controls (SS1 Tool's), and SDL's names for them */
enum { C_A, C_B, C_C, C_X, C_Y, C_Z, C_BACK, C_START, C_GUIDE, C_L1, C_R1, C_L2, C_R2, C_L3, C_R3,
       C_UP, C_DOWN, C_LEFT, C_RIGHT, C_LSX, C_LSY, C_RSX, C_RSY, C_N };
static const char *const CTL[C_N] = { "a", "b", "c", "x", "y", "z", "back", "start", "guide", "l1", "r1", "l2", "r2",
	"l3", "r3", "up", "down", "left", "right", "lsx", "lsy", "rsx", "rsy" };
static const char *const SDL[C_N] = { "a", "b", "", "x", "y", "", "back", "start", "guide", "leftshoulder",
	"rightshoulder", "lefttrigger", "righttrigger", "leftstick", "rightstick", "dpup", "dpdown", "dpleft", "dpright",
	"leftx", "lefty", "rightx", "righty" };

typedef struct {
	char ev[16], name[128], db_name[128], where[16];
	unsigned bus, vid, pid, ver;
	int nkeys, keys[96], nabs, abs[24];
	unsigned long long at_edge;    /* axes resting at one end when the controller was looked at: triggers, not sticks */
	int db_user;
	bind_t b[C_N];
} pad_t;

static const char *fat_path(char *out, size_t n, const char *rel)
{
	snprintf(out, n, "%s/%s", cm_fat, rel);
	return out;
}

static int read_line(const char *path, char *out, size_t n)
{
	FILE *f = fopen(path, "r");
	if (!f) return 0;
	int ok = fgets(out, (int)n, f) != NULL;
	fclose(f);
	out[strcspn(out, "\n")] = 0;
	return ok;
}

static unsigned read_hex(const char *path)
{
	char b[32];
	return read_line(path, b, sizeof(b)) ? (unsigned)strtoul(b, NULL, 16) : 0;
}

/* ---------- the database (SDL's gamecontrollerdb), Linux lines only, cached per file change ---------- */

static struct { char *text; time_t mtime[2]; } dbc;

static void db_load(void)
{
	char p[2][PATH_MAX];
	fat_path(p[0], sizeof(p[0]), "linux/gamecontrollerdb/gamecontrollerdb_user.txt");
	fat_path(p[1], sizeof(p[1]), "linux/gamecontrollerdb/gamecontrollerdb.txt");
	struct stat st[2];
	time_t m[2];
	for (int i = 0; i < 2; i++) m[i] = stat(p[i], &st[i]) ? 0 : st[i].st_mtime;
	if (dbc.text && m[0] == dbc.mtime[0] && m[1] == dbc.mtime[1]) return;
	free(dbc.text);
	dbc.text = NULL;
	size_t len = 0, cap = 0;
	for (int i = 0; i < 2; i++) {
		FILE *f = fopen(p[i], "r");
		if (!f) continue;
		char line[2048];
		while (fgets(line, sizeof(line), f)) {
			if (!strstr(line, "platform:Linux")) continue;
			size_t l = strlen(line) + 2;
			if (len + l + 1 > cap) { cap = (len + l + 1) * 2; dbc.text = realloc(dbc.text, cap); }
			dbc.text[len++] = i ? 'S' : 'U';            /* user's file or SDL's */
			memcpy(dbc.text + len, line, l - 2);
			len += l - 2;
			if (dbc.text[len - 1] != '\n') dbc.text[len++] = '\n';
			dbc.text[len] = 0;
		}
		fclose(f);
	}
	dbc.mtime[0] = m[0];
	dbc.mtime[1] = m[1];
}

static unsigned le16hex(const char *s)       /* "c82d" -> 0x2dc8 */
{
	char b[5] = { s[2], s[3], s[0], s[1], 0 };
	return (unsigned)strtoul(b, NULL, 16);
}

/* SDL's b<n>/a<n>/h<n>.<m> into Linux codes, numbered the way SDL numbers them */
static void sdl_binds(pad_t *d, const char *fields)
{
	int btn[96], nb = 0, axes[24], na = 0;
	for (int i = 0; i < d->nkeys; i++) if (d->keys[i] >= BTN_JOYSTICK) btn[nb++] = d->keys[i];
	for (int i = 0; i < d->nkeys; i++) if (d->keys[i] >= BTN_MISC && d->keys[i] < BTN_JOYSTICK) btn[nb++] = d->keys[i];
	for (int i = 0; i < d->nabs; i++) if (d->abs[i] < ABS_HAT0X || d->abs[i] > ABS_HAT3Y) axes[na++] = d->abs[i];
	char *dup = strdup(fields), *save = NULL;
	for (char *f = strtok_r(dup, ",", &save); f; f = strtok_r(NULL, ",", &save)) {
		char *v = strchr(f, ':');
		if (!v || !v[1]) continue;
		*v++ = 0;
		int ci = -1;
		for (int i = 0; i < C_N; i++) if (SDL[i][0] && !strcmp(SDL[i], f)) ci = i;
		if (ci < 0) continue;
		int half = 0, inv = 0;
		if (*v == '+') { half = 1; v++; } else if (*v == '-') { half = -1; v++; }
		size_t l = strlen(v);
		if (l && v[l - 1] == '~') { inv = 1; v[l - 1] = 0; }
		int n = atoi(v + 1);
		if (*v == 'b' && n < nb) d->b[ci] = (bind_t){ 'b', btn[n], 0, 0 };
		else if (*v == 'a' && n < na) d->b[ci] = (bind_t){ 'a', axes[n], half, inv };
		else if (*v == 'h') {
			char *dot = strchr(v, '.');
			if (dot) d->b[ci] = (bind_t){ 'h', ABS_HAT0X + 2 * atoi(v + 1), atoi(dot + 1), 0 };
		}
	}
	free(dup);
}

/* the best entry: same bus and version, then same bus, then any; the user's file wins ties */
static int db_find(pad_t *d)
{
	db_load();
	if (!dbc.text) return 0;
	const char *best = NULL;
	int best_score = -1;
	for (const char *l = dbc.text; *l; ) {
		const char *e = strchr(l, '\n');
		if (!e) break;
		const char *g = l + 1;
		if (e - g > 34 && le16hex(g + 8) == d->vid && le16hex(g + 16) == d->pid) {
			int score = (le16hex(g) == d->bus ? 2 : 0) + (le16hex(g + 24) == d->ver ? 1 : 0) + (*l == 'U' ? 4 : 0);
			if (score >= best_score) { best = l; best_score = score; }
		}
		l = e + 1;
	}
	if (!best) return 0;
	const char *e = strchr(best, '\n');
	char *line = strndup(best + 1, (size_t)(e - best - 1));
	char *c1 = strchr(line, ','), *c2 = c1 ? strchr(c1 + 1, ',') : NULL;
	if (c1 && c2) {
		snprintf(d->db_name, sizeof(d->db_name), "%.*s", (int)(c2 - c1 - 1), c1 + 1);
		d->db_user = *best == 'U';
		sdl_binds(d, c2 + 1);
	}
	free(line);
	return c1 && c2;
}

static int has_key(const pad_t *d, int c) { for (int i = 0; i < d->nkeys; i++) if (d->keys[i] == c) return 1; return 0; }
static int has_abs(const pad_t *d, int c) { for (int i = 0; i < d->nabs; i++) if (d->abs[i] == c) return 1; return 0; }

/* the fallback for controllers that aren't in the database (SS1 Tool's linuxBinds) */
static void linux_binds(pad_t *d)
{
	static const struct { int c, k; } K[] = { { C_A, BTN_SOUTH }, { C_B, BTN_EAST }, { C_X, BTN_WEST }, { C_Y, BTN_NORTH },
		{ C_L1, BTN_TL }, { C_R1, BTN_TR }, { C_BACK, BTN_SELECT }, { C_START, BTN_START }, { C_GUIDE, BTN_MODE },
		{ C_L3, BTN_THUMBL }, { C_R3, BTN_THUMBR }, { C_UP, BTN_DPAD_UP }, { C_DOWN, BTN_DPAD_DOWN },
		{ C_LEFT, BTN_DPAD_LEFT }, { C_RIGHT, BTN_DPAD_RIGHT } };
	for (size_t i = 0; i < sizeof(K) / sizeof(K[0]); i++) if (has_key(d, K[i].k)) d->b[K[i].c] = (bind_t){ 'b', K[i].k, 0, 0 };
	int gp = 0;
	for (int k = BTN_SOUTH; k <= BTN_THUMBR; k++) gp |= has_key(d, k);
	if (!gp) {    /* plain joysticks and cheap pads: the joystick buttons in order, as SDL does */
		static const int ORD[] = { C_A, C_B, C_X, C_Y, C_L1, C_R1, C_L2, C_R2, C_BACK, C_START, C_L3, C_R3 }, ORD4[] = { C_A, C_B, C_BACK, C_START };
		int n = 0;
		for (int k = BTN_JOYSTICK; k <= BTN_DEAD; k++) n += has_key(d, k);
		const int *o = n <= 4 ? ORD4 : ORD;
		int no = n <= 4 ? 4 : 12, i = 0;
		for (int k = BTN_JOYSTICK; k <= BTN_DEAD && i < no; k++) if (has_key(d, k)) d->b[o[i++]] = (bind_t){ 'b', k, 0, 0 };
	}
	int dkeys = has_key(d, BTN_DPAD_UP) || has_key(d, BTN_DPAD_DOWN) || has_key(d, BTN_DPAD_LEFT) || has_key(d, BTN_DPAD_RIGHT);
	if (has_abs(d, ABS_X) && has_abs(d, ABS_Y) && d->nabs == 2 && !dkeys) {   /* only X and Y: a retro pad's D-pad */
		d->b[C_LEFT] = (bind_t){ 'a', ABS_X, -1, 0 }; d->b[C_RIGHT] = (bind_t){ 'a', ABS_X, 1, 0 };
		d->b[C_UP] = (bind_t){ 'a', ABS_Y, -1, 0 }; d->b[C_DOWN] = (bind_t){ 'a', ABS_Y, 1, 0 };
	} else if (has_abs(d, ABS_X) && has_abs(d, ABS_Y)) {
		d->b[C_LSX] = (bind_t){ 'a', ABS_X, 0, 0 }; d->b[C_LSY] = (bind_t){ 'a', ABS_Y, 0, 0 };
	}
	/* Xbox style: right stick on RX/RY, triggers on Z/RZ. Some HID pads have all four the other way round
	   (TinyUSB's gamepad, MC BlueBridge: right stick on Z/RZ, triggers on RX/RY): a trigger rests at one end
	   of its range, a stick in the middle, so the pair resting at an end is the triggers */
	int rx_trig = (d->at_edge >> ABS_RX & 1) || (d->at_edge >> ABS_RY & 1);
	int z_trig = (d->at_edge >> ABS_Z & 1) || (d->at_edge >> ABS_RZ & 1);
	/* MC BlueBridge (Raspberry Pi's 2e8a) in its MiSTer modes (one controller: b0xx, two: 10b3) always has
	   that layout, whatever is held when it's looked at; its Generic HID and X-Input modes have the usual one */
	if (d->vid == 0x2e8a && strstr(d->name, "BlueBridge") && (d->pid == 0x10b3 || (d->pid & 0xf000) == 0xb000)) { rx_trig = 1; z_trig = 0; }
	if (has_abs(d, ABS_RX) && has_abs(d, ABS_RY) && has_abs(d, ABS_Z) && has_abs(d, ABS_RZ) && rx_trig && !z_trig) {
		d->b[C_RSX] = (bind_t){ 'a', ABS_Z, 0, 0 }; d->b[C_RSY] = (bind_t){ 'a', ABS_RZ, 0, 0 };
		d->b[C_L2] = (bind_t){ 'a', ABS_RX, 0, 0 }; d->b[C_R2] = (bind_t){ 'a', ABS_RY, 0, 0 };
	} else if (has_abs(d, ABS_RX) && has_abs(d, ABS_RY)) {
		d->b[C_RSX] = (bind_t){ 'a', ABS_RX, 0, 0 }; d->b[C_RSY] = (bind_t){ 'a', ABS_RY, 0, 0 };
		if (has_abs(d, ABS_Z)) d->b[C_L2] = (bind_t){ 'a', ABS_Z, 0, 0 };
		if (has_abs(d, ABS_RZ)) d->b[C_R2] = (bind_t){ 'a', ABS_RZ, 0, 0 };
	} else if (has_abs(d, ABS_Z) && has_abs(d, ABS_RZ)) {    /* generic HID pads: right stick on Z/RZ */
		d->b[C_RSX] = (bind_t){ 'a', ABS_Z, 0, 0 }; d->b[C_RSY] = (bind_t){ 'a', ABS_RZ, 0, 0 };
	}
	if (!d->b[C_L2].kind) { if (has_abs(d, ABS_BRAKE)) d->b[C_L2] = (bind_t){ 'a', ABS_BRAKE, 0, 0 }; else if (has_key(d, BTN_TL2)) d->b[C_L2] = (bind_t){ 'b', BTN_TL2, 0, 0 }; }
	if (!d->b[C_R2].kind) { if (has_abs(d, ABS_GAS)) d->b[C_R2] = (bind_t){ 'a', ABS_GAS, 0, 0 }; else if (has_key(d, BTN_TR2)) d->b[C_R2] = (bind_t){ 'b', BTN_TR2, 0, 0 }; }
	/* MC BlueBridge's MiSTer modes: its buttons go out in its own order (South, East, West, North, L1, R1, L2,
	   R2, Select, Start, L3, R3, Home), and Linux's plain HID driver numbers them from BTN_SOUTH on, so the
	   names it gives are off from West on (West is BTN_C, Start BTN_TR2...) */
	if (d->vid == 0x2e8a && strstr(d->name, "BlueBridge") && (d->pid == 0x10b3 || (d->pid & 0xf000) == 0xb000)) {
		static const struct { int c, k; } BB[] = { { C_A, BTN_SOUTH }, { C_B, BTN_EAST }, { C_X, BTN_C }, { C_Y, BTN_NORTH },
			{ C_L1, BTN_WEST }, { C_R1, BTN_Z }, { C_BACK, BTN_TL2 }, { C_START, BTN_TR2 }, { C_L3, BTN_SELECT },
			{ C_R3, BTN_START }, { C_GUIDE, BTN_MODE } };
		for (size_t i = 0; i < sizeof(BB) / sizeof(BB[0]); i++) d->b[BB[i].c] = has_key(d, BB[i].k) ? (bind_t){ 'b', BB[i].k, 0, 0 } : (bind_t){ 0 };
	}
	if (!d->b[C_UP].kind && has_abs(d, ABS_HAT0X) && has_abs(d, ABS_HAT0Y)) {
		d->b[C_UP] = (bind_t){ 'h', ABS_HAT0X, 1, 0 }; d->b[C_RIGHT] = (bind_t){ 'h', ABS_HAT0X, 2, 0 };
		d->b[C_DOWN] = (bind_t){ 'h', ABS_HAT0X, 4, 0 }; d->b[C_LEFT] = (bind_t){ 'h', ABS_HAT0X, 8, 0 };
	}
}

/* ---------- connected controllers ---------- */

static int read_bits(int fd, int type, int max, int *out, int cap)
{
	unsigned long bits[NBITS(KEY_MAX + 1)];
	memset(bits, 0, sizeof(bits));
	if (ioctl(fd, EVIOCGBIT(type, sizeof(bits)), bits) < 0) return 0;
	int n = 0;
	for (int i = 0; i <= max && n < cap; i++) if (TESTBIT(i, bits)) out[n++] = i;
	return n;
}

static int pads(pad_t *out, int max)
{
	glob_t g;
	int n = 0;
	if (glob("/sys/class/input/event*", 0, NULL, &g)) return 0;
	for (size_t i = 0; i < g.gl_pathc && n < max; i++) {
		char p[PATH_MAX], js[PATH_MAX], real[PATH_MAX];
		const char *ev = strrchr(g.gl_pathv[i], '/') + 1;
		snprintf(js, sizeof(js), "%s/device/js*", g.gl_pathv[i]);
		glob_t gj;
		int isjs = !glob(js, 0, NULL, &gj);
		if (isjs) globfree(&gj);
		if (!isjs) continue;                                /* not a joystick or pad */
		pad_t *d = &out[n];
		memset(d, 0, sizeof(*d));
		snprintf(d->ev, sizeof(d->ev), "%s", ev);
		snprintf(p, sizeof(p), "%s/device/name", g.gl_pathv[i]); read_line(p, d->name, sizeof(d->name));
		snprintf(p, sizeof(p), "%s/device/id/bustype", g.gl_pathv[i]); d->bus = read_hex(p);
		snprintf(p, sizeof(p), "%s/device/id/vendor", g.gl_pathv[i]); d->vid = read_hex(p);
		snprintf(p, sizeof(p), "%s/device/id/product", g.gl_pathv[i]); d->pid = read_hex(p);
		snprintf(p, sizeof(p), "%s/device/id/version", g.gl_pathv[i]); d->ver = read_hex(p);
		if (d->bus == BUS_VIRTUAL || strstr(d->name, "MiSTer")) continue;   /* software pads (Main's, the remote) */
		snprintf(p, sizeof(p), "%s/device", g.gl_pathv[i]);
		if (!realpath(p, real)) real[0] = 0;
		/* SuperStation One: its own hub on 1-1, the dock's chained on 1-1.1 */
		snprintf(d->where, sizeof(d->where), "%s", d->bus == BUS_BLUETOOTH ? "bluetooth" : strstr(real, "/1-1/1-1.1/") ? "dock" :
			strstr(real, "/usb") ? "superstation" : "other");
		snprintf(p, sizeof(p), "/dev/input/%s", ev);
		int fd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0) continue;
		int k[KEY_MAX + 1 > 768 ? 768 : KEY_MAX + 1];
		int nk = read_bits(fd, EV_KEY, KEY_MAX, k, 768);
		for (int j = 0; j < nk && d->nkeys < 96; j++) if (k[j] >= BTN_MISC) d->keys[d->nkeys++] = k[j];
		d->nabs = read_bits(fd, EV_ABS, ABS_MAX, d->abs, 24);
		for (int j = 0; j < d->nabs; j++) {
			struct input_absinfo ai;
			if (d->abs[j] >= 64 || ioctl(fd, EVIOCGABS(d->abs[j]), &ai) < 0 || ai.maximum - ai.minimum < 8) continue;
			int span = ai.maximum - ai.minimum;
			if (ai.value - ai.minimum < span / 8 || ai.maximum - ai.value < span / 8) d->at_edge |= 1ull << d->abs[j];
		}
		close(fd);
		/* Console Mode's own front-port pad borrows a PS3 pad's ids, but it isn't one: Linux button codes
		   (South, East, North, West, the shoulders, Select, Start, L3, R3) and a hat for the D-pad, where the
		   database's PS3 entry counts a real one's buttons and D-pad buttons (every button came out wrong and
		   the D-pad never) */
		int cm_snac = !strncmp(d->name, "ConsoleMode SNAC", 16);
		if (cm_snac || !db_find(d)) linux_binds(d);
		n++;
	}
	globfree(&g);
	return n;
}

/* ---------- which drawing (SS1 Tool's ptPick) ---------- */

static const char *const STYLES[] = { "generic", "xbox", "dualshock", "switchpro", "nes", "snes", "pce", "gen3", "gen6",
	"neogeo", "jaguar", "saturn", "psx", "n64", "dreamcast", "gamecube", "fight6", "fight8", "leverless", "stick8", "stick6",
	"haute42m", NULL };

static int style_ok(const char *s) { for (int i = 0; STYLES[i]; i++) if (!strcmp(STYLES[i], s)) return 1; return 0; }

static const char *style_auto(const pad_t *d)
{
	static const struct { const char *re, *style; } AUTO[] = {
		{ "cosmox.*m ?ultra|m16 ?ultra", "haute42m" },
		{ "hit ?box|haute|snack ?box|leverless|all.?button|gp2040", "leverless" },
		{ "arcade|fight ?stick|\\brap\\b|f[35]00|qanba|\\bstick\\b", "stick8" },
		{ "m30|saturn", "saturn" }, { "genesis|mega ?drive|sega", "gen6" },
		{ "fighting commander|fight ?pad|fusion", "fight8" }, { "n64|nintendo 64", "n64" },
		{ "gamecube|\\bgc\\b", "gamecube" }, { "dreamcast", "dreamcast" },
		{ "snes|super famicom|sfc|^retro controller$", "snes" }, { "\\bnes\\b|famicom", "nes" },
		{ "pc ?engine|turbo ?grafx", "pce" }, { "neo ?geo", "neogeo" }, { "jaguar", "jaguar" },
		{ "dual ?shock|dualsense|playstation|ps[345]|sony", "dualshock" },
		{ "switch|pro controller|joy-?con|nintendo", "switchpro" }, { "xbox|x-box|xinput", "xbox" },
	};
	const char *names[2] = { d->db_name, d->name };
	for (int n = 0; n < 2; n++) {
		if (!names[n][0]) continue;
		for (size_t i = 0; i < sizeof(AUTO) / sizeof(AUTO[0]); i++) {
			regex_t re;
			if (regcomp(&re, AUTO[i].re, REG_EXTENDED | REG_ICASE | REG_NOSUB)) continue;
			int m = !regexec(&re, names[n], 0, NULL, 0);
			regfree(&re);
			if (m) return AUTO[i].style;
		}
	}
	static const struct { unsigned vid, pid; const char *style; } VID[] = {
		{ 0x057e, 0x2017, "snes" }, { 0x057e, 0x2019, "n64" }, { 0x057e, 0x201e, "gen6" }, { 0x057e, 0x0337, "gamecube" },
		{ 0x045e, 0, "xbox" }, { 0x054c, 0, "dualshock" }, { 0x057e, 0, "switchpro" },
	};
	for (size_t i = 0; i < sizeof(VID) / sizeof(VID[0]); i++)
		if (VID[i].vid == d->vid && (!VID[i].pid || VID[i].pid == d->pid)) return VID[i].style;
	int face = !!d->b[C_A].kind + !!d->b[C_B].kind + !!d->b[C_X].kind + !!d->b[C_Y].kind;
	if (d->b[C_LSX].kind || d->b[C_RSX].kind) return "xbox";
	if (has_key(d, BTN_C) || has_key(d, BTN_Z)) return "gen6";
	if (face <= 2 && !d->b[C_L1].kind && !d->b[C_R1].kind) return "nes";
	if (d->b[C_L2].kind && d->b[C_R2].kind) return "fight8";
	if (face >= 4 && (d->b[C_L1].kind || d->b[C_R1].kind)) return "snes";
	return "generic";
}

/* the drawing picked by hand, per controller (vid:pid): <data>/controller-styles.txt */
static void styles_path(char *out, size_t n) { snprintf(out, n, "%s/controller-styles.txt", cmod_home()); }

static int style_get(unsigned vid, unsigned pid, char *out, size_t n)
{
	char p[PATH_MAX], line[96], key[16];
	styles_path(p, sizeof(p));
	snprintf(key, sizeof(key), "%04x:%04x ", vid, pid);
	FILE *f = fopen(p, "r");
	if (!f) return 0;
	int found = 0;
	while (fgets(line, sizeof(line), f))
		if (!strncmp(line, key, strlen(key))) { line[strcspn(line, "\r\n")] = 0; snprintf(out, n, "%s", line + strlen(key)); found = style_ok(out); }
	fclose(f);
	return found;
}

static int style_set(unsigned vid, unsigned pid, const char *style)
{
	char p[PATH_MAX], tmp[PATH_MAX], line[96], key[16];
	styles_path(p, sizeof(p));
	snprintf(tmp, sizeof(tmp), "%s.new", p);
	snprintf(key, sizeof(key), "%04x:%04x ", vid, pid);
	FILE *in = fopen(p, "r"), *out = fopen(tmp, "w");
	if (!out) { if (in) fclose(in); return -1; }
	if (in) {
		while (fgets(line, sizeof(line), in)) if (strncmp(line, key, strlen(key))) fputs(line, out);
		fclose(in);
	}
	if (style && strcmp(style, "auto")) fprintf(out, "%s%s\n", key, style);
	fclose(out);
	return rename(tmp, p);
}

/* ---------- mapping files and backups ---------- */

static void inputs_dir(char *out, size_t n) { fat_path(out, n, "config/inputs"); }
static void backups_dir(char *out, size_t n) { snprintf(out, n, "%s/backups/controllers", cmod_home()); }

/* is this file one of this controller's (vid_pid)? sets *core to its core ("" for the menu's) */
static int map_of(const char *f, const char *id, char *core, size_t cn)
{
	char lf[256], pat[48];
	snprintf(lf, sizeof(lf), "%s", f);
	for (char *c = lf; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
	snprintf(pat, sizeof(pat), "input_%s", id);
	if (!strncmp(lf, pat, strlen(pat))) { core[0] = 0; return 1; }
	snprintf(pat, sizeof(pat), "kbd_%s", id);
	if (!strncmp(lf, pat, strlen(pat))) { snprintf(core, cn, "(keyboard)"); return 1; }
	snprintf(pat, sizeof(pat), "_input_%s", id);
	char *at = strstr(lf, pat);
	if (!at) return 0;
	int len = (int)(at - lf);
	const char *adv = strstr(lf, "_advanced");
	if (adv && adv < at) len = (int)(adv - lf);
	snprintf(core, cn, "%.*s", len, f);
	if (!strncmp(lf, "advanced", 8)) snprintf(core, cn, "(advanced)");
	return 1;
}

static int is_map_file(const char *n)
{
	size_t l = strlen(n);
	return l > 4 && !strcmp(n + l - 4, ".map");
}

static int copy_file(const char *from, const char *to)
{
	FILE *a = fopen(from, "rb"), *b = a ? fopen(to, "wb") : NULL;
	if (!a || !b) { if (a) fclose(a); return -1; }
	char buf[8192];
	size_t r;
	int ok = 1;
	while ((r = fread(buf, 1, sizeof(buf), a)) > 0) if (fwrite(buf, 1, r, b) != r) ok = 0;
	if (ferror(a)) ok = 0;
	fclose(a);
	if (fclose(b)) ok = 0;
	if (!ok) unlink(to);                             /* no half copy left behind (a full card) */
	return ok ? 0 : -1;
}

/* every mapping file and the user's controller profiles, into a new backup; returns files copied, or
   -1 when any of them couldn't be copied (a full card): nothing may be deleted on the strength of it */
static int backup_now(const char *label, char *made, size_t mn)
{
	char in[PATH_MAX], root[PATH_MAX], dir[PATH_MAX], a[PATH_MAX], b[PATH_MAX], stamp[32], clean[48];
	inputs_dir(in, sizeof(in));
	backups_dir(root, sizeof(root));
	time_t t = time(NULL);
	strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", localtime(&t));
	int k = 0;
	for (const char *c = label ? label : ""; *c && k < (int)sizeof(clean) - 1; c++)
		clean[k++] = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '-' ? *c : '_';
	clean[k] = 0;
	snprintf(dir, sizeof(dir), "%s/%s%s%s", root, stamp, k ? "_" : "", clean);
	if (mkdirs(dir)) return -1;
	int n = 0, failed = 0;
	DIR *d = opendir(in);
	if (d) {
		struct dirent *e;
		while ((e = readdir(d))) {
			if (!is_map_file(e->d_name)) continue;
			snprintf(a, sizeof(a), "%s/%s", in, e->d_name);
			snprintf(b, sizeof(b), "%s/%s", dir, e->d_name);
			if (copy_file(a, b) == 0) n++;
			else failed++;
		}
		closedir(d);
	}
	fat_path(a, sizeof(a), "linux/gamecontrollerdb/gamecontrollerdb_user.txt");
	snprintf(b, sizeof(b), "%s/gamecontrollerdb_user.txt", dir);
	if (file_exists(a)) { if (copy_file(a, b) == 0) n++; else failed++; }
	sync();
	if (failed) { plog("controllers: backup failed (%d file%s not copied): nothing changed", failed, failed == 1 ? "" : "s"); return -1; }
	snprintf(made, mn, "%s", strrchr(dir, '/') + 1);
	return n;
}

static int valid_backup(const char *name)
{
	if (!name || !*name || strchr(name, '/') || strstr(name, "..")) return 0;
	char p[PATH_MAX];
	backups_dir(p, sizeof(p));
	strncat(p, "/", sizeof(p) - strlen(p) - 1);
	strncat(p, name, sizeof(p) - strlen(p) - 1);
	return dir_exists(p);
}

/* the mapping files exactly as they were in that backup (the current ones are backed up first) */
static int restore(const char *name, char *before, size_t bn)
{
	char in[PATH_MAX], dir[PATH_MAX], a[PATH_MAX], b[PATH_MAX];
	if (backup_now("before-restore", before, bn) < 0) return -1;
	inputs_dir(in, sizeof(in));
	backups_dir(dir, sizeof(dir));
	strncat(dir, "/", sizeof(dir) - strlen(dir) - 1);
	strncat(dir, name, sizeof(dir) - strlen(dir) - 1);
	mkdirs(in);
	DIR *d = opendir(in);
	if (d) {
		struct dirent *e;
		while ((e = readdir(d))) if (is_map_file(e->d_name)) { snprintf(a, sizeof(a), "%s/%s", in, e->d_name); unlink(a); }
		closedir(d);
	}
	int n = 0;
	if ((d = opendir(dir))) {
		struct dirent *e;
		while ((e = readdir(d))) {
			snprintf(a, sizeof(a), "%s/%s", dir, e->d_name);
			if (is_map_file(e->d_name)) snprintf(b, sizeof(b), "%s/%s", in, e->d_name);
			else if (!strcmp(e->d_name, "gamecontrollerdb_user.txt")) fat_path(b, sizeof(b), "linux/gamecontrollerdb/gamecontrollerdb_user.txt");
			else continue;
			if (copy_file(a, b) == 0) n++;
		}
		closedir(d);
	}
	sync();
	return n;
}

/* this controller's mapping files away (backed up first): MiSTer goes back to its automatic mapping */
static int reset(unsigned vid, unsigned pid, char *before, size_t bn)
{
	char in[PATH_MAX], a[PATH_MAX], id[16], core[64];
	snprintf(id, sizeof(id), "%04x_%04x", vid, pid);
	if (backup_now("before-reset", before, bn) < 0) return -1;
	inputs_dir(in, sizeof(in));
	int n = 0;
	DIR *d = opendir(in);
	if (d) {
		struct dirent *e;
		while ((e = readdir(d)))
			if (is_map_file(e->d_name) && map_of(e->d_name, id, core, sizeof(core))) {
				snprintf(a, sizeof(a), "%s/%s", in, e->d_name);
				if (!unlink(a)) n++;
			}
		closedir(d);
	}
	sync();
	return n;
}

/* ---------- live state ---------- */

typedef struct { int has; double v, x, y; int on; } ctl_t;

static double norm(const struct input_absinfo *a)
{
	if (a->maximum <= a->minimum) return 0;
	return 2.0 * (a->value - a->minimum) / (double)(a->maximum - a->minimum) - 1;
}

static cJSON *live(const pad_t *d)
{
	char p[64];
	snprintf(p, sizeof(p), "/dev/input/%s", d->ev);
	int fd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) return NULL;
	unsigned long keys[NBITS(KEY_MAX + 1)];
	memset(keys, 0, sizeof(keys));
	ioctl(fd, EVIOCGKEY(sizeof(keys)), keys);
	struct input_absinfo ab[ABS_MAX + 1];
	memset(ab, 0, sizeof(ab));
	for (int i = 0; i < d->nabs; i++) ioctl(fd, EVIOCGABS(d->abs[i]), &ab[d->abs[i]]);
	close(fd);
	ctl_t c[C_N];
	memset(c, 0, sizeof(c));
	for (int i = 0; i < C_N; i++) {
		const bind_t *b = &d->b[i];
		double v = 0;
		if (b->kind == 'b') { c[i].has = 1; v = TESTBIT(b->code, keys) ? 1 : 0; }
		else if (b->kind == 'a') {
			c[i].has = 1;
			v = norm(&ab[b->code]);
			if (b->inv) v = -v;
			if (b->half == 1) v = v > 0 ? v : 0;
			else if (b->half == -1) v = v < 0 ? -v : 0;
			if ((i == C_L2 || i == C_R2) && !b->half) v = (v + 1) / 2;
		} else if (b->kind == 'h') {
			c[i].has = 1;
			int x = ab[b->code].value, y = ab[b->code + 1].value;
			v = ((b->half & 1) && y < 0) || ((b->half & 4) && y > 0) || ((b->half & 8) && x < 0) || ((b->half & 2) && x > 0);
		}
		c[i].v = v;
	}
	cJSON *o = cJSON_CreateObject(), *on = cJSON_AddArrayToObject(o, "on");
	for (int i = 0; i < C_N; i++) {
		if (i == C_LSX || i == C_LSY || i == C_RSX || i == C_RSY) continue;
		if (c[i].has && c[i].v > ((i == C_L2 || i == C_R2) && d->b[i].kind == 'a' ? 0.3 : 0.5)) cJSON_AddItemToArray(on, cJSON_CreateString(CTL[i]));
	}
	/* C and Z on 6-button pads, when the database gave those buttons no other name */
	/* (not when C or Z is already another button's code: MC BlueBridge's X is BTN_C, its R1 BTN_Z) */
	int c_used = 0, z_used = 0;
	for (int i = 0; i < C_N; i++) if (d->b[i].kind == 'b') { c_used |= d->b[i].code == BTN_C; z_used |= d->b[i].code == BTN_Z; }
	if (!c_used && has_key(d, BTN_C) && TESTBIT(BTN_C, keys)) cJSON_AddItemToArray(on, cJSON_CreateString("c"));
	if (!z_used && has_key(d, BTN_Z) && TESTBIT(BTN_Z, keys)) cJSON_AddItemToArray(on, cJSON_CreateString("z"));
	const char *st[2] = { "ls", "rs" };
	for (int s = 0; s < 2; s++) {
		int ix = s ? C_RSX : C_LSX;
		if (!c[ix].has) continue;
		cJSON *a = cJSON_AddArrayToObject(o, st[s]);
		cJSON_AddItemToArray(a, cJSON_CreateNumber(round(c[ix].v * 100) / 100));
		cJSON_AddItemToArray(a, cJSON_CreateNumber(round(c[ix + 1].v * 100) / 100));
	}
	if (c[C_L2].has) cJSON_AddNumberToObject(o, "l2", round(c[C_L2].v * 100) / 100);
	if (c[C_R2].has) cJSON_AddNumberToObject(o, "r2", round(c[C_R2].v * 100) / 100);
	return o;
}

/* ---------- the command ---------- */

static void add_pad(cJSON *arr, const pad_t *d, const char *files)
{
	cJSON *x = cJSON_CreateObject();
	char vp[16], id[16], style[24];
	snprintf(vp, sizeof(vp), "%04x:%04x", d->vid, d->pid);
	snprintf(id, sizeof(id), "%04x_%04x", d->vid, d->pid);
	cJSON_AddStringToObject(x, "id", d->ev);
	cJSON_AddStringToObject(x, "name", d->name);
	cJSON_AddStringToObject(x, "vidpid", vp);
	cJSON_AddStringToObject(x, "where", d->where);
	cJSON_AddNumberToObject(x, "buttons", d->nkeys);
	cJSON_AddNumberToObject(x, "axes", d->nabs);
	cJSON_AddStringToObject(x, "db_name", d->db_name);
	cJSON_AddBoolToObject(x, "db_user", d->db_user);
	const char *a = style_auto(d);
	int set = style_get(d->vid, d->pid, style, sizeof(style));
	cJSON_AddStringToObject(x, "style", set ? style : a);
	cJSON_AddStringToObject(x, "style_auto", a);
	cJSON_AddBoolToObject(x, "style_set", set);
	cJSON *has = cJSON_AddArrayToObject(x, "has");     /* the controls it has (the drawing dims the rest) */
	for (int i = 0; i < C_N; i++) if (d->b[i].kind) cJSON_AddItemToArray(has, cJSON_CreateString(CTL[i]));
	if (has_key(d, BTN_C)) cJSON_AddItemToArray(has, cJSON_CreateString("c"));
	if (has_key(d, BTN_Z)) cJSON_AddItemToArray(has, cJSON_CreateString("z"));
	/* MiSTer's mapping: custom (files), or the database, or none yet */
	cJSON *m = cJSON_AddObjectToObject(x, "mapping"), *cores = cJSON_AddArrayToObject(m, "cores");
	int custom = 0;
	char core[64];
	for (const char *f = files; f && *f; f += strlen(f) + 1)
		if (map_of(f, id, core, sizeof(core))) {
			custom = 1;
			if (core[0]) cJSON_AddItemToArray(cores, cJSON_CreateString(core));
			else cJSON_AddStringToObject(m, "menu", f);
		}
	cJSON_AddStringToObject(m, "uses", custom ? "custom" : d->db_name[0] ? "database" : "none");
	cJSON_AddItemToArray(arr, x);
}

/* the mapping files, as one block of NUL-separated names */
static char *list_files(cJSON *out)
{
	char in[PATH_MAX], a[PATH_MAX];
	inputs_dir(in, sizeof(in));
	size_t len = 0, cap = 1024;
	char *all = calloc(1, cap);
	cJSON *arr = out ? cJSON_AddArrayToObject(out, "files") : NULL;
	DIR *d = opendir(in);
	if (d) {
		struct dirent *e;
		while ((e = readdir(d))) {
			if (!is_map_file(e->d_name)) continue;
			size_t l = strlen(e->d_name) + 1;
			if (len + l + 1 > cap) { cap = (len + l + 1) * 2; all = realloc(all, cap); }
			memcpy(all + len, e->d_name, l);
			len += l;
			all[len] = 0;
			if (arr) {
				struct stat st;
				snprintf(a, sizeof(a), "%s/%s", in, e->d_name);
				cJSON *f = cJSON_CreateObject();
				cJSON_AddStringToObject(f, "name", e->d_name);
				cJSON_AddNumberToObject(f, "mtime", stat(a, &st) ? 0 : (double)st.st_mtime);
				cJSON_AddItemToArray(arr, f);
			}
		}
		closedir(d);
	}
	return all;
}

static void list_backups(cJSON *out)
{
	char root[PATH_MAX], p[PATH_MAX];
	backups_dir(root, sizeof(root));
	cJSON *arr = cJSON_AddArrayToObject(out, "backups");
	struct dirent **nl = NULL;
	int n = scandir(root, &nl, NULL, alphasort);
	for (int i = n - 1; i >= 0; i--) {             /* newest first */
		if (nl[i]->d_name[0] != '.') {
			snprintf(p, sizeof(p), "%s/%s", root, nl[i]->d_name);
			if (dir_exists(p)) {
				int files = 0;
				DIR *d = opendir(p);
				if (d) { struct dirent *e; while ((e = readdir(d))) files += e->d_name[0] != '.'; closedir(d); }
				cJSON *b = cJSON_CreateObject();
				cJSON_AddStringToObject(b, "name", nl[i]->d_name);
				cJSON_AddNumberToObject(b, "files", files);
				cJSON_AddItemToArray(arr, b);
			}
		}
		free(nl[i]);
	}
	free(nl);
}

/* the mapping the shell runs (its locked window): requests go in <tmp>/map-request, its state comes
   back in <tmp>/map-state (one JSON line) */
static void map_state(cJSON *out)
{
	char p[PATH_MAX], line[512];
	snprintf(p, sizeof(p), "%s/map-state", cmod_tmp());
	cJSON *s = read_line(p, line, sizeof(line)) ? cJSON_Parse(line) : NULL;
	cJSON_AddItemToObject(out, "map", s ? s : cJSON_CreateObject());
}

static int map_request(const char *what)
{
	char p[PATH_MAX], tmp[PATH_MAX];
	snprintf(p, sizeof(p), "%s/map-request", cmod_tmp());
	snprintf(tmp, sizeof(tmp), "%s.new", p);
	FILE *f = fopen(tmp, "w");
	if (!f) return -1;
	fprintf(f, "%s\n", what);
	fclose(f);
	return rename(tmp, p);
}

static int parse_vidpid(cJSON *req, unsigned *vid, unsigned *pid)
{
	const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(req, "vidpid"));
	return v && sscanf(v, "%4x:%4x", vid, pid) == 2;
}

/* the input devices there are now ("event0 event3 ..."): the controllers are looked up again when it changes */
static void ev_sig(char *sig, size_t n)
{
	glob_t g;
	sig[0] = 0;
	if (glob("/dev/input/event*", 0, NULL, &g)) return;
	for (size_t i = 0; i < g.gl_pathc; i++) { strncat(sig, g.gl_pathv[i] + 11, n - strlen(sig) - 2); strncat(sig, " ", n - strlen(sig) - 1); }
	globfree(&g);
}
static int pads_n = -1;
static time_t pads_at;
static char pads_sig[512];
static void pads_look(pad_t *P, const char *sig) { pads_n = pads(P, MAXDEV); pads_at = time(NULL); snprintf(pads_sig, sizeof(pads_sig), "%s", sig); }

/* ---------- RetroSpy (retrospy.c): a controller's layout and its controls, numbered as its jsN device does ---------- */

/* joydev numbers buttons BTN_JOYSTICK..KEY_MAX first, then BTN_MISC..BTN_JOYSTICK-1; axes in code order */
static int js_button(const pad_t *d, int code)
{
	int n = 0;
	for (int i = 0; i < d->nkeys; i++) if (d->keys[i] >= BTN_JOYSTICK) { if (d->keys[i] == code) return n; n++; }
	for (int i = 0; i < d->nkeys; i++) if (d->keys[i] >= BTN_MISC && d->keys[i] < BTN_JOYSTICK) { if (d->keys[i] == code) return n; n++; }
	return -1;
}

static int js_axis(const pad_t *d, int code)
{
	for (int i = 0; i < d->nabs; i++) if (d->abs[i] == code) return i;
	return -1;
}

static cJSON *rs_bind(const pad_t *d, int c)
{
	const bind_t *b = &d->b[c];
	cJSON *o = NULL;
	if (b->kind == 'b') {
		int i = js_button(d, b->code);
		if (i >= 0) { o = cJSON_CreateObject(); cJSON_AddNumberToObject(o, "b", i); }
	} else if (b->kind == 'a') {
		int i = js_axis(d, b->code);
		if (i >= 0) {
			o = cJSON_CreateObject();
			cJSON_AddNumberToObject(o, "a", i);
			if (b->half) cJSON_AddNumberToObject(o, "half", b->half);
			if (b->inv) cJSON_AddBoolToObject(o, "inv", 1);
		}
	} else if (b->kind == 'h') {                  /* a hat: two axes on jsN, up/down on its Y, left/right on its X */
		int vert = b->half & 5, i = js_axis(d, b->code + (vert ? 1 : 0));
		if (i >= 0) {
			o = cJSON_CreateObject();
			cJSON_AddNumberToObject(o, "a", i);
			cJSON_AddNumberToObject(o, "half", (b->half & 9) ? -1 : 1);     /* up, left: negative */
		}
	}
	return o;
}

cJSON *ctrl_retrospy(const char *ev)
{
	static pad_t P[MAXDEV];
	static int n = -1;
	static char sig[256];
	char now[256];
	ev_sig(now, sizeof(now));
	if (n < 0 || strcmp(now, sig)) { n = pads(P, MAXDEV); snprintf(sig, sizeof(sig), "%s", now); }
	for (int k = 0; k < n; k++) {
		const pad_t *d = &P[k];
		if (strcmp(d->ev, ev)) continue;
		cJSON *o = cJSON_CreateObject();
		char style[24];
		const char *a = style_auto(d);
		cJSON_AddStringToObject(o, "layout", style_get(d->vid, d->pid, style, sizeof(style)) ? style : a);
		char vp[16];
		snprintf(vp, sizeof(vp), "%04x:%04x", d->vid, d->pid);
		cJSON_AddStringToObject(o, "vidpid", vp);
		cJSON *bs = cJSON_AddObjectToObject(o, "binds");
		for (int c = 0; c < C_N; c++) { cJSON *b = rs_bind(d, c); if (b) cJSON_AddItemToObject(bs, CTL[c], b); }
		/* C and Z on 6-button pads, when no control has those buttons already */
		int cu = 0, zu = 0;
		for (int c = 0; c < C_N; c++) if (d->b[c].kind == 'b') { cu |= d->b[c].code == BTN_C; zu |= d->b[c].code == BTN_Z; }
		if (!cu && has_key(d, BTN_C)) { cJSON *b = cJSON_CreateObject(); cJSON_AddNumberToObject(b, "b", js_button(d, BTN_C)); cJSON_AddItemToObject(bs, "c", b); }
		if (!zu && has_key(d, BTN_Z)) { cJSON *b = cJSON_CreateObject(); cJSON_AddNumberToObject(b, "b", js_button(d, BTN_Z)); cJSON_AddItemToObject(bs, "z", b); }
		cJSON_AddNumberToObject(o, "js_buttons", d->nkeys);
		cJSON_AddNumberToObject(o, "js_axes", d->nabs);
		return o;
	}
	return NULL;
}

cJSON *ctrl_command(cJSON *req, const char **err)
{
	static pad_t P[MAXDEV];
	const char *d = cJSON_GetStringValue(cJSON_GetObjectItem(req, "do"));
	const char *lv = cJSON_GetStringValue(cJSON_GetObjectItem(req, "live"));
	char made[96];
	unsigned vid = 0, pid = 0;
	/* tests (snapshots without controllers): CMOD_CTRL_TEST is a file with a whole reply */
	const char *test = getenv("CMOD_CTRL_TEST");
	if (test && *test) {
		FILE *f = fopen(test, "r");
		char buf[16384];
		size_t r = f ? fread(buf, 1, sizeof(buf) - 1, f) : 0;
		if (f) fclose(f);
		buf[r] = 0;
		cJSON *t = cJSON_Parse(buf);
		if (t) return t;
	}
	cJSON *o = cJSON_CreateObject();
	if (lv) {                                      /* what's pressed now on one controller */
		/* the controller test and the mapping window ask 10-16 times a second: the controllers are looked
		   up again (every input device opened, the controller database searched, about 0.5 s on the SS1)
		   only when the set of input devices changes, else the last look is used (the menu
		   waited for each answer, and lagged) */
		char sig[512];
		ev_sig(sig, sizeof(sig));
		if (pads_n < 0 || strcmp(sig, pads_sig)) pads_look(P, sig);
		int n = pads_n;
		for (int i = 0; i < n; i++)
			if (!strcmp(P[i].ev, lv)) { cJSON *s = live(&P[i]); if (s) cJSON_AddItemToObject(o, "live", s); }
		map_state(o);
		if (!cJSON_GetObjectItem(o, "live")) { cJSON_Delete(o); *err = "that controller isn't connected"; return NULL; }
		return o;
	}
	if (d && *d) {
		if (!strcmp(d, "style")) {
			const char *s = cJSON_GetStringValue(cJSON_GetObjectItem(req, "style"));
			if (!parse_vidpid(req, &vid, &pid) || !s || (strcmp(s, "auto") && !style_ok(s))) { *err = "style: vidpid and a style (or auto)"; goto bad; }
			if (style_set(vid, pid, s)) { *err = "couldn't save the drawing"; goto bad; }
		} else if (!strcmp(d, "reset")) {
			if (!parse_vidpid(req, &vid, &pid)) { *err = "reset: vidpid"; goto bad; }
			int n = reset(vid, pid, made, sizeof(made));
			if (n < 0) { *err = "couldn't back up the mappings first, nothing was removed"; goto bad; }
			plog("controllers: reset %04x:%04x to automatic (%d files, backup %s)", vid, pid, n, made);
			cJSON_AddNumberToObject(o, "removed", n);
			cJSON_AddStringToObject(o, "backup", made);
		} else if (!strcmp(d, "backup")) {
			int n = backup_now(cJSON_GetStringValue(cJSON_GetObjectItem(req, "name")), made, sizeof(made));
			if (n < 0) { *err = "couldn't make the backup"; goto bad; }
			cJSON_AddNumberToObject(o, "saved", n);
			cJSON_AddStringToObject(o, "backup", made);
		} else if (!strcmp(d, "restore")) {
			const char *b = cJSON_GetStringValue(cJSON_GetObjectItem(req, "name"));
			if (!valid_backup(b)) { *err = "no such backup"; goto bad; }
			int n = restore(b, made, sizeof(made));
			if (n < 0) { *err = "couldn't back up the current mappings first, nothing was changed"; goto bad; }
			plog("controllers: restored %s (%d files; the ones before are in %s)", b, n, made);
			cJSON_AddNumberToObject(o, "restored", n);
			cJSON_AddStringToObject(o, "backup", made);
		} else if (!strcmp(d, "map")) {
			char what[32];
			if (!parse_vidpid(req, &vid, &pid)) { *err = "map: vidpid"; goto bad; }
			snprintf(what, sizeof(what), "start %04x:%04x", vid, pid);
			if (map_request(what)) { *err = "couldn't ask the TV"; goto bad; }
		} else if (!strcmp(d, "map_skip") || !strcmp(d, "map_cancel")) {
			if (map_request(d + 4)) { *err = "couldn't ask the TV"; goto bad; }
		} else { *err = "do: style, reset, backup, restore, map, map_skip or map_cancel"; goto bad; }
	}
	char *files = list_files(o);
	cJSON *arr = cJSON_AddArrayToObject(o, "controllers");
	char sig[512];
	ev_sig(sig, sizeof(sig));
	/* a controller is worked out once, when it comes: looked up again only when one comes or goes, or after
	   a change made here (a mapping, a style). The list is asked for every 2 s, and a look takes 0.5 s */
	if (pads_n < 0 || (d && *d) || strcmp(sig, pads_sig)) pads_look(P, sig);
	int n = pads_n;
	for (int i = 0; i < n; i++) add_pad(arr, &P[i], files);
	free(files);
	list_backups(o);
	map_state(o);
	return o;
bad:
	cJSON_Delete(o);
	return NULL;
}
