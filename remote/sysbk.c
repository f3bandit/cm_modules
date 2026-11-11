/* cmremote: MiSTer & Linux backups for the apps: the system
 * files (Main, the menu core, Linux) and the settings files, read in pieces so the Windows app and the
 * phone can keep every version, and written back in pieces when one is put back.
 * Copyright (C) 2026 f3bandit. All rights reserved; licensed to Retro Remake (see ../LICENSE.md).
 *
 * Only the files below. Passwords, logins, pairings and keys stay here: WiFi (wpa_supplicant), SSH keys
 * (authorized_keys), the app link's pairings (remote.ini) and box art logins (screenscraper.txt,
 * tgdb_apikey.txt) are never read or written through this.
 *
 *   {cmd:"sysbk"}                               {linux, busy, items:[{id, name, path, kind, exists, size, mtime, hash}]}
 *                                               hash: SHA-256, null while a big file is still being read
 *   {cmd:"sysbk", get:id, offset}               {size, offset, data (base64, up to 512 KB), hash}
 *   {cmd:"sysbk", put:id, offset, size, hash, data}   pieces in order; the last one checks the hash, keeps
 *                                               the file as it is now in backups/linux-settings/sysbk, then puts
 *                                               the new one in place: {done, restart} */
#include "sysbk.h"
#include "compat.h"
#include "sha256.h"
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PIECE (512 * 1024)
#define MAX_ITEMS 40

typedef struct {
	char id[64], name[96], path[600];
	int system;              /* a system file: the MiSTer restarts to use one put back */
} item_t;

static const char *const SETTINGS[][2] = {      /* path (FAT-relative when it doesn't start with /), name */
	{ "MiSTer.ini", "MiSTer.ini (video, input and more)" },
	{ "downloader.ini", "downloader.ini (Update All)" },
	{ "linux/u-boot.txt", "u-boot.txt (start-up options)" },
	{ "linux/user-startup.sh", "user-startup.sh (what starts with Linux)" },
	{ "ConsoleMode/config.ini", "Console Mode's settings (config.ini)" },
	{ "ConsoleMode/gamelist.ini", "Console Mode's favourites (gamelist.ini)" },
	{ "ConsoleMode/tools/update-guard.ini", "Restore points' settings (update-guard.ini)" },
	{ "ConsoleMode/tools/backup-agent.ini", "Save snapshots' settings (backup-agent.ini)" },
	{ "/etc/dhcpcd.conf", "Fixed network address (dhcpcd.conf)" },
	{ "/etc/hostname", "Name on the network (hostname)" },
	{ "/etc/hosts", "Known names (hosts)" },
	{ "/etc/ssh/sshd_config", "SSH settings (sshd_config)" },
};
static const char *const SYSTEM[][2] = {
	{ "MiSTer", "MiSTer's Main" },
	{ "ConsoleMode/MiSTer_ConsoleMode", "Console Mode's Main" },
	{ "ConsoleMode/menu_ConsoleMode.rbf", "Console Mode's menu core" },
	{ "menu.rbf", "Menu core" },
	{ "linux/zImage_dtb", "Linux kernel" },
	{ "linux/linux.img", "Linux" },
};

static item_t items[MAX_ITEMS];
static int nitems;

static void add(const char *rel, const char *name, int system)
{
	if (nitems >= MAX_ITEMS) return;
	item_t *it = &items[nitems++];
	const char *root = getenv("CMOD_SYSROOT");         /* tests: a stand-in for / */
	if (rel[0] == '/') snprintf(it->path, sizeof(it->path), "%s%s", root && *root ? root : "", rel);
	else snprintf(it->path, sizeof(it->path), "%s/%s", p_fat(), rel);
	snprintf(it->id, sizeof(it->id), "%s", rel[0] == '/' ? rel + 1 : rel);
	for (char *c = it->id; *c; c++) if (*c == '/') *c = '+';     /* '+': allowed in file names on FAT and Windows (':' isn't) */
	snprintf(it->name, sizeof(it->name), "%s", name);
	it->system = system;
}

static int ci(const void *a, const void *b) { return strcasecmp((const char *)a, (const char *)b); }

/* the list, made again each time (the MiSTer_*.ini profiles come and go) */
static void build(void)
{
	nitems = 0;
	add(SETTINGS[0][0], SETTINGS[0][1], 0);
	char prof[16][64];
	int np = 0;
	DIR *d = opendir(p_fat());
	struct dirent *e;
	while (d && (e = readdir(d)) && np < 16) {
		size_t l = strlen(e->d_name);
		if (l > 11 && l < 64 && !strncasecmp(e->d_name, "MiSTer_", 7) && !strcasecmp(e->d_name + l - 4, ".ini")) snprintf(prof[np++], 64, "%s", e->d_name);
	}
	if (d) closedir(d);
	qsort(prof, (size_t)np, 64, ci);
	for (int i = 0; i < np; i++) { char n[96]; snprintf(n, sizeof(n), "%s (profile)", prof[i]); add(prof[i], n, 0); }
	for (size_t i = 1; i < sizeof(SETTINGS) / sizeof(SETTINGS[0]); i++) add(SETTINGS[i][0], SETTINGS[i][1], 0);
	for (size_t i = 0; i < sizeof(SYSTEM) / sizeof(SYSTEM[0]); i++) add(SYSTEM[i][0], SYSTEM[i][1], 1);
}

static item_t *find(const char *id)
{
	build();
	for (int i = 0; i < nitems; i++) if (!strcmp(items[i].id, id)) return &items[i];
	return NULL;
}

/* ---------- hashes, kept while size and time stay the same; big files are read in the background ---------- */

typedef struct { char path[600]; long long size; long mtime; char hash[65]; } hcache_t;
static hcache_t hc[MAX_ITEMS];
static int nhc;
static pthread_mutex_t hmu = PTHREAD_MUTEX_INITIALIZER;
static int hashing;

static int cached(const char *path, const struct stat *st, char out[65])
{
	for (int i = 0; i < nhc; i++)
		if (!strcmp(hc[i].path, path) && hc[i].size == (long long)st->st_size && hc[i].mtime == (long)st->st_mtime) { snprintf(out, 65, "%s", hc[i].hash); return 1; }
	return 0;
}

static void remember(const char *path, const struct stat *st, const char *hash)
{
	int k = -1;
	for (int i = 0; i < nhc; i++) if (!strcmp(hc[i].path, path)) k = i;
	if (k < 0) { if (nhc >= MAX_ITEMS) return; k = nhc++; }
	snprintf(hc[k].path, sizeof(hc[k].path), "%s", path);
	hc[k].size = (long long)st->st_size;
	hc[k].mtime = (long)st->st_mtime;
	snprintf(hc[k].hash, sizeof(hc[k].hash), "%s", hash);
}

static void *hash_all(void *u)
{
	(void)u;
	for (;;) {
		char path[600] = "";
		struct stat st;
		pthread_mutex_lock(&hmu);
		build();
		for (int i = 0; i < nitems && !path[0]; i++) {
			char h[65];
			if (stat(items[i].path, &st) == 0 && !cached(items[i].path, &st, h)) snprintf(path, sizeof(path), "%s", items[i].path);
		}
		if (!path[0]) { hashing = 0; pthread_mutex_unlock(&hmu); return NULL; }
		pthread_mutex_unlock(&hmu);
		char h[65];
		if (sha256_file(path, h) == 0) {
			struct stat st2;
			pthread_mutex_lock(&hmu);
			if (stat(path, &st2) == 0 && st2.st_size == st.st_size && st2.st_mtime == st.st_mtime) remember(path, &st2, h);
			pthread_mutex_unlock(&hmu);
		} else {
			pthread_mutex_lock(&hmu);
			remember(path, &st, "");               /* unreadable: don't try again until it changes */
			pthread_mutex_unlock(&hmu);
		}
	}
}

/* ---------- base64 ---------- */

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static char *b64(const unsigned char *d, size_t n)
{
	char *o = malloc((n + 2) / 3 * 4 + 1), *p = o;
	if (!o) return NULL;
	for (size_t i = 0; i < n; i += 3) {
		unsigned v = d[i] << 16 | (i + 1 < n ? d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
		*p++ = B64[v >> 18 & 63];
		*p++ = B64[v >> 12 & 63];
		*p++ = i + 1 < n ? B64[v >> 6 & 63] : '=';
		*p++ = i + 2 < n ? B64[v & 63] : '=';
	}
	*p = 0;
	return o;
}
static unsigned char *unb64(const char *s, size_t *n)
{
	static signed char T[256];
	if (!T['B']) { memset(T, -1, sizeof(T)); for (int i = 0; i < 64; i++) T[(unsigned char)B64[i]] = (signed char)i; }
	unsigned char *o = malloc(strlen(s) / 4 * 3 + 3);
	if (!o) return NULL;
	unsigned v = 0;
	int bits = 0;
	*n = 0;
	for (; *s; s++) {
		int k = T[(unsigned char)*s];
		if (k < 0) continue;
		v = v << 6 | (unsigned)k;
		bits += 6;
		if (bits >= 8) { bits -= 8; o[(*n)++] = (unsigned char)(v >> bits & 255); }
	}
	return o;
}

/* ---------- the command ---------- */

static void linux_version(char *out, size_t n)
{
	*out = 0;
	char p[300];
	const char *root = getenv("CMOD_SYSROOT");          /* tests */
	snprintf(p, sizeof(p), "%s/MiSTer.version", root && *root ? root : "");
	FILE *f = fopen(p, "r");
	if (!f) return;
	if (fgets(out, (int)n, f)) out[strcspn(out, "\r\n ")] = 0;
	fclose(f);
}

static int copy_file(const char *from, const char *to)
{
	FILE *a = fopen(from, "rb");
	if (!a) return -1;
	char tmp[700];
	snprintf(tmp, sizeof(tmp), "%s.phos-new", to);
	FILE *b = fopen(tmp, "wb");
	if (!b) { fclose(a); return -1; }
	static char buf[1 << 16];
	size_t r;
	int bad = 0;
	while ((r = fread(buf, 1, sizeof(buf), a)) > 0) if (fwrite(buf, 1, r, b) != r) { bad = 1; break; }
	bad |= ferror(a);
	fclose(a);
	bad |= fflush(b) != 0 || fsync(fileno(b)) != 0;
	bad |= fclose(b) != 0;
	if (bad || rename(tmp, to)) { unlink(tmp); return -1; }
	return 0;
}

cJSON *sysbk_cmd(cJSON *req, const char **err)
{
	static char e[200];
	*err = NULL;
	const char *get = cJSON_GetStringValue(cJSON_GetObjectItem(req, "get"));
	const char *put = cJSON_GetStringValue(cJSON_GetObjectItem(req, "put"));
	cJSON *offj = cJSON_GetObjectItem(req, "offset");
	long long off = cJSON_IsNumber(offj) ? (long long)offj->valuedouble : 0;
	if (off < 0) off = 0;

	if (get) {
		pthread_mutex_lock(&hmu);
		item_t *it = find(get);
		item_t c = it ? *it : (item_t){ 0 };
		pthread_mutex_unlock(&hmu);
		if (!it) { *err = "Not one of the files kept here"; return NULL; }
		FILE *f = fopen(c.path, "rb");
		if (!f) { *err = "That file isn't there"; return NULL; }
		struct stat st;
		fstat(fileno(f), &st);
		unsigned char *buf = malloc(PIECE);
		if (!buf || fseeko(f, (off_t)off, SEEK_SET)) { free(buf); fclose(f); *err = "Couldn't read it"; return NULL; }
		size_t r = fread(buf, 1, PIECE, f);
		fclose(f);
		char *d = b64(buf, r);
		free(buf);
		cJSON *o = cJSON_CreateObject();
		cJSON_AddNumberToObject(o, "size", (double)st.st_size);
		cJSON_AddNumberToObject(o, "offset", (double)off);
		cJSON_AddNumberToObject(o, "length", (double)r);
		cJSON_AddStringToObject(o, "data", d ? d : "");
		free(d);
		char h[65];
		pthread_mutex_lock(&hmu);
		if (cached(c.path, &st, h) && h[0]) cJSON_AddStringToObject(o, "hash", h);
		else if (st.st_size <= 4 * 1024 * 1024 && sha256_file(c.path, h) == 0) { remember(c.path, &st, h); cJSON_AddStringToObject(o, "hash", h); }
		else cJSON_AddNullToObject(o, "hash");
		pthread_mutex_unlock(&hmu);
		return o;
	}

	if (put) {
		pthread_mutex_lock(&hmu);
		item_t *it = find(put);
		item_t c = it ? *it : (item_t){ 0 };
		pthread_mutex_unlock(&hmu);
		if (!it) { *err = "Not one of the files kept here"; return NULL; }
		const char *data = cJSON_GetStringValue(cJSON_GetObjectItem(req, "data"));
		const char *hash = cJSON_GetStringValue(cJSON_GetObjectItem(req, "hash"));
		cJSON *sz = cJSON_GetObjectItem(req, "size");
		if (!data || !hash || strlen(hash) != 64 || !cJSON_IsNumber(sz)) { *err = "put: data, size and hash"; return NULL; }
		long long size = (long long)sz->valuedouble;
		char part[700];
		snprintf(part, sizeof(part), "%s.phos-restore", c.path);
		size_t n = 0;
		unsigned char *bytes = unb64(data, &n);
		if (!bytes) { *err = "Out of memory"; return NULL; }
		FILE *f = fopen(part, off == 0 ? "wb" : "r+b");
		struct stat pst;
		if (f && off > 0 && (fstat(fileno(f), &pst) || pst.st_size != off)) { fclose(f); f = NULL; }   /* pieces in order only */
		if (!f || fseeko(f, (off_t)off, SEEK_SET) || fwrite(bytes, 1, n, f) != n || fclose(f)) {
			free(bytes);
			unlink(part);
			*err = "Couldn't write it (the file on the MiSTer is untouched)";
			return NULL;
		}
		free(bytes);
		cJSON *o = cJSON_CreateObject();
		if (off + (long long)n < size) { cJSON_AddBoolToObject(o, "done", 0); cJSON_AddNumberToObject(o, "offset", (double)(off + (long long)n)); return o; }
		char got[65];
		if (sha256_file(part, got) || strcmp(got, hash)) {
			unlink(part);
			cJSON_Delete(o);
			*err = "What arrived doesn't match the backup (the file on the MiSTer is untouched)";
			return NULL;
		}
		/* the file as it is now, kept first */
		char dir[600], keep[800], stamp[32];
		snprintf(dir, sizeof(dir), "%s/backups/linux-settings/sysbk", p_data());
		mkdirs(dir);
		time_t t = time(NULL);
		strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", localtime(&t));
		snprintf(keep, sizeof(keep), "%s/%s.%s", dir, c.id, stamp);
		struct stat now;
		if (stat(c.path, &now) == 0 && copy_file(c.path, keep)) {
			unlink(part);
			cJSON_Delete(o);
			snprintf(e, sizeof(e), "Couldn't keep a copy of %s first, so nothing was changed", c.name);
			*err = e;
			return NULL;
		}
		if (rename(part, c.path)) { unlink(part); cJSON_Delete(o); *err = "Couldn't put it in place (the old one is untouched)"; return NULL; }
		sync();
		plog("sysbk: %s put back from a backup (the one before kept as %s)", c.path, keep);
		cJSON_AddBoolToObject(o, "done", 1);
		cJSON_AddBoolToObject(o, "restart", c.system);
		return o;
	}

	/* the list */
	cJSON *o = cJSON_CreateObject();
	char lv[64];
	linux_version(lv, sizeof(lv));
	cJSON_AddStringToObject(o, "linux", lv);
	cJSON *arr = cJSON_AddArrayToObject(o, "items");
	int start = 0;
	pthread_mutex_lock(&hmu);
	build();
	for (int i = 0; i < nitems; i++) {
		item_t *it = &items[i];
		struct stat st;
		int ex = stat(it->path, &st) == 0 && S_ISREG(st.st_mode);
		cJSON *x = cJSON_CreateObject();
		cJSON_AddStringToObject(x, "id", it->id);
		cJSON_AddStringToObject(x, "name", it->name);
		cJSON_AddStringToObject(x, "path", it->path);
		cJSON_AddStringToObject(x, "kind", it->system ? "system" : "settings");
		cJSON_AddBoolToObject(x, "exists", ex);
		if (ex) {
			cJSON_AddNumberToObject(x, "size", (double)st.st_size);
			cJSON_AddNumberToObject(x, "mtime", (double)st.st_mtime);
			char h[65];
			if (cached(it->path, &st, h) && h[0]) cJSON_AddStringToObject(x, "hash", h);
			else if (st.st_size <= 4 * 1024 * 1024 && sha256_file(it->path, h) == 0) { remember(it->path, &st, h); cJSON_AddStringToObject(x, "hash", h); }
			else { cJSON_AddNullToObject(x, "hash"); start = 1; }
		}
		cJSON_AddItemToArray(arr, x);
	}
	if (start && !hashing) {
		pthread_t th;
		hashing = 1;
		if (pthread_create(&th, NULL, hash_all, NULL) == 0) pthread_detach(th);
		else hashing = 0;
	}
	cJSON_AddBoolToObject(o, "busy", hashing);
	pthread_mutex_unlock(&hmu);
	return o;
}
