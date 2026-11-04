/* cmremote: MiSTer's own video settings (the [MiSTer] section of MiSTer.ini), with safe trying.
 * Copyright (C) 2026 f3bandit. PolyForm Noncommercial 1.0.0 (see ../LICENSE.md); Retro Remake: see ../LICENSE-RETRO-REMAKE.md.
 *
 * Main reads MiSTer.ini when it starts; a try writes the change and restarts Main ("load_core
 * menu.rbf": Console Mode comes back; cmremote keeps running). Before that the file is kept in
 * backups/ini (like every ini save), and a timer is started on the MiSTer itself, as its
 * own process: unless Keep comes in time (35 s), it puts the old file back and restarts Main again.
 * So a picture that's gone, a phone or PC that drops off, or cmremote itself stopping still
 * rolls back. The try is also written to the SD card: if the power goes before Keep, cmremote
 * puts the old file back when it starts. */
#include "vtry.h"
#include "extras.h"
#include "compat.h"
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TEXT_MAX (256 * 1024)

/* the settings the apps and the menu show and change (the [MiSTer] section only) */
static const char *KEYS[] = {
	"video_mode", "video_mode_ntsc", "video_mode_pal", "vsync_adjust", "vscale_mode", "vscale_border",
	"hdmi_limited", "dvi_mode", "hdmi_game_mode", "hdr", "vrr_mode", "hdmi_audio_96k", "direct_video",
	"vga_mode", "composite_sync", "vga_sog", "forced_scandoubler", "vga_scaler", "menu_pal", "ntsc_mode",
	"video_info", "fb_terminal",
	"hdmi_cec", "hdmi_cec_input_mode", "hdmi_cec_power_on", "hdmi_cec_sleep", "hdmi_cec_wake", "hdmi_cec_clock",
	"hdmi_off", "video_off", "video_off_logo", NULL,
};

static int key_ok(const char *k)
{
	for (int i = 0; KEYS[i]; i++) if (!strcasecmp(KEYS[i], k)) return 1;
	return 0;
}

static int value_ok(const char *v)
{
	if (strlen(v) > 80) return 0;
	for (const char *p = v; *p; p++)
		if (!isalnum((unsigned char)*p) && !strchr("_.,:+ -", *p)) return 0;
	return 1;
}

static void bk_path(char *out, size_t n, const char *kept) { snprintf(out, n, "%s/backups/ini/%s", p_data(), kept); }
static void marker_path(char *out, size_t n) { snprintf(out, n, "%s/video_try", p_data()); }

static char *slurp(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	char *b = malloc(TEXT_MAX + 1);
	size_t r = b ? fread(b, 1, TEXT_MAX + 1, f) : 0;
	fclose(f);
	if (!b || r > TEXT_MAX) { free(b); return NULL; }
	b[r] = 0;
	return b;
}

static int write_atomic(const char *path, const char *data, size_t n)
{
	char tmp[700];
	snprintf(tmp, sizeof(tmp), "%s.cm-new", path);
	FILE *f = fopen(tmp, "wb");
	if (!f) return -1;
	int bad = fwrite(data, 1, n, f) != n;
	bad |= fflush(f) != 0;
	bad |= fsync(fileno(f)) != 0;
	bad |= fclose(f) != 0;
	if (bad || rename(tmp, path)) { unlink(tmp); return -1; }
	sync();
	return 0;
}

/* ---------- ini lines ---------- */

typedef struct {
	char **l;
	int n;
	int crlf;
} lines_t;

static void lines_split(const char *text, lines_t *L)
{
	L->crlf = strstr(text, "\r\n") != NULL;
	L->n = 0;
	int cap = 64;
	L->l = malloc(sizeof(char *) * (size_t)cap);
	const char *p = text;
	for (;;) {
		const char *e = strchr(p, '\n');
		size_t len = e ? (size_t)(e - p) : strlen(p);
		if (len && p[len - 1] == '\r') len--;
		if (L->n == cap) L->l = realloc(L->l, sizeof(char *) * (size_t)(cap *= 2));
		L->l[L->n++] = strndup(p, len);
		if (!e) break;
		p = e + 1;
	}
}

static void lines_insert(lines_t *L, int at, char *s)
{
	L->l = realloc(L->l, sizeof(char *) * (size_t)(L->n + 1));
	memmove(L->l + at + 1, L->l + at, sizeof(char *) * (size_t)(L->n - at));
	L->l[at] = s;
	L->n++;
}

static char *lines_join(lines_t *L)
{
	size_t n = 1;
	for (int i = 0; i < L->n; i++) n += strlen(L->l[i]) + 2;
	char *o = malloc(n), *q = o;
	for (int i = 0; i < L->n; i++) {
		size_t k = strlen(L->l[i]);
		memcpy(q, L->l[i], k);
		q += k;
		if (i + 1 < L->n) { if (L->crlf) *q++ = '\r'; *q++ = '\n'; }
	}
	*q = 0;
	return o;
}

static void lines_free(lines_t *L)
{
	for (int i = 0; i < L->n; i++) free(L->l[i]);
	free(L->l);
}

/* a section header: its name in lower case */
static int is_header(const char *s, char *sec, size_t n)
{
	while (*s == ' ' || *s == '\t') s++;
	if (*s != '[') return 0;
	const char *e = strchr(s, ']');
	if (!e) return 0;
	size_t k = (size_t)(e - s - 1);
	if (k >= n) k = n - 1;
	for (size_t i = 0; i < k; i++) sec[i] = (char)tolower((unsigned char)s[1 + i]);
	sec[k] = 0;
	return 1;
}

/* "key=..." or ";key=...": the key in lower case, and whether the line is active */
static int key_of(const char *s, char *key, size_t n, int *active)
{
	while (*s == ' ' || *s == '\t') s++;
	*active = 1;
	if (*s == ';') {
		*active = 0;
		while (*s == ';') s++;
		while (*s == ' ' || *s == '\t') s++;
	}
	size_t k = 0;
	while (isalnum((unsigned char)s[k]) || s[k] == '_') k++;
	if (!k || k >= n) return 0;
	const char *p = s + k;
	while (*p == ' ' || *p == '\t') p++;
	if (*p != '=') return 0;
	for (size_t i = 0; i < k; i++) key[i] = (char)tolower((unsigned char)s[i]);
	key[k] = 0;
	return 1;
}

static int global(const char *sec) { return !*sec || !strcmp(sec, "mister"); }

/* the value of "key=value ; comment", and where the comment starts (with its spacing) */
static void value_of(const char *line, char *val, size_t n, const char **cmt)
{
	const char *v = strchr(line, '=') + 1;
	const char *c = strchr(v, ';');
	const char *end = c ? c : v + strlen(v);
	if (cmt) {
		const char *j = end;
		while (c && j > v && (j[-1] == ' ' || j[-1] == '\t')) j--;
		*cmt = c ? j : end;
	}
	while (v < end && (*v == ' ' || *v == '\t')) v++;
	while (end > v && (end[-1] == ' ' || end[-1] == '\t')) end--;
	size_t k = (size_t)(end - v);
	if (k >= n) k = n - 1;
	memcpy(val, v, k);
	val[k] = 0;
}

cJSON *vt_read(const char *text)
{
	cJSON *o = cJSON_CreateObject();
	for (int i = 0; KEYS[i]; i++) {
		cJSON *x = cJSON_AddObjectToObject(o, KEYS[i]);
		cJSON_AddStringToObject(x, "value", "");
		cJSON_AddBoolToObject(x, "set", 0);
		cJSON_AddBoolToObject(x, "commented", 0);
		cJSON_AddArrayToObject(x, "overrides");
	}
	lines_t L;
	lines_split(text, &L);
	char sec[64] = "", key[64], val[128];
	for (int i = 0; i < L.n; i++) {
		int act;
		if (is_header(L.l[i], sec, sizeof(sec))) continue;
		if (!key_of(L.l[i], key, sizeof(key), &act)) continue;
		cJSON *x = cJSON_GetObjectItem(o, key);
		if (!x) continue;
		if (!global(sec)) {
			if (act) cJSON_AddItemToArray(cJSON_GetObjectItem(x, "overrides"), cJSON_CreateString(sec));
			continue;
		}
		if (act) {
			value_of(L.l[i], val, sizeof(val), NULL);
			cJSON_ReplaceItemInObject(x, "value", cJSON_CreateString(val));
			cJSON_ReplaceItemInObject(x, "set", cJSON_CreateTrue());
			cJSON_ReplaceItemInObject(x, "commented", cJSON_CreateFalse());
		} else if (!cJSON_IsTrue(cJSON_GetObjectItem(x, "set"))) {
			cJSON_ReplaceItemInObject(x, "commented", cJSON_CreateTrue());
		}
	}
	lines_free(&L);
	return o;
}

/* key=value in [MiSTer] (value "" comments it out, so Main's own default applies). A commented-out
   line is reused, otherwise the line goes at the end of the section. Comments and line ends stay. */
char *vt_set(const char *text, const char *key_in, const char *value)
{
	lines_t L;
	lines_split(text, &L);
	char want[64], sec[64] = "", key[64], buf[512];
	snprintf(want, sizeof(want), "%s", key_in);
	for (char *p = want; *p; p++) *p = (char)tolower((unsigned char)*p);
	int done = 0, last_cmt = -1;
	for (int i = 0; i < L.n; i++) {
		int act;
		if (is_header(L.l[i], sec, sizeof(sec))) continue;
		if (!global(sec) || !key_of(L.l[i], key, sizeof(key), &act) || strcmp(key, want)) continue;
		if (!act) { last_cmt = i; continue; }
		if (!*value) {
			const char *s = L.l[i];
			while (*s == ' ' || *s == '\t') s++;
			snprintf(buf, sizeof(buf), ";%s", s);
		} else {
			const char *cmt;
			char v[128];
			value_of(L.l[i], v, sizeof(v), &cmt);
			const char *eq = strchr(L.l[i], '=');
			const char *s = L.l[i];
			while (*s == ' ' || *s == '\t') s++;
			int nl = (int)(eq - s);
			while (nl > 0 && (s[nl - 1] == ' ' || s[nl - 1] == '\t')) nl--;
			snprintf(buf, sizeof(buf), "%.*s=%s%s", nl, s, value, cmt);
		}
		free(L.l[i]);
		L.l[i] = strdup(buf);
		done = 1;
	}
	if (!done && *value) {
		if (last_cmt >= 0) {
			const char *cmt;
			char v[128];
			const char *s = L.l[last_cmt];
			value_of(s, v, sizeof(v), &cmt);
			while (*s == ' ' || *s == '\t' || *s == ';') s++;
			const char *eq = strchr(s, '=');
			int nl = (int)(eq - s);
			while (nl > 0 && (s[nl - 1] == ' ' || s[nl - 1] == '\t')) nl--;
			snprintf(buf, sizeof(buf), "%.*s=%s%s", nl, s, value, cmt);
			free(L.l[last_cmt]);
			L.l[last_cmt] = strdup(buf);
		} else {
			/* the end of the [MiSTer] section: after its last line that isn't blank */
			int at = -1, hdr = -1;
			sec[0] = 0;
			for (int i = 0; i < L.n; i++) {
				char s2[64];
				if (is_header(L.l[i], s2, sizeof(s2))) {
					snprintf(sec, sizeof(sec), "%s", s2);
					if (global(sec)) hdr = i;
					else if (hdr >= 0 || at >= 0) break;
					continue;
				}
				const char *t = L.l[i];
				while (*t == ' ' || *t == '\t') t++;
				if (global(sec) && *t) at = i;
			}
			snprintf(buf, sizeof(buf), "%s=%s", want, value);
			if (at >= 0) lines_insert(&L, at + 1, strdup(buf));
			else if (hdr >= 0) lines_insert(&L, hdr + 1, strdup(buf));
			else { lines_insert(&L, 0, strdup(buf)); lines_insert(&L, 0, strdup("[MiSTer]")); }
		}
	}
	char *out = lines_join(&L);
	lines_free(&L);
	return out;
}

/* ---------- profiles (MiSTer.ini and up to three MiSTer_*.ini, the way Console Mode's Main picks them) ----------
 * The SuperStation One ships MiSTer.ini plus four video profiles (RGHV, RGsB, SVID, YPbP). MiSTer's Main, and
 * Console Mode's (MiSTer_ConsoleMode: "@reboot expects an INI index 0-3"), offer the first three MiSTer_*.ini
 * they find; the others are listed, marked as not offered. */
#define VT_ALT_MAX 3

typedef struct {
	char name[64];
	int alt;            /* 0 MiSTer.ini, 1-4 an alternative, -1 one Main doesn't see (only the first four it finds) */
	char label[8];      /* as Main's menu shows it */
} prof_t;

static int name_ok(const char *n)
{
	size_t l = strlen(n);
	if (!strcmp(n, "MiSTer.ini")) return 1;
	return l > 11 && l < 64 && !strncasecmp(n, "MiSTer_", 7) && !strcasecmp(n + l - 4, ".ini") && !strchr(n, '/')
	       && !strpbrk(n, "'\"\\$`");                 /* (the name goes into a shell command) */
}

static int ci_cmp(const void *a, const void *b) { return strcasecmp((const char *)a, (const char *)b); }

/* cfg_get_name / cfg_get_label in MiSTer's Main: the first four MiSTer_*.ini in directory order, then sorted */
static int profiles(prof_t *p, int max)
{
	int n = 0;
	snprintf(p[n].name, sizeof(p[n].name), "MiSTer.ini");
	p[n].alt = 0;
	snprintf(p[n].label, sizeof(p[n].label), "Main");
	n++;
	char seen[VT_ALT_MAX][64], all[16][64];
	int ns = 0, na = 0;
	DIR *d = opendir(p_fat());
	struct dirent *e;
	while (d && (e = readdir(d))) {
		size_t l = strlen(e->d_name);
		if (l < 64 && !strncasecmp(e->d_name, "MiSTer_", 7) && l >= 4 && !strcasecmp(e->d_name + l - 4, ".ini")) {
			if (ns < VT_ALT_MAX) snprintf(seen[ns++], 64, "%s", e->d_name);
			if (na < 16) snprintf(all[na++], 64, "%s", e->d_name);
		}
	}
	if (d) closedir(d);
	qsort(seen, (size_t)ns, 64, ci_cmp);
	qsort(all, (size_t)na, 64, ci_cmp);
	for (int i = 0; i < na && n < max; i++) {
		prof_t *q = &p[n++];
		snprintf(q->name, sizeof(q->name), "%s", all[i]);
		q->alt = -1;
		for (int k = 0; k < ns; k++) if (!strcmp(seen[k], all[i])) q->alt = k + 1;
		char lb[8];
		snprintf(lb, sizeof(lb), "%.5s", all[i] + 7);
		char *dot = strrchr(lb, '.');
		if (dot) *dot = 0;
		if (!strcasecmp(lb, "alt") || !strcasecmp(lb, "alt_1")) snprintf(lb, sizeof(lb), "Alt1");
		else if (!strcasecmp(lb, "alt_2")) snprintf(lb, sizeof(lb), "Alt2");
		else if (!strcasecmp(lb, "alt_3")) snprintf(lb, sizeof(lb), "Alt3");
		else lb[4] = 0;
		snprintf(q->label, sizeof(q->label), "%s", lb);
	}
	return n;
}

static const char *alt_file(int alt)
{
	static prof_t p[17];
	int n = profiles(p, 17);
	for (int i = 0; i < n; i++) if (p[i].alt == alt) return p[i].name;
	return NULL;
}

/* the profile Main uses: kept in shared memory (altcfg() in Main's user_io.cpp). -1: can't be read */
#define ALT_BASE 0x1FFFF000
#define ALT_OFF 0xF04
int vt_altcfg(int set)
{
	const char *t = getenv("CMOD_ALTCFG");                /* tests: a file holding the number */
	if (t && *t) {
		if (set >= 0) { FILE *f = fopen(t, "w"); if (!f) return -1; fprintf(f, "%d\n", set); fclose(f); return set; }
		FILE *f = fopen(t, "r");
		int v = 0;
		if (f) { if (fscanf(f, "%d", &v) != 1) v = 0; fclose(f); }
		return v;
	}
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) return -1;
	void *m = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, ALT_BASE);
	close(fd);
	if (m == MAP_FAILED) return -1;
	volatile unsigned char *b = (volatile unsigned char *)m + ALT_OFF;
	int r = 0;
	if (set >= 0) { b[0] = 0x34; b[1] = 0x99; b[2] = 0xBA; b[3] = (unsigned char)set; r = set; }
	else if (b[0] == 0x34 && b[1] == 0x99 && b[2] == 0xBA) r = b[3] <= VT_ALT_MAX ? b[3] : 0;
	munmap(m, 0x1000);
	return r;
}

/* S-Video / composite made by the FPGA: the video settings Main uses now (MiSTer.ini or the profile chosen)
   have vga_mode=svideo or cvbs (the SS1's SVID profile, DIP 3 up). Only then is a Y/C build of a core any
   use: on HDMI, RGB, component or the Sony encoder (subcarrier, as shipped) it isn't. */
int vt_fpga_yc(void)
{
	int a = vt_altcfg(-1);
	const char *name = a > 0 ? alt_file(a) : NULL;
	char path[700];
	snprintf(path, sizeof(path), "%s/%s", p_fat(), name ? name : "MiSTer.ini");
	char *text = slurp(path);
	if (!text) return 0;
	cJSON *v = vt_read(text);
	free(text);
	const char *m = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(v, "vga_mode"), "value"));
	int yc = m && (!strcasecmp(m, "svideo") || !strcasecmp(m, "cvbs"));
	cJSON_Delete(v);
	return yc;
}

/* ---------- trying ---------- */

/* a shell command run on its own (not cmremote's child: it outlives cmremote) */
static int spawn(const char *sh)
{
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		pid_t q = fork();
		if (q == 0) {
			int fd = open("/dev/null", O_RDWR);
			if (fd >= 0) { dup2(fd, 0); dup2(fd, 1); dup2(fd, 2); if (fd > 2) close(fd); }
			execl("/bin/sh", "sh", "-c", sh, (char *)NULL);
			_exit(127);
		}
		_exit(q > 0 ? 0 : 1);
	}
	int st = 1;
	if (pid > 0) waitpid(pid, &st, 0);
	return pid > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

/* the shell line that restarts Main (a FIFO: only when Main reads it, never waiting on it) */
static void restart_line(char *out, size_t n)
{
	const char *c = p_mister_cmd();
	snprintf(out, n, "if [ -p '%s' ]; then timeout 3 sh -c \"echo 'load_core menu.rbf' > '%s'\"; "
	                 "elif [ -n \"$CMOD_MISTER_CMD\" ]; then echo 'load_core menu.rbf' >> '%s'; fi", c, c, c);
}

static void self_exe(char *out, size_t n)
{
	ssize_t k = readlink("/proc/self/exe", out, n - 1);
	if (k <= 0) snprintf(out, n, "%s/cmremote", p_data());
	else out[k] = 0;
}

/* the try in progress, on the SD card (so it's put back after a power cut too):
   token=, kind=file|profile, file= (the ini changed), backup= (its copy from before), prev= (the
   profile before a profile try), deadline=, changes= (JSON) */
typedef struct {
	char token[40], kind[12], file[600], backup[700], changes[2048];
	int prev;
	long deadline;
} marker_t;

static int marker_read(marker_t *m)
{
	char p[600];
	marker_path(p, sizeof(p));
	memset(m, 0, sizeof(*m));
	m->prev = -1;
	FILE *f = fopen(p, "r");
	if (!f) return -1;
	char line[2200];
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		char *eq = strchr(line, '=');
		if (!eq) continue;
		*eq++ = 0;
		if (!strcmp(line, "token")) snprintf(m->token, sizeof(m->token), "%s", eq);
		else if (!strcmp(line, "kind")) snprintf(m->kind, sizeof(m->kind), "%s", eq);
		else if (!strcmp(line, "file")) snprintf(m->file, sizeof(m->file), "%s", eq);
		else if (!strcmp(line, "backup")) snprintf(m->backup, sizeof(m->backup), "%s", eq);
		else if (!strcmp(line, "prev")) m->prev = atoi(eq);
		else if (!strcmp(line, "deadline")) m->deadline = atol(eq);
		else if (!strcmp(line, "changes")) snprintf(m->changes, sizeof(m->changes), "%s", eq);
	}
	fclose(f);
	if (!m->token[0]) return -1;
	if (!strcmp(m->kind, "profile")) return m->prev >= 0 ? 0 : -1;
	return m->file[0] && m->backup[0] ? 0 : -1;
}

static void marker_drop(void)
{
	char p[600];
	marker_path(p, sizeof(p));
	unlink(p);
	sync();
}

static int marker_write(const marker_t *m)
{
	char p[600], t[4000];
	marker_path(p, sizeof(p));
	mkdirs(p_data());
	snprintf(t, sizeof(t), "token=%s\nkind=%s\nfile=%s\nbackup=%s\nprev=%d\ndeadline=%ld\nchanges=%s\n",
	         m->token, m->kind, m->file, m->backup, m->prev, m->deadline, m->changes);
	return write_atomic(p, t, strlen(t));
}

/* Keep never came: the old file (or profile) back now, and Main restarted */
static int put_back(const marker_t *m, const char *why)
{
	char sh[1400], rl[700];
	int r = 0;
	if (!strcmp(m->kind, "profile")) {
		r = vt_altcfg(m->prev) < 0 ? -1 : 0;
		plog("video: %s: back to profile %d", why, m->prev);
	} else {
		char *old = slurp(m->backup);
		if (!old) { plog("video: %s: the kept copy %s is missing, nothing put back", why, m->backup); marker_drop(); return -1; }
		r = write_atomic(m->file, old, strlen(old));
		free(old);
		plog("video: %s: %s put back from %s", why, m->file, m->backup);
	}
	marker_drop();
	restart_line(rl, sizeof(rl));
	/* at start-up Main may not be reading its pipe yet: wait for it up to 30 s */
	snprintf(sh, sizeof(sh), "i=0; while [ $i -lt 30 ]; do if [ -p '%s' ] || [ -n \"$CMOD_MISTER_CMD\" ]; then %s; break; fi; i=$((i+1)); sleep 1; done", p_mister_cmd(), rl);
	spawn(sh);
	return r;
}

void vt_boot(void)
{
	marker_t m;
	if (marker_read(&m) == 0) put_back(&m, "a try was never kept (power or cmremote went)");
}

static int game_running(void)
{
	FILE *f = fopen(p_corename(), "r");
	if (!f) return 0;
	char c[128] = "";
	if (!fgets(c, sizeof(c), f)) c[0] = 0;
	fclose(f);
	c[strcspn(c, "\r\n")] = 0;
	return c[0] && strcasecmp(c, "MENU");
}

static void pending_json(cJSON *o)
{
	marker_t m;
	if (marker_read(&m)) { cJSON_AddNullToObject(o, "pending"); return; }
	cJSON *p = cJSON_AddObjectToObject(o, "pending");
	long left = m.deadline - (long)time(NULL);
	cJSON_AddNumberToObject(p, "secs_left", left > 0 ? left : 0);
	cJSON_AddStringToObject(p, "kind", m.kind);
	if (!strcmp(m.kind, "file")) { const char *b = strrchr(m.file, '/'); cJSON_AddStringToObject(p, "file", b ? b + 1 : m.file); }
	else cJSON_AddNumberToObject(p, "from_profile", m.prev);
	cJSON *ch = cJSON_Parse(m.changes);
	cJSON_AddItemToObject(p, "changes", ch ? ch : cJSON_CreateObject());
}

/* the timer: unless Keep took the marker away (or another try replaced it), the old file or profile goes back */
static int start_timer(const marker_t *m, int secs)
{
	char sh[2600], rl[700], mk[600], self[400], undo[1500];
	marker_path(mk, sizeof(mk));
	restart_line(rl, sizeof(rl));
	if (!strcmp(m->kind, "profile")) {
		self_exe(self, sizeof(self));
		snprintf(undo, sizeof(undo), "'%s' --altcfg %d", self, m->prev);
	} else {
		snprintf(undo, sizeof(undo), "cp -f '%s' '%s.cm-new' && mv -f '%s.cm-new' '%s'", m->backup, m->file, m->file, m->file);
	}
	snprintf(sh, sizeof(sh),
	         "sleep %d; m='%s'; if [ -f \"$m\" ] && grep -qx 'token=%s' \"$m\"; then %s; rm -f \"$m\"; sync; "
	         "echo \"$(date '+%%Y-%%m-%%d %%H:%%M:%%S') video: no Keep in %d s: put back\" >> '%s/cmremote-video.log'; %s; fi",
	         secs, mk, m->token, undo, secs, p_tmp(), rl);
	return spawn(sh);
}

static void new_token(marker_t *m, int secs, cJSON *changes)
{
	snprintf(m->token, sizeof(m->token), "%ld-%d", (long)time(NULL), (int)getpid() ^ rand());
	m->deadline = (long)time(NULL) + secs;
	char *cj = cJSON_PrintUnformatted(changes);
	snprintf(m->changes, sizeof(m->changes), "%s", cj ? cj : "{}");
	free(cj);
}

/* text becomes the new <file>: the old one kept first, then the timer, then Main restarted */
static int try_text(const char *file, const char *text, cJSON *changes, int secs, char *err, size_t errn)
{
	char kept[200] = "";
	if (ex_ini_keep(file, kept, sizeof(kept)) || !kept[0]) { snprintf(err, errn, "Couldn't keep a copy of %s, so nothing was changed", file); return -1; }
	marker_t m = { .prev = -1 };
	snprintf(m.kind, sizeof(m.kind), "file");
	snprintf(m.file, sizeof(m.file), "%s/%s", p_fat(), file);
	bk_path(m.backup, sizeof(m.backup), kept);
	new_token(&m, secs, changes);
	if (marker_write(&m)) { snprintf(err, errn, "Couldn't start the timer, so nothing was changed"); return -1; }
	if (write_atomic(m.file, text, strlen(text))) { marker_drop(); snprintf(err, errn, "Couldn't write %s (it's untouched)", file); return -1; }
	if (start_timer(&m, secs)) { put_back(&m, "the timer didn't start"); snprintf(err, errn, "Couldn't start the timer: nothing was changed"); return -1; }
	plog("video: trying a change to %s for %d s (the old file kept as %s)", file, secs, kept);
	char e2[200];
	if (mister_cmd("load_core menu.rbf\n", e2, sizeof(e2))) plog("video: restart: %s", e2);
	return 0;
}

static int try_profile(int to, int secs, char *err, size_t errn)
{
	int cur = vt_altcfg(-1);
	if (cur < 0) { snprintf(err, errn, "Couldn't read which profile Main uses"); return -1; }
	if (cur == to) { snprintf(err, errn, "That profile is already in use"); return -1; }
	marker_t m = { .prev = cur };
	snprintf(m.kind, sizeof(m.kind), "profile");
	cJSON *ch = cJSON_CreateObject();
	cJSON_AddNumberToObject(ch, "profile", to);
	new_token(&m, secs, ch);
	cJSON_Delete(ch);
	if (marker_write(&m)) { snprintf(err, errn, "Couldn't start the timer, so nothing was changed"); return -1; }
	if (start_timer(&m, secs)) { marker_drop(); snprintf(err, errn, "Couldn't start the timer: nothing was changed"); return -1; }
	if (vt_altcfg(to) < 0) { put_back(&m, "the profile couldn't be set"); snprintf(err, errn, "Couldn't switch the profile"); return -1; }
	plog("video: trying profile %d (%s) for %d s (was %d)", to, alt_file(to), secs, cur);
	char e2[200];
	if (mister_cmd("load_core menu.rbf\n", e2, sizeof(e2))) plog("video: restart: %s", e2);
	return 0;
}

/* the newest kept copy of <file> that differs from it now (tries that went back leave copies the same as it) */
static char *undo_text(const char *file, const char *cur)
{
	char dir[600], pre[80];
	snprintf(dir, sizeof(dir), "%s/backups/ini", p_data());
	snprintf(pre, sizeof(pre), "%s.", file);
	size_t pl = strlen(pre);
	struct dirent **nl = NULL;
	int m = scandir(dir, &nl, NULL, alphasort);
	char *best = NULL;
	for (int i = m - 1; i >= 0; i--) {
		const char *n = nl[i]->d_name;
		if (!best && !strncmp(n, pre, pl) && isdigit((unsigned char)n[pl]) && !strstr(n, ".cm-new")) {
			char p[900];
			snprintf(p, sizeof(p), "%s/%s", dir, n);
			char *t = slurp(p);
			if (t && strcmp(t, cur)) best = t;
			else free(t);
		}
		free(nl[i]);
	}
	free(nl);
	return best;
}

cJSON *vt_cmd(cJSON *req, const char **err)
{
	static char e[260];
	*err = NULL;
	prof_t pr[17];
	int np = profiles(pr, 17);
	int act = vt_altcfg(-1);
	const char *af = act >= 0 ? alt_file(act) : NULL;
	if (!af) af = "MiSTer.ini";                         /* Main falls back to MiSTer.ini too */
	const char *file = cJSON_GetStringValue(cJSON_GetObjectItem(req, "file"));
	if (!file) file = af;
	if (!name_ok(file)) { *err = "file: MiSTer.ini or MiSTer_<name>.ini"; return NULL; }
	char path[600];
	snprintf(path, sizeof(path), "%s/%s", p_fat(), file);
	int secs = VT_SECS;
	cJSON *s = cJSON_GetObjectItem(req, "secs");
	if (cJSON_IsNumber(s)) secs = s->valueint < 10 ? 10 : s->valueint > 120 ? 120 : s->valueint;
	cJSON *tr = cJSON_GetObjectItem(req, "try"), *prof = cJSON_GetObjectItem(req, "profile");
	marker_t m;
	int waiting = marker_read(&m) == 0;
	char e2[220];

	if (cJSON_IsTrue(cJSON_GetObjectItem(req, "keep"))) {
		if (!waiting) { *err = "Nothing is waiting for Keep (it may have gone back already)"; return NULL; }
		marker_drop();
		plog("video: kept");
	} else if (cJSON_IsTrue(cJSON_GetObjectItem(req, "revert"))) {
		if (!waiting) { *err = "Nothing is waiting for Keep"; return NULL; }
		put_back(&m, "put back from the app or menu");
	} else if (cJSON_IsObject(tr) || cJSON_IsTrue(cJSON_GetObjectItem(req, "undo")) || cJSON_IsNumber(prof)) {
		if (waiting) { *err = "A change is still waiting for Keep: keep it or put it back first"; return NULL; }
		if (game_running()) { *err = "A game is running: close it first (trying restarts the menu)"; return NULL; }
		if (cJSON_IsNumber(prof)) {
			int to = prof->valueint;
			if (to < 0 || to > VT_ALT_MAX || !alt_file(to)) { *err = "profile: 0 (MiSTer.ini) or one of the alternatives Main sees (1-4)"; return NULL; }
			if (try_profile(to, secs, e2, sizeof(e2))) { snprintf(e, sizeof(e), "%s", e2); *err = e; return NULL; }
		} else {
			char *text = slurp(path);
			if (!text) { snprintf(e, sizeof(e), "%s isn't there", file); *err = e; return NULL; }
			cJSON *changes = cJSON_CreateObject();
			if (cJSON_IsObject(tr)) {
				cJSON *it;
				cJSON_ArrayForEach(it, tr) {
					const char *v = cJSON_IsString(it) ? it->valuestring : cJSON_IsNull(it) ? "" : NULL;
					if (!key_ok(it->string) || !v || !value_ok(v)) {
						snprintf(e, sizeof(e), "%s: not a video setting or not a value MiSTer takes", it->string);
						free(text); cJSON_Delete(changes); *err = e; return NULL;
					}
					char *n2 = vt_set(text, it->string, v);
					free(text);
					text = n2;
					cJSON_AddStringToObject(changes, it->string, v);
				}
			} else {
				char *old = undo_text(file, text);
				free(text);
				if (!old) { cJSON_Delete(changes); snprintf(e, sizeof(e), "There's no earlier copy of %s to go back to", file); *err = e; return NULL; }
				text = old;
				cJSON_AddBoolToObject(changes, "undo", 1);
			}
			int r = try_text(file, text, changes, secs, e2, sizeof(e2));
			free(text);
			cJSON_Delete(changes);
			if (r) { snprintf(e, sizeof(e), "%s", e2); *err = e; return NULL; }
		}
	}
	act = vt_altcfg(-1);
	cJSON *o = cJSON_CreateObject();
	cJSON *fa = cJSON_AddArrayToObject(o, "files");
	for (int i = 0; i < np; i++) {
		cJSON *x = cJSON_CreateObject();
		cJSON_AddStringToObject(x, "name", pr[i].name);
		cJSON_AddNumberToObject(x, "profile", pr[i].alt);
		cJSON_AddStringToObject(x, "label", pr[i].label);
		cJSON_AddBoolToObject(x, "active", pr[i].alt >= 0 && pr[i].alt == act);
		cJSON_AddItemToArray(fa, x);
	}
	if (act >= 0) cJSON_AddNumberToObject(o, "active", act);
	else cJSON_AddNullToObject(o, "active");
	cJSON_AddStringToObject(o, "file", file);
	char *text = slurp(path);
	cJSON_AddBoolToObject(o, "exists", text != NULL);
	cJSON_AddItemToObject(o, "settings", vt_read(text ? text : ""));
	free(text);
	pending_json(o);
	cJSON_AddNumberToObject(o, "secs", VT_SECS);
	cJSON_AddBoolToObject(o, "game_running", game_running());
	return o;
}
