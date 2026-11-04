/* cmmod wifi: WiFi for a front end and the apps (scan, connect, forget, order, undo) on MiSTer's own WiFi
 * setup, the same wpa_supplicant.conf Console Mode's WiFi menu uses.
 * Copyright (C) 2026 f3bandit. PolyForm Noncommercial 1.0.0 (see LICENSE.md).
 *
 * MiSTer runs wpa_supplicant with /media/fat/linux/wpa_supplicant.conf and dhcpcd for the address,
 * without a control socket, so this works the same way MiSTer's own WiFi script does: networks are
 * written to that file (a backup is made first, every time; "undo" puts the last one back) and
 * wpa_supplicant is told to read it again (SIGHUP), or started the way MiSTer starts it if it isn't
 * running. dhcpcd picks up the new connection by itself. Passwords are stored as WPA keys (hashed
 * by wpa_passphrase), never as plain text, and never sent back.
 *
 * Scanning uses iw and takes a few seconds, so it runs in the background.
 *   CMOD_WPA_CONF, CMOD_WPA_PID, CMOD_IW, CMOD_WIFI_IF: paths and tools, for tests */
#define _GNU_SOURCE
#include "cmmod.h"
#include <errno.h>
#include <stdarg.h>
#include <ctype.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

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

static int dir_exists(const char *p)
{
	struct stat st;
	return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static void (*log_fn)(const char *);
void cmod_wifi_set_log(void (*fn)(const char *msg)) { log_fn = fn; }
static void plog(const char *fmt, ...)
{
	if (!log_fn) return;
	char b[300];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	log_fn(b);
}

static const char *env_or(const char *k, const char *d) { const char *v = getenv(k); return v && *v ? v : d; }
static const char *conf_path(void) { return env_or("CMOD_WPA_CONF", "/media/fat/linux/wpa_supplicant.conf"); }
static const char *pid_path(void) { return env_or("CMOD_WPA_PID", "/run/wpa_supplicant.wlan0.pid"); }
static const char *ifname(void) { return env_or("CMOD_WIFI_IF", "wlan0"); }

/* runs a command, returns its output (malloc'd) or NULL; killed after limit seconds */
static char *capture(const char *const *argv, int limit)
{
	int po[2];
	if (pipe2(po, O_CLOEXEC)) return NULL;          /* (not inherited by what other threads start) */
	fcntl(po[0], F_SETFL, O_NONBLOCK);
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, po[1], 1);
	posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
	posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_addclose(&fa, po[0]);
	pid_t pid;
	int rc = posix_spawnp(&pid, argv[0], &fa, NULL, (char *const *)argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	close(po[1]);
	if (rc) { close(po[0]); return NULL; }
	size_t cap = 16384, len = 0;
	char *buf = malloc(cap);
	time_t end = time(NULL) + limit;
	int st;
	for (;;) {
		ssize_t r = read(po[0], buf + len, cap - len - 1);
		if (r > 0) { len += (size_t)r; if (len + 4096 > cap) { cap *= 2; buf = realloc(buf, cap); } continue; }
		if (waitpid(pid, &st, WNOHANG) == pid) {
			while ((r = read(po[0], buf + len, cap - len - 1)) > 0) { len += (size_t)r; if (len + 4096 > cap) { cap *= 2; buf = realloc(buf, cap); } }
			break;
		}
		if (time(NULL) > end) { kill(pid, SIGKILL); waitpid(pid, &st, 0); break; }
		usleep(50000);
	}
	close(po[0]);
	buf[len] = 0;
	return buf;
}

/* iw prints SSIDs with \xNN escapes */
static void iw_unescape(const char *in, char *out, size_t n)
{
	size_t j = 0;
	for (const char *p = in; *p && j + 1 < n; p++) {
		if (p[0] == '\\' && p[1] == 'x' && isxdigit((unsigned char)p[2]) && isxdigit((unsigned char)p[3])) {
			char h[3] = { p[2], p[3], 0 };
			out[j++] = (char)strtol(h, NULL, 16);
			p += 3;
		} else {
			out[j++] = *p;
		}
	}
	out[j] = 0;
}

/* ---------- status ---------- */

void cmod_wifi_status(cmod_wifi_status_t *s)
{
	memset(s, 0, sizeof(*s));
	snprintf(s->iface, sizeof(s->iface), "%s", ifname());
	char sys[200];
	snprintf(sys, sizeof(sys), "/sys/class/net/%s", ifname());
	s->present = getenv("CMOD_IW") || dir_exists(sys);
	if (!s->present) return;
	const char *iw = env_or("CMOD_IW", "iw");
	const char *a1[] = { iw, "dev", ifname(), "link", NULL };
	char *out = capture(a1, 5);
	if (out) {
		char *p = strstr(out, "SSID: ");
		if (p) {
			char raw[200];
			sscanf(p + 6, "%199[^\n]", raw);
			iw_unescape(raw, s->ssid, sizeof(s->ssid));
			s->connected = 1;
		}
		if ((p = strstr(out, "signal: "))) s->signal = atoi(p + 8);
		if ((p = strstr(out, "freq: "))) s->freq = atoi(p + 6);
		free(out);
	}
	const char *a2[] = { "ip", "-4", "-o", "addr", "show", ifname(), NULL };
	out = capture(a2, 3);
	if (out) {
		char *p = strstr(out, "inet ");
		if (p) sscanf(p + 5, "%47[^/ ]", s->ip);
		free(out);
	}
	s->supplicant = 0;
	FILE *f = fopen(pid_path(), "r");
	if (f) {
		int pid = 0;
		if (fscanf(f, "%d", &pid) == 1 && pid > 0 && kill(pid, 0) == 0) s->supplicant = 1;
		fclose(f);
	}
}

/* ---------- scanning, in the background ---------- */

static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static cmod_wifi_net_t found[CMOD_WIFI_MAX];
static int nfound, scanning;
static time_t scanned_at;
static char scan_error[160];

static void add_bss(cmod_wifi_net_t *b)
{
	if (!b->ssid[0]) return;                       /* hidden networks: joined by name, not listed */
	for (int i = 0; i < nfound; i++) {
		if (strcmp(found[i].ssid, b->ssid)) continue;
		if (b->signal > found[i].signal) { int bands = found[i].bands; found[i] = *b; found[i].bands |= bands; }
		found[i].bands |= b->bands;
		return;
	}
	if (nfound < CMOD_WIFI_MAX) found[nfound++] = *b;
}

static int by_signal(const void *a, const void *b) { return ((const cmod_wifi_net_t *)b)->signal - ((const cmod_wifi_net_t *)a)->signal; }

static void *scan_thread(void *arg)
{
	(void)arg;
	const char *iw = env_or("CMOD_IW", "iw");
	const char *argv[] = { iw, "dev", ifname(), "scan", NULL };
	char *out = capture(argv, 20);
	pthread_mutex_lock(&lk);
	nfound = 0;
	scan_error[0] = 0;
	if (!out || !strstr(out, "BSS ")) {
		snprintf(scan_error, sizeof(scan_error), "No networks found (is WiFi on?)");
	} else {
		cmod_wifi_net_t cur;
		int have = 0;
		char *save = NULL;
		for (char *line = strtok_r(out, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
			char *t = line;
			while (*t == ' ' || *t == '\t') t++;
			if (!strncmp(line, "BSS ", 4)) {
				if (have) add_bss(&cur);
				memset(&cur, 0, sizeof(cur));
				have = 1;
			} else if (!have) {
				continue;
			} else if (!strncmp(t, "SSID: ", 6)) {
				iw_unescape(t + 6, cur.ssid, sizeof(cur.ssid));
			} else if (!strncmp(t, "signal: ", 8)) {
				cur.signal = (int)atof(t + 8);
			} else if (!strncmp(t, "freq: ", 6)) {
				int f = atoi(t + 6);
				cur.bands |= f >= 5000 ? 2 : 1;
			} else if (!strncmp(t, "capability:", 11)) {
				if (strstr(t, "Privacy")) cur.secure = 1;
			} else if (strstr(t, "Authentication suites:")) {
				if (strstr(t, "SAE")) cur.wpa3 = 1;
				if (strstr(t, "PSK")) cur.psk = 1;
				if (strstr(t, "IEEE 802.1X")) cur.enterprise = 1;
			}
		}
		if (have) add_bss(&cur);
		for (int i = 0; i < nfound; i++) if (found[i].psk || found[i].wpa3 || found[i].enterprise) found[i].secure = 1;
		qsort(found, (size_t)nfound, sizeof(found[0]), by_signal);
	}
	free(out);
	scanning = 0;
	scanned_at = time(NULL);
	pthread_mutex_unlock(&lk);
	return NULL;
}

int cmod_wifi_scan_start(void)
{
	pthread_mutex_lock(&lk);
	if (scanning) { pthread_mutex_unlock(&lk); return 1; }
	scanning = 1;
	pthread_mutex_unlock(&lk);
	pthread_t t;
	if (pthread_create(&t, NULL, scan_thread, NULL)) { scanning = 0; return -1; }
	pthread_detach(t);
	return 0;
}

int cmod_wifi_scan_results(cmod_wifi_net_t *out, int max, int *busy, time_t *at, char *err, size_t errn)
{
	pthread_mutex_lock(&lk);
	int n = nfound < max ? nfound : max;
	memcpy(out, found, sizeof(found[0]) * (size_t)n);
	*busy = scanning;
	*at = scanned_at;
	snprintf(err, errn, "%s", scan_error);
	pthread_mutex_unlock(&lk);
	return n;
}

/* ---------- the settings file ---------- */

typedef struct { char ssid[CMOD_WIFI_SSID]; int priority; char *text; } block_t;   /* text: the whole network={...} */

typedef struct {
	char *head;                 /* everything before the first network block */
	block_t b[CMOD_WIFI_SAVED_MAX];
	int n;
} conf_t;

static void conf_free(conf_t *c)
{
	free(c->head);
	for (int i = 0; i < c->n; i++) free(c->b[i].text);
	memset(c, 0, sizeof(*c));
}

static char *slurp(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *b = malloc((size_t)n + 1);
	n = (long)fread(b, 1, (size_t)n, f);
	b[n] = 0;
	fclose(f);
	return b;
}

/* a field "key=" at the start of a line (after tabs or spaces), not inside a value or a longer key
   (bssid=, scan_ssid=); NULL when there's none */
static char *field(char *text, const char *key)
{
	size_t kl = strlen(key);
	for (char *l = text; l && *l; ) {
		char *q = l;
		while (*q == ' ' || *q == '\t') q++;
		if (!strncmp(q, key, kl)) return q;
		l = strchr(l, '\n');
		if (l) l++;
	}
	return NULL;
}

/* the end of a network={ block: the line that's just "}" (a "}" in a quoted SSID or a #psk="..."
   comment doesn't end it); NULL when the block isn't closed */
static char *block_end(char *p)
{
	for (char *l = strchr(p, '\n'); l; l = strchr(l, '\n')) {
		l++;
		char *q = l;
		while (*q == ' ' || *q == '\t') q++;
		if (*q == '}') {
			q++;
			while (*q == ' ' || *q == '\t' || *q == '\r') q++;
			if (*q == '\n' || !*q) return q;      /* just past the "}" (the caller takes its newline) */
		}
	}
	return NULL;
}

static int conf_read(conf_t *c)
{
	memset(c, 0, sizeof(*c));
	char *all = slurp(conf_path());
	if (!all) { c->head = strdup(""); return 0; }   /* no file yet: networks only, like MiSTer's own */
	char *p = strstr(all, "network={");
	c->head = strndup(all, p ? (size_t)(p - all) : strlen(all));
	while (p && c->n < CMOD_WIFI_SAVED_MAX) {
		char *e = block_end(p);
		if (!e) break;
		if (*e == '\n') e++;
		block_t *b = &c->b[c->n++];
		b->text = strndup(p, (size_t)(e - p));
		char *s = field(b->text, "ssid=");               /* not bssid= or scan_ssid= */
		if (s) {
			s += 5;
			if (*s == '"') { s++; size_t k = 0; while (s[k] && s[k] != '"' && k + 1 < sizeof(b->ssid)) { b->ssid[k] = s[k]; k++; } b->ssid[k] = 0; }
			else {   /* hex SSID */
				size_t k = 0;
				while (isxdigit((unsigned char)s[0]) && isxdigit((unsigned char)s[1]) && k + 1 < sizeof(b->ssid)) { char h[3] = { s[0], s[1], 0 }; b->ssid[k++] = (char)strtol(h, NULL, 16); s += 2; }
				b->ssid[k] = 0;
			}
		}
		char *pr = field(b->text, "priority=");
		b->priority = pr ? atoi(pr + 9) : 0;
		p = strstr(e, "network={");
	}
	free(all);
	return 0;
}

/* (CMOD_HOME in the environment moves it, for tests) */
static void backup_dir(char *out, size_t n)
{
	const char *h = getenv("CMOD_HOME");
	if (h && *h) snprintf(out, n, "%s/backups/wifi", h);
	else snprintf(out, n, "%s", CMOD_BK_WIFI);
}

/* a copy before every change; the last 10 are kept */
static int backup(void)
{
	char dir[600], dest[700];
	backup_dir(dir, sizeof(dir));
	mkdirs(dir);
	char *all = slurp(conf_path());
	if (!all) return 0;                             /* nothing to back up yet */
	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);
	FILE *f = NULL;
	for (int seq = 0; seq < 100 && !f; seq++) {       /* (several changes in one second: each its own copy) */
		snprintf(dest, sizeof(dest), "%s/wpa_supplicant.conf.%04d%02d%02d-%02d%02d%02d-%02d", dir, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		         tm.tm_hour, tm.tm_min, tm.tm_sec, seq);
		f = fopen(dest, "wx");
	}
	if (!f) { free(all); return -1; }
	fputs(all, f);
	free(all);
	if (fclose(f)) return -1;
	/* keep the newest 10 */
	char cmd[800];
	snprintf(cmd, sizeof(cmd), "ls -1 '%s'/wpa_supplicant.conf.* 2>/dev/null | sort -r | tail -n +11 | while read f; do rm -f \"$f\"; done", dir);
	if (system(cmd)) { /* best effort */ }
	return 0;
}

static int conf_write(conf_t *c)
{
	char tmp[600];
	snprintf(tmp, sizeof(tmp), "%s.cm-new", conf_path());
	FILE *f = fopen(tmp, "w");
	if (!f) return -1;
	fputs(c->head, f);
	if (c->head[0] && c->head[strlen(c->head) - 1] != '\n') fputc('\n', f);
	for (int i = 0; i < c->n; i++) fputs(c->b[i].text, f);
	if (fflush(f) || fsync(fileno(f)) || fclose(f)) { unlink(tmp); return -1; }
	if (rename(tmp, conf_path())) { unlink(tmp); return -1; }
	sync();
	return 0;
}

/* tell wpa_supplicant to read the file again; start it the way MiSTer does if it isn't running */
static pthread_mutex_t reload_lk = PTHREAD_MUTEX_INITIALIZER;   /* one reload at a time (saving, and the keep-up thread) */
static void reload_locked(void);
static void reload(void)
{
	pthread_mutex_lock(&reload_lk);
	reload_locked();
	pthread_mutex_unlock(&reload_lk);
}
static void reload_locked(void)
{
	FILE *f = fopen(pid_path(), "r");
	int pid = 0;
	if (f) { if (fscanf(f, "%d", &pid) != 1) pid = 0; fclose(f); }
	if (pid > 0 && kill(pid, SIGHUP) == 0) { plog("wifi: wpa_supplicant reloading its settings"); return; }
	if (getenv("CMOD_WPA_CONF")) return;           /* tests: no real supplicant */
	const char *argv[] = { "wpa_supplicant", "-s", "-B", "-P", pid_path(), "-i", ifname(), "-D", "nl80211,wext", "-c", conf_path(), NULL };
	char *out = capture(argv, 10);
	free(out);
	plog("wifi: started wpa_supplicant");
}

/* WPA key from a passphrase (wpa_passphrase), so the password isn't stored as text */
static int psk_of(const char *ssid, const char *pass, char *out, size_t n)
{
	const char *argv[] = { "wpa_passphrase", ssid, pass, NULL };
	char *o = capture(argv, 5);
	if (!o) return -1;
	char *p = strstr(o, "\tpsk=");
	if (!p) { free(o); return -1; }
	p += 5;
	size_t k = 0;
	while (isxdigit((unsigned char)p[k]) && k + 1 < n) { out[k] = p[k]; k++; }
	out[k] = 0;
	free(o);
	return k == 64 ? 0 : -1;
}

static void quote_ssid(const char *ssid, char *out, size_t n)
{
	/* plain text in quotes unless it has a quote or control character: then hex */
	int plain = 1;
	for (const unsigned char *p = (const unsigned char *)ssid; *p; p++)
		if (*p == '"' || *p < 32 || *p == '{' || *p == '}' || *p == '#' || *p == '\\') plain = 0;
	if (plain) { snprintf(out, n, "\"%s\"", ssid); return; }
	size_t j = 0;
	for (const unsigned char *p = (const unsigned char *)ssid; *p && j + 3 < n; p++) j += (size_t)snprintf(out + j, n - j, "%02x", *p);
}

int cmod_wifi_saved(cmod_wifi_saved_t *out, int max)
{
	conf_t c;
	conf_read(&c);
	int n = 0;
	for (int i = 0; i < c.n && n < max; i++) {
		snprintf(out[n].ssid, sizeof(out[n].ssid), "%s", c.b[i].ssid);
		out[n].priority = c.b[i].priority;
		out[n].open = strstr(c.b[i].text, "key_mgmt=NONE") != NULL;
		n++;
	}
	/* highest priority first: that's the order wpa_supplicant tries them */
	for (int i = 0; i < n; i++)
		for (int k = i + 1; k < n; k++)
			if (out[k].priority > out[i].priority) { cmod_wifi_saved_t t = out[i]; out[i] = out[k]; out[k] = t; }
	conf_free(&c);
	return n;
}

int cmod_wifi_connect(const char *ssid, const char *password, int hidden, char *err, size_t errn)
{
	if (!ssid || !*ssid || strlen(ssid) > 32) { snprintf(err, errn, "Network name missing or too long"); return -1; }
	char psk[80] = "";
	if (password && *password) {
		size_t pl = strlen(password);
		if (pl < 8 || pl > 63) { snprintf(err, errn, "WiFi passwords are 8 to 63 characters"); return -1; }
		if (psk_of(ssid, password, psk, sizeof(psk))) { snprintf(err, errn, "Couldn't work out the WiFi key"); return -1; }
	}
	conf_t c;
	conf_read(&c);
	int top = 0, idx = -1;
	for (int i = 0; i < c.n; i++) { if (c.b[i].priority > top) top = c.b[i].priority; if (!strcmp(c.b[i].ssid, ssid)) idx = i; }
	if (idx >= 0 && !psk[0] && !strstr(c.b[idx].text, "key_mgmt=NONE")) {
		/* saved already, no new password: just make it the first choice */
		char *p = strstr(c.b[idx].text, "priority=");
		if (p) { char *e = p + 9; while (isdigit((unsigned char)*e)) e++; char nt[2048]; snprintf(nt, sizeof(nt), "%.*spriority=%d%s", (int)(p - c.b[idx].text), c.b[idx].text, top + 1, e); free(c.b[idx].text); c.b[idx].text = strdup(nt); }
		else { char nt[2048]; size_t l = strlen(c.b[idx].text); char *close = strrchr(c.b[idx].text, '}'); snprintf(nt, sizeof(nt), "%.*s\tpriority=%d\n%s", (int)(close - c.b[idx].text), c.b[idx].text, top + 1, close); (void)l; free(c.b[idx].text); c.b[idx].text = strdup(nt); }
	} else {
		char q[80], text[400];
		quote_ssid(ssid, q, sizeof(q));
		if (psk[0]) snprintf(text, sizeof(text), "network={\n\tssid=%s\n%s\tpsk=%s\n\tpriority=%d\n}\n", q, hidden ? "\tscan_ssid=1\n" : "", psk, top + 1);
		else snprintf(text, sizeof(text), "network={\n\tssid=%s\n%s\tkey_mgmt=NONE\n\tpriority=%d\n}\n", q, hidden ? "\tscan_ssid=1\n" : "", top + 1);
		if (idx >= 0) { free(c.b[idx].text); c.b[idx].text = strdup(text); c.b[idx].priority = top + 1; }
		else if (c.n < CMOD_WIFI_SAVED_MAX) { snprintf(c.b[c.n].ssid, sizeof(c.b[0].ssid), "%s", ssid); c.b[c.n].priority = top + 1; c.b[c.n].text = strdup(text); c.n++; }
		else { conf_free(&c); snprintf(err, errn, "Too many saved networks"); return -1; }
	}
	int rc = backup() || conf_write(&c);
	conf_free(&c);
	if (rc) { snprintf(err, errn, "Couldn't save the WiFi settings"); return -1; }
	reload();
	plog("wifi: saved \"%s\"%s", ssid, psk[0] ? " (WPA)" : " (open)");
	return 0;
}

int cmod_wifi_forget(const char *ssid, char *err, size_t errn)
{
	conf_t c;
	conf_read(&c);
	int k = 0, gone = 0;
	for (int i = 0; i < c.n; i++) {
		if (!strcmp(c.b[i].ssid, ssid)) { free(c.b[i].text); gone = 1; continue; }
		c.b[k++] = c.b[i];
	}
	c.n = k;
	if (!gone) { conf_free(&c); snprintf(err, errn, "That network isn't saved"); return -1; }
	int rc = backup() || conf_write(&c);
	conf_free(&c);
	if (rc) { snprintf(err, errn, "Couldn't save the WiFi settings"); return -1; }
	reload();
	plog("wifi: forgot \"%s\"", ssid);
	return 0;
}

/* move a saved network up (dir -1) or down (+1) in the order wpa_supplicant tries them */
int cmod_wifi_move(const char *ssid, int dir, char *err, size_t errn)
{
	cmod_wifi_saved_t s[CMOD_WIFI_SAVED_MAX];
	int n = cmod_wifi_saved(s, CMOD_WIFI_SAVED_MAX), at = -1;
	for (int i = 0; i < n; i++) if (!strcmp(s[i].ssid, ssid)) at = i;
	if (at < 0) { snprintf(err, errn, "That network isn't saved"); return -1; }
	int to = at + dir;
	if (to < 0 || to >= n) return 0;
	cmod_wifi_saved_t t = s[at]; s[at] = s[to]; s[to] = t;
	conf_t c;
	conf_read(&c);
	for (int i = 0; i < c.n; i++) {
		int pr = 0;
		for (int k = 0; k < n; k++) if (!strcmp(s[k].ssid, c.b[i].ssid)) pr = n - k;      /* first = highest */
		char *p = strstr(c.b[i].text, "priority="), nt[2048];
		if (p) { char *e = p + 9; while (isdigit((unsigned char)*e)) e++; snprintf(nt, sizeof(nt), "%.*spriority=%d%s", (int)(p - c.b[i].text), c.b[i].text, pr, e); }
		else { char *close = strrchr(c.b[i].text, '}'); snprintf(nt, sizeof(nt), "%.*s\tpriority=%d\n%s", (int)(close - c.b[i].text), c.b[i].text, pr, close); }
		free(c.b[i].text);
		c.b[i].text = strdup(nt);
	}
	int rc = backup() || conf_write(&c);
	conf_free(&c);
	if (rc) { snprintf(err, errn, "Couldn't save the WiFi settings"); return -1; }
	reload();
	return 0;
}

/* put back the file as it was before the last change */
int cmod_wifi_undo(char *err, size_t errn)
{
	char dir[600], cmd[800];
	backup_dir(dir, sizeof(dir));
	snprintf(cmd, sizeof(cmd), "ls -1 '%s'/wpa_supplicant.conf.* 2>/dev/null | sort -r | head -n 1", dir);
	FILE *p = popen(cmd, "r");
	char last[700] = "";
	if (p) { if (!fgets(last, sizeof(last), p)) last[0] = 0; pclose(p); }
	last[strcspn(last, "\n")] = 0;
	if (!last[0]) { snprintf(err, errn, "Nothing to undo"); return -1; }
	char *all = slurp(last);
	if (!all) { snprintf(err, errn, "Couldn't read the backup"); return -1; }
	char tmp[600];
	snprintf(tmp, sizeof(tmp), "%s.cm-new", conf_path());
	FILE *f = fopen(tmp, "w");
	if (!f) { free(all); snprintf(err, errn, "Couldn't write the WiFi settings"); return -1; }
	fputs(all, f);
	free(all);
	if (fclose(f) || rename(tmp, conf_path())) { unlink(tmp); snprintf(err, errn, "Couldn't write the WiFi settings"); return -1; }
	sync();
	unlink(last);                                   /* undone: the next undo goes one further back */
	reload();
	plog("wifi: undid the last change");
	return 0;
}

/* ---------- keeping WiFi up ----------
 * MiSTer starts wpa_supplicant once at boot (ifup wlan0), and a USB WiFi adapter that isn't ready yet
 * then stays off ("Failed to bring up wlan0") until something starts it. With a saved network, this
 * starts it as soon as the adapter is there: every 5 s for the first 2 minutes, then every 30 s. */
static int supplicant_running(void)
{
	FILE *f = fopen(pid_path(), "r");
	int pid = 0;
	if (f) { if (fscanf(f, "%d", &pid) != 1) pid = 0; fclose(f); }
	return pid > 0 && kill(pid, 0) == 0;
}

static int has_saved_network(void)
{
	FILE *f = fopen(conf_path(), "r");
	if (!f) return 0;
	char line[256];
	int yes = 0;
	while (!yes && fgets(line, sizeof(line), f)) if (strstr(line, "network={")) yes = 1;
	fclose(f);
	return yes;
}

static void *keep_up(void *arg)
{
	(void)arg;
	for (int t = 0;; t++) {
		char sys[200];
		snprintf(sys, sizeof(sys), "/sys/class/net/%s", ifname());
		if (dir_exists(sys) && has_saved_network() && !supplicant_running()) {
			pthread_mutex_lock(&lk);
			reload();
			pthread_mutex_unlock(&lk);
		}
		sleep(t < 24 ? 5 : 30);
	}
	return NULL;
}

void cmod_wifi_start(void)
{
	if (getenv("CMOD_WPA_CONF")) return;         /* tests */
	pthread_t th;
	if (pthread_create(&th, NULL, keep_up, NULL) == 0) pthread_detach(th);
}
