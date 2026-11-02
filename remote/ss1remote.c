/* ss1remote: the app link. The phone and PC apps reach Console Mode's game library over the home network,
 * with the same requests as Phosphor's remote access (docs/remote-api.md in phosphor_os).
 * Copyright (C) 2026 f3bandit. GPL-3.0-or-later (see ../LICENSE); Retro Remake: see ../LICENSE-RETRO-REMAKE.md.
 *
 * Not SS1 Link: that one is for debugging and testing from a PC (ss1-link/). This one is for end users.
 *
 *   GET  /info                       {name, version, api}, no key needed; ?nonce=: proofs (see below)
 *   POST /pair    {"code","name"}    {token, id}: the code Console Mode shows (Pair a device), 5 tries, 2 minutes
 *   POST /api     {"cmd":...}        status, categories, systems, games, game, last, launch, edit, rescan (cmlib.c)
 *   GET  /art/<game id>              the game's box art (PNG)
 *   PUT  /art/<game id>   <image>    new box art (PNG/JPEG/..., up to 16 MB), stored as Console Mode's PNG
 *   GET  /font/regular|bold          Console Mode's own font, so an app can look like the TV
 *
 * Off until turned on (from Console Mode, through the control socket). Plain HTTP/1.1 on port 8767, one
 * request per connection, home-network addresses only. Every request but /info and /pair needs
 * "Authorization: Bearer <token>"; only a hash of each token is kept (remote.ini).
 *
 * Console Mode drives it through a Unix socket, /tmp/ss1remote.sock, one line in, key=value lines out
 * (ss1_remote_cmd() in ss1mod.h): info | on | off | pair | pair_stop | forget <id> | rescan.
 *
 * Ported from phosphord's remote.c (Phosphor OS, also f3bandit's). */
#define _GNU_SOURCE
#include "cmlib.h"
#include "jobs.h"
#include "ua.h"
#include "tools.h"
#include "sha256.h"
#include "vendor/cJSON.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <limits.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define API_VERSION 1
#define DEFAULT_PORT 8767
#define MAX_DEVICES 16
#define HEAD_MAX (16 * 1024)
#define API_BODY_MAX (1024 * 1024)
#define ART_BODY_MAX (16 * 1024 * 1024)
#define PAIR_SECONDS 120
#define PAIR_TRIES 5
#define PAIR_BODY_MAX 1024
#define HEAD_MS 3000
#define PAIR_BODY_MS 3000
#define CONN_MAX 8
#define CONN_PER_IP 6
#define FAIL_LIMIT 5
#define LOCK_SECONDS 30
#define FAIL_FORGET 600
#define NPEERS 32

static const char *home = "/media/fat/ConsoleMode/ss1";
static const char *ctl_path = "/tmp/ss1remote.sock";
static char ini_file[PATH_MAX], log_file[PATH_MAX];
/* what /info calls this MiSTer: the apps look for "Phosphor" until they know Console Mode (remote.ini: name=) */
static char server_name[32] = "Console Mode";

typedef struct {
	char hash[65];
	char name[64];
	long created, last_seen;
} device_t;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int on, port = DEFAULT_PORT, listening;
static char err_msg[128];
static device_t devs[MAX_DEVICES];
static int ndevs;
static char code[8];
static long code_until;
static int code_tries;
static char paired_name[64];
static long saved_at;

void plog(const char *fmt, ...)
{
	FILE *f = fopen(log_file, "a");
	if (!f) return;
	time_t t = time(NULL);
	char ts[32];
	strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&t));
	fprintf(f, "%s ", ts);
	va_list ap;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
	fclose(f);
}

/* ---------- settings: remote.ini ---------- */

static void load(void)
{
	char line[512];
	FILE *f = fopen(ini_file, "r");
	if (!f) return;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		char *s = line;
		while (*s == ' ' || *s == '\t') s++;
		if (!*s || *s == ';' || *s == '#') continue;
		char *eq = strchr(s, '=');
		if (!eq) continue;
		*eq = 0;
		char *k = s, *v = eq + 1;
		for (char *e = eq - 1; e >= k && (*e == ' ' || *e == '\t'); e--) *e = 0;
		while (*v == ' ' || *v == '\t') v++;
		if (!strcmp(k, "on")) on = atoi(v) != 0;
		else if (!strcmp(k, "port")) { int p = atoi(v); if (p > 0 && p < 65536) port = p; }
		else if (!strcmp(k, "name") && *v) snprintf(server_name, sizeof(server_name), "%s", v);
		else if (!strcmp(k, "device") && ndevs < MAX_DEVICES) {
			device_t *d = &devs[ndevs];
			int used = 0;
			if (sscanf(v, "%64s %ld %ld %n", d->hash, &d->created, &d->last_seen, &used) >= 3 && strlen(d->hash) == 64) {
				snprintf(d->name, sizeof(d->name), "%s", v + used);
				ndevs++;
			}
		}
	}
	fclose(f);
}

/* call with mu held */
static int save(void)
{
	char tmp[PATH_MAX + 8];
	snprintf(tmp, sizeof(tmp), "%s.new", ini_file);
	mkdir(home, 0755);
	FILE *f = fopen(tmp, "w");
	if (!f) return -1;
	fprintf(f, "; The app link (ss1remote). Devices: key hash, paired, last used, name.\n"
	           "; Only hashes of the keys are kept here; remove a line to forget a device.\n");
	fprintf(f, "on = %d\nport = %d\nname = %s\n", on, port, server_name);
	for (int i = 0; i < ndevs; i++) fprintf(f, "device = %s %ld %ld %s\n", devs[i].hash, devs[i].created, devs[i].last_seen, devs[i].name);
	int bad = fflush(f) != 0 || fsync(fileno(f)) != 0;
	bad |= fclose(f) != 0;
	if (bad || rename(tmp, ini_file) != 0) { unlink(tmp); return -1; }
	saved_at = time(NULL);
	return 0;
}

static int random_bytes(void *buf, size_t n)
{
	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	if (fd < 0) return -1;
	size_t got = 0;
	while (got < n) {
		ssize_t r = read(fd, (char *)buf + got, n - got);
		if (r <= 0) { if (r < 0 && errno == EINTR) continue; close(fd); return -1; }
		got += (size_t)r;
	}
	close(fd);
	return 0;
}

/* ---------- HTTP ---------- */

typedef struct {
	int fd;
	uint32_t ip;
	long long deadline_ms;
	char head[HEAD_MAX + 1];
	size_t hlen, body_off;
	char method[8], path[256];
	long clen;
	int chunked, expect;
	char auth[160];
} req_t;

static long long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

enum { F_KEY, F_CODE };
typedef struct {
	uint32_t ip;
	int conns;
	int fails[2];
	long last[2], until[2];
	long seen;
} peer_t;

static pthread_mutex_t pmu = PTHREAD_MUTEX_INITIALIZER;
static peer_t peers[NPEERS];
static int nconns;

static long mono_s(void) { return (long)(now_ms() / 1000); }

static peer_t *peer_get(uint32_t ip)
{
	peer_t *pick = NULL;
	for (int i = 0; i < NPEERS; i++) if (peers[i].ip == ip) return &peers[i];
	for (int i = 0; i < NPEERS; i++) {
		peer_t *p = &peers[i];
		if (p->conns) continue;
		if (!pick || (pick->ip && (!p->ip || p->seen < pick->seen))) pick = p;
	}
	memset(pick, 0, sizeof(*pick));
	pick->ip = ip;
	return pick;
}

static int conn_take(uint32_t ip)
{
	pthread_mutex_lock(&pmu);
	int ok = 0;
	if (nconns < CONN_MAX) {
		peer_t *p = peer_get(ip);
		p->seen = mono_s();
		if (p->conns < CONN_PER_IP) { p->conns++; nconns++; ok = 1; }
	}
	pthread_mutex_unlock(&pmu);
	return ok;
}

static void conn_drop(uint32_t ip)
{
	pthread_mutex_lock(&pmu);
	peer_t *p = peer_get(ip);
	if (p->conns > 0) p->conns--;
	nconns--;
	pthread_mutex_unlock(&pmu);
}

static int locked_for(uint32_t ip, int kind)
{
	pthread_mutex_lock(&pmu);
	long left = peer_get(ip)->until[kind] - mono_s();
	pthread_mutex_unlock(&pmu);
	return left > 0 ? (int)left : 0;
}

static void failed(uint32_t ip, int kind)
{
	long now = mono_s();
	int lock = 0;
	pthread_mutex_lock(&pmu);
	peer_t *p = peer_get(ip);
	if (now - p->last[kind] > FAIL_FORGET) p->fails[kind] = 0;
	p->last[kind] = now;
	if (++p->fails[kind] >= FAIL_LIMIT) { p->fails[kind] = 0; p->until[kind] = now + LOCK_SECONDS; lock = 1; }
	pthread_mutex_unlock(&pmu);
	if (lock) {
		struct in_addr a = { ip };
		char s[32];
		inet_ntop(AF_INET, &a, s, sizeof(s));
		plog("%s waits %d s (too many wrong %s)", s, LOCK_SECONDS, kind == F_KEY ? "keys" : "codes");
	}
}

static void succeeded(uint32_t ip, int kind)
{
	pthread_mutex_lock(&pmu);
	peer_get(ip)->fails[kind] = 0;
	pthread_mutex_unlock(&pmu);
}

static ssize_t rx(req_t *r, void *buf, size_t n)
{
	for (;;) {
		long long left = r->deadline_ms - now_ms();
		if (left <= 0) { errno = ETIMEDOUT; return -1; }
		struct pollfd p = { r->fd, POLLIN, 0 };
		int k = poll(&p, 1, (int)(left > 5000 ? 5000 : left));
		if (k < 0) { if (errno == EINTR) continue; return -1; }
		if (k == 0) continue;
		ssize_t got = recv(r->fd, buf, n, 0);
		if (got < 0 && errno == EINTR) continue;
		return got;
	}
}

static int send_all(int fd, const void *p, size_t n)
{
	const char *s = p;
	while (n) {
		ssize_t w = send(fd, s, n, MSG_NOSIGNAL);
		if (w < 0) { if (errno == EINTR) continue; return -1; }
		s += w;
		n -= (size_t)w;
	}
	return 0;
}

static const char *status_text(int c)
{
	switch (c) {
	case 200: return "OK";
	case 400: return "Bad Request";
	case 401: return "Unauthorized";
	case 403: return "Forbidden";
	case 404: return "Not Found";
	case 405: return "Method Not Allowed";
	case 411: return "Length Required";
	case 413: return "Payload Too Large";
	case 429: return "Too Many Requests";
	case 500: return "Internal Server Error";
	default: return "Service Unavailable";
	}
}

static void respond(int fd, int st, const char *type, const void *body, size_t n)
{
	char h[300];
	int hl = snprintf(h, sizeof(h), "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
	                  "Cache-Control: no-store\r\nConnection: close\r\n\r\n", st, status_text(st), type, n);
	if (send_all(fd, h, (size_t)hl) == 0 && n) send_all(fd, body, n);
}

static void respond_json(int fd, int st, const char *json) { respond(fd, st, "application/json", json, strlen(json)); }

static void respond_obj(int fd, int st, cJSON *o)
{
	char *s = cJSON_PrintUnformatted(o);
	respond_json(fd, st, s ? s : "{\"ok\":false,\"error\":\"out of memory\"}");
	free(s);
}

static void respond_error(int fd, int st, const char *msg)
{
	cJSON *o = cJSON_CreateObject();
	cJSON_AddBoolToObject(o, "ok", 0);
	cJSON_AddStringToObject(o, "error", msg);
	respond_obj(fd, st, o);
	cJSON_Delete(o);
}

static int read_head(req_t *r)
{
	r->hlen = 0;
	for (;;) {
		if (r->hlen >= HEAD_MAX) return 413;
		ssize_t n = rx(r, r->head + r->hlen, HEAD_MAX - r->hlen);
		if (n <= 0) return -1;
		r->hlen += (size_t)n;
		r->head[r->hlen] = 0;
		char *e = strstr(r->head, "\r\n\r\n");
		if (e) { r->body_off = (size_t)(e - r->head) + 4; break; }
	}
	char *line = r->head, *eol = strstr(line, "\r\n");
	*eol = 0;
	if (sscanf(line, "%7s %255s", r->method, r->path) != 2) return 400;
	r->clen = -1;
	for (char *h = eol + 2; h < r->head + r->body_off - 2;) {
		char *e = strstr(h, "\r\n");
		if (!e) break;
		*e = 0;
		char *c = strchr(h, ':');
		if (c) {
			*c = 0;
			char *v = c + 1;
			while (*v == ' ' || *v == '\t') v++;
			if (!strcasecmp(h, "content-length")) {
				char *end;
				errno = 0;
				long l = strtol(v, &end, 10);
				if (errno || *end || l < 0) return 400;
				r->clen = l;
			} else if (!strcasecmp(h, "transfer-encoding")) r->chunked = strcasecmp(v, "identity") != 0;
			else if (!strcasecmp(h, "expect")) r->expect = !strcasecmp(v, "100-continue");
			else if (!strcasecmp(h, "authorization")) snprintf(r->auth, sizeof(r->auth), "%s", v);
		}
		h = e + 2;
	}
	return 0;
}

static int read_body(req_t *r, char *buf, int out)
{
	size_t have = r->hlen - r->body_off, want = (size_t)r->clen;
	if (have > want) have = want;
	if (r->expect && have < want) send_all(r->fd, "HTTP/1.1 100 Continue\r\n\r\n", 25);
	if (out >= 0) { if (have && write(out, r->head + r->body_off, have) != (ssize_t)have) return -1; }
	else memcpy(buf, r->head + r->body_off, have);
	size_t got = have;
	char tmp[16384];
	while (got < want) {
		size_t chunk = want - got;
		if (out >= 0 && chunk > sizeof(tmp)) chunk = sizeof(tmp);
		ssize_t n = rx(r, out >= 0 ? tmp : buf + got, chunk);
		if (n <= 0) return -1;
		if (out >= 0 && write(out, tmp, (size_t)n) != n) return -1;
		got += (size_t)n;
	}
	if (buf) buf[want] = 0;
	return 0;
}

/* a home-network address: the private ranges, or the subnet of one of the SS1's own connections (/16 or narrower) */
static int local_peer(const struct sockaddr_in *a)
{
	uint32_t ip = ntohl(a->sin_addr.s_addr);
	if ((ip >> 24) == 10 || (ip >> 24) == 127 || (ip >> 20) == (172u << 4 | 1) ||
	    (ip >> 16) == (192u << 8 | 168) || (ip >> 16) == (169u << 8 | 254))
		return 1;
	struct ifaddrs *ifs, *i;
	int ok = 0;
	if (getifaddrs(&ifs)) return 0;
	for (i = ifs; i && !ok; i = i->ifa_next) {
		if (!i->ifa_addr || !i->ifa_netmask || i->ifa_addr->sa_family != AF_INET) continue;
		if (!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
		uint32_t me = ntohl(((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr);
		uint32_t m = ntohl(((struct sockaddr_in *)i->ifa_netmask)->sin_addr.s_addr);
		if (m >= 0xffff0000u && m != 0xffffffffu && (me & m) == (ip & m)) ok = 1;
	}
	freeifaddrs(ifs);
	return ok;
}

static int authorize(const req_t *r, char *name, size_t nn)
{
	if (strncasecmp(r->auth, "Bearer ", 7)) return -1;
	const char *tok = r->auth + 7;
	if (strlen(tok) != 64) return -1;
	char h[65];
	sha256_hex(tok, 64, h);
	int found = -1;
	pthread_mutex_lock(&mu);
	for (int i = 0; i < ndevs; i++) {
		int diff = 0;
		for (int k = 0; k < 64; k++) diff |= devs[i].hash[k] ^ h[k];
		if (!diff) {
			found = i;
			snprintf(name, nn, "%s", devs[i].name);
			devs[i].last_seen = time(NULL);
			if (devs[i].last_seen - saved_at > 600) save();
		}
	}
	pthread_mutex_unlock(&mu);
	return found;
}

static void do_pair(req_t *r, const char *body)
{
	cJSON *j = cJSON_Parse(body);
	const char *c = j ? cJSON_GetStringValue(cJSON_GetObjectItem(j, "code")) : NULL;
	const char *nm = j ? cJSON_GetStringValue(cJSON_GetObjectItem(j, "name")) : NULL;
	char name[64];
	snprintf(name, sizeof(name), "%s", nm && *nm ? nm : "Device");
	for (char *p = name; *p; p++) if ((unsigned char)*p < 0x20) *p = ' ';
	int st = 0;
	char token[65] = "", id[17] = "";
	pthread_mutex_lock(&mu);
	long now = time(NULL);
	if (!code[0] || now >= code_until) st = 403;
	else if (!c || strlen(c) != 6) st = 400;
	else {
		int diff = 0;
		for (int k = 0; k < 6; k++) diff |= c[k] ^ code[k];
		if (diff) {
			if (++code_tries >= PAIR_TRIES) code[0] = 0;
			st = 401;
		} else if (ndevs >= MAX_DEVICES) st = 429;
		else {
			uint8_t raw[32];
			if (random_bytes(raw, sizeof(raw))) st = 500;
			else {
				for (int k = 0; k < 32; k++) sprintf(token + k * 2, "%02x", raw[k]);
				device_t *d = &devs[ndevs++];
				sha256_hex(token, 64, d->hash);
				snprintf(d->name, sizeof(d->name), "%s", name);
				d->created = d->last_seen = now;
				snprintf(id, sizeof(id), "%.16s", d->hash);
				snprintf(paired_name, sizeof(paired_name), "%s", name);
				code[0] = 0;
				st = save() ? 500 : 200;
			}
		}
	}
	pthread_mutex_unlock(&mu);
	cJSON_Delete(j);
	if (st == 401) failed(r->ip, F_CODE);
	if (st == 200) {
		succeeded(r->ip, F_CODE);
		plog("paired %s", name);
		cJSON *o = cJSON_CreateObject();
		cJSON_AddBoolToObject(o, "ok", 1);
		cJSON_AddStringToObject(o, "token", token);
		cJSON_AddStringToObject(o, "id", id);
		respond_obj(r->fd, 200, o);
		cJSON_Delete(o);
	} else {
		respond_error(r->fd, st, st == 403 ? "pairing isn't open: choose Pair a device on the SuperStation" :
		                         st == 401 ? "wrong code" : st == 429 ? "too many devices: forget one on the SuperStation" :
		                         st == 400 ? "code and name needed" : "couldn't save the device");
	}
}

static void do_api(req_t *r, const char *body)
{
	cJSON *j = cJSON_Parse(body);
	const char *cmd = j ? cJSON_GetStringValue(cJSON_GetObjectItem(j, "cmd")) : NULL;
	if (!j || !cmd) { cJSON_Delete(j); respond_error(r->fd, 400, "send a JSON command: {\"cmd\":...}"); return; }
	int ncmd = 0;
	for (cJSON *it = j->child; it; it = it->next) if (it->string && !strcasecmp(it->string, "cmd")) ncmd++;
	if (ncmd != 1) { cJSON_Delete(j); respond_error(r->fd, 400, "one \"cmd\" only"); return; }
	cJSON *out = jobs_api(j);
	if (!out) out = tools_api(j);
	if (!out) out = cm_api(j);
	cJSON_Delete(j);
	respond_obj(r->fd, 200, out);
	cJSON_Delete(out);
}

static long art_id(const char *path)
{
	char *end;
	long id = strtol(path + 5, &end, 10);
	return (*end || id <= 0) ? -1 : id;
}

static void do_art_get(req_t *r, long id)
{
	char p[1700];
	if (cm_art_path(id, p, sizeof(p))) { respond_error(r->fd, 404, "no box art"); return; }
	int fd = open(p, O_RDONLY | O_CLOEXEC);
	struct stat sb;
	if (fd < 0 || fstat(fd, &sb) || sb.st_size > ART_BODY_MAX) { if (fd >= 0) close(fd); respond_error(r->fd, 404, "no box art"); return; }
	char *buf = malloc((size_t)sb.st_size + 1);
	ssize_t n = 0;
	while (buf && n < sb.st_size) {
		ssize_t k = read(fd, buf + n, (size_t)(sb.st_size - n));
		if (k < 0 && errno == EINTR) continue;
		if (k <= 0) break;
		n += k;
	}
	close(fd);
	if (!buf || n != sb.st_size) { free(buf); respond_error(r->fd, 500, "couldn't read the art"); return; }
	respond(r->fd, 200, n > 4 && !memcmp(buf, "\x89PNG", 4) ? "image/png" : "image/jpeg", buf, (size_t)n);
	free(buf);
}

static void do_art_put(req_t *r, long id)
{
	if (r->clen > ART_BODY_MAX) { respond_error(r->fd, 413, "image too big (16 MB at most)"); return; }
	r->deadline_ms = now_ms() + 30000 + r->clen / 50;
	char tmp[] = "/tmp/ss1remote-art.XXXXXX";
	int fd = mkstemp(tmp);
	if (fd < 0) { respond_error(r->fd, 500, "no room for the upload"); return; }
	int bad = read_body(r, NULL, fd);
	close(fd);
	if (bad) { unlink(tmp); return; }
	cJSON *o = cm_art_set(id, tmp);
	unlink(tmp);
	respond_obj(r->fd, 200, o);
	cJSON_Delete(o);
}

static void do_font(req_t *r)
{
	char p[800];
	if (cm_font_path(r->path + 6, p, sizeof(p))) { respond_error(r->fd, 404, "no such font"); return; }
	FILE *f = fopen(p, "rb");
	if (!f) { respond_error(r->fd, 404, "no such font"); return; }
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *buf = n > 0 && n < 8L * 1024 * 1024 ? malloc((size_t)n) : NULL;
	if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); respond_error(r->fd, 500, "couldn't read it"); return; }
	fclose(f);
	respond(r->fd, 200, "font/ttf", buf, (size_t)n);
	free(buf);
}

static void do_info(req_t *r)
{
	/* ?nonce=<hex>: for each paired device, sha256hex(<its key's hash> ":" nonce), so an app can tell this
	   is its SuperStation before it sends its key */
	char nonce[65] = "";
	const char *q = strstr(r->path, "?nonce=");
	if (!q) q = strstr(r->path, "&nonce=");
	if (q) {
		q += 7;
		size_t n = 0;
		while (n < 64 && isxdigit((unsigned char)q[n])) { nonce[n] = (char)tolower((unsigned char)q[n]); n++; }
		nonce[n] = 0;
		if (n < 16 || (q[n] && q[n] != '&')) nonce[0] = 0;
	}
	cJSON *o = cJSON_CreateObject();
	cJSON_AddBoolToObject(o, "ok", 1);
	pthread_mutex_lock(&mu);
	cJSON_AddStringToObject(o, "name", server_name);
	cJSON_AddStringToObject(o, "frontend", "Console Mode");
	cJSON_AddStringToObject(o, "version", SS1REMOTE_VERSION);
	cJSON_AddNumberToObject(o, "api", API_VERSION);
	if (nonce[0]) {
		cJSON *pr = cJSON_AddArrayToObject(o, "proofs");
		for (int i = 0; i < ndevs; i++) {
			char in[65 + 1 + 64 + 1], h[65];
			int l = snprintf(in, sizeof(in), "%s:%s", devs[i].hash, nonce);
			sha256_hex(in, (size_t)l, h);
			cJSON_AddItemToArray(pr, cJSON_CreateString(h));
		}
	}
	pthread_mutex_unlock(&mu);
	respond_obj(r->fd, 200, o);
	cJSON_Delete(o);
}

static void handle(int fd, uint32_t ip)
{
	req_t *r = calloc(1, sizeof(*r));
	if (!r) { close(fd); return; }
	r->fd = fd;
	r->ip = ip;
	r->deadline_ms = now_ms() + HEAD_MS;
	struct timeval tv = { 15, 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	int e = read_head(r);
	if (e) { if (e > 0) respond_error(fd, e, "bad request"); goto done; }
	if (r->chunked) { respond_error(fd, 411, "send Content-Length"); goto done; }
	if (!strcmp(r->path, "/info") || !strncmp(r->path, "/info?", 6)) {
		if (strcmp(r->method, "GET")) respond_error(fd, 405, "GET");
		else do_info(r);
		goto done;
	}
	int is_api = !strcmp(r->path, "/api"), is_pair = !strcmp(r->path, "/pair"), is_art = !strncmp(r->path, "/art/", 5);
	int is_font = !strncmp(r->path, "/font/", 6);
	if (!is_api && !is_pair && !is_art && !is_font) { respond_error(fd, 404, "no such path"); goto done; }
	if (is_font && strcmp(r->method, "GET")) { respond_error(fd, 405, "GET"); goto done; }
	if ((is_api || is_pair) && strcmp(r->method, "POST")) { respond_error(fd, 405, "POST"); goto done; }
	if (is_art && strcmp(r->method, "GET") && strcmp(r->method, "PUT")) { respond_error(fd, 405, "GET or PUT"); goto done; }
	int kind = is_pair ? F_CODE : F_KEY, wait = locked_for(ip, kind);
	if (wait) {
		char m[96];
		snprintf(m, sizeof(m), "too many wrong %s: try again in %d s", is_pair ? "codes" : "keys", wait);
		respond_error(fd, 429, m);
		goto done;
	}
	if (!is_pair) {
		char who[64];
		if (authorize(r, who, sizeof(who)) < 0) {
			if (r->auth[0]) failed(ip, F_KEY);
			respond_error(fd, 401, "pair this device first (Pair a device on the SuperStation)");
			goto done;
		}
		succeeded(ip, F_KEY);
	}
	if (is_font) { do_font(r); goto done; }
	if (is_art) {
		long id = art_id(r->path);
		if (id < 0) respond_error(fd, 404, "no such game");
		else if (!strcmp(r->method, "GET")) do_art_get(r, id);
		else if (r->clen < 0) respond_error(fd, 411, "send Content-Length");
		else do_art_put(r, id);
		goto done;
	}
	if (r->clen < 0) { respond_error(fd, 411, "send Content-Length"); goto done; }
	if (r->clen > (is_pair ? PAIR_BODY_MAX : API_BODY_MAX)) { respond_error(fd, 413, "request too big"); goto done; }
	r->deadline_ms = now_ms() + (is_pair ? PAIR_BODY_MS : 30000);
	char *body = malloc((size_t)r->clen + 1);
	if (!body || read_body(r, body, -1)) { free(body); goto done; }
	if (is_pair) do_pair(r, body);
	else do_api(r, body);
	free(body);
done:
	free(r);
	close(fd);
}

/* ---------- the listener ---------- */

static int open_listener(int p)
{
	int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (s < 0) return -1;
	int one = 1;
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)p), .sin_addr.s_addr = htonl(INADDR_ANY) };
	if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(s, 8) < 0) {
		pthread_mutex_lock(&mu);
		snprintf(err_msg, sizeof(err_msg), "port %d: %s", p, strerror(errno));
		pthread_mutex_unlock(&mu);
		close(s);
		return -1;
	}
	return s;
}

static const char BUSY_MSG[] = "HTTP/1.1 503 Service Unavailable\r\nContent-Type: application/json\r\nContent-Length: 39\r\n"
                               "Retry-After: 1\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n"
                               "{\"ok\":false,\"error\":\"busy: try again\"}\n";

/* too many connections: "busy" once the request has started to come in, and the rest read before closing,
   so the client gets the answer rather than a reset (phosphord 0.27.60) */
static void *drain_run(void *arg)
{
	int fd = (int)(intptr_t)arg;
	char junk[16384];
	struct pollfd pf = { fd, POLLIN, 0 };
	poll(&pf, 1, 2000);
	if (send(fd, BUSY_MSG, sizeof(BUSY_MSG) - 1, MSG_DONTWAIT | MSG_NOSIGNAL) >= 0) {
		shutdown(fd, SHUT_WR);
		long got = 0;
		long long until = now_ms() + 10000;
		while (got < 16L * 1024 * 1024) {
			long long left = until - now_ms();
			if (left <= 0) break;
			pf.revents = 0;
			if (poll(&pf, 1, (int)(left > 1000 ? 1000 : left)) <= 0) continue;
			ssize_t n = recv(fd, junk, sizeof(junk), MSG_DONTWAIT);
			if (n <= 0) break;
			got += n;
		}
	}
	close(fd);
	return NULL;
}

typedef struct { int fd; uint32_t ip; } conn_t;

static void *conn_run(void *arg)
{
	conn_t c = *(conn_t *)arg;
	free(arg);
	handle(c.fd, c.ip);
	conn_drop(c.ip);
	return NULL;
}

static void spawn(void *(*fn)(void *), void *arg)
{
	pthread_t t;
	pthread_attr_t at;
	pthread_attr_init(&at);
	pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&at, 256 * 1024);
	if (pthread_create(&t, &at, fn, arg)) {
		if (fn == conn_run) { conn_t *c = arg; close(c->fd); conn_drop(c->ip); free(c); }
		else close((int)(intptr_t)arg);
	}
	pthread_attr_destroy(&at);
}

static void *http_run(void *arg)
{
	(void)arg;
	int lfd = -1, lport = 0;
	long retry_at = 0;
	for (;;) {
		pthread_mutex_lock(&mu);
		int want = on, p = port;
		pthread_mutex_unlock(&mu);
		if ((!want || p != lport) && lfd >= 0) { close(lfd); lfd = -1; plog("stopped listening"); }
		if (want && lfd < 0 && time(NULL) >= retry_at) {
			lfd = open_listener(p);
			if (lfd >= 0) { lport = p; plog("listening on port %d", p); }
			else retry_at = time(NULL) + 10;
		}
		pthread_mutex_lock(&mu);
		listening = lfd >= 0;
		pthread_mutex_unlock(&mu);
		if (lfd < 0) { poll(NULL, 0, 1000); continue; }
		struct pollfd pf = { lfd, POLLIN, 0 };
		if (poll(&pf, 1, 1000) <= 0) continue;
		struct sockaddr_in peer;
		socklen_t pl = sizeof(peer);
		int fd = accept4(lfd, (struct sockaddr *)&peer, &pl, SOCK_CLOEXEC);
		if (fd < 0) continue;
		if (peer.sin_family != AF_INET || !local_peer(&peer)) { close(fd); continue; }
		uint32_t ip = peer.sin_addr.s_addr;
		if (!conn_take(ip)) { spawn(drain_run, (void *)(intptr_t)fd); continue; }
		conn_t *c = malloc(sizeof(*c));
		if (!c) { conn_drop(ip); close(fd); continue; }
		c->fd = fd;
		c->ip = ip;
		spawn(conn_run, c);
	}
	return NULL;
}

/* ---------- the control socket, for Console Mode's screens ---------- */

static void addresses(FILE *o)
{
	struct ifaddrs *ifa, *i;
	if (getifaddrs(&ifa) != 0) return;
	for (i = ifa; i; i = i->ifa_next) {
		if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK) || !(i->ifa_flags & IFF_UP)) continue;
		char a[INET_ADDRSTRLEN];
		inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, a, sizeof(a));
		fprintf(o, "address=%s:%d\n", a, port);
	}
	freeifaddrs(ifa);
}

static void ctl_info(FILE *o)
{
	pthread_mutex_lock(&mu);
	long now = time(NULL);
	if (code[0] && now >= code_until) code[0] = 0;
	fprintf(o, "on=%d\nport=%d\nlistening=%d\nerror=%s\n", on, port, listening, on && !listening ? err_msg : "");
	fprintf(o, "code=%s\ncode_left=%ld\npaired=%s\n", code, code[0] ? code_until - now : 0, paired_name);
	fprintf(o, "devices=%d\n", ndevs);
	for (int i = 0; i < ndevs; i++)
		fprintf(o, "device=%.16s %ld %ld %s\n", devs[i].hash, devs[i].created, devs[i].last_seen, devs[i].name);
	addresses(o);
	pthread_mutex_unlock(&mu);
}

static void ctl_line(char *line, FILE *o)
{
	char *arg = strchr(line, ' ');
	if (arg) *arg++ = 0;
	if (!strcmp(line, "info")) { ctl_info(o); return; }
	if (!strcmp(line, "on") || !strcmp(line, "off")) {
		pthread_mutex_lock(&mu);
		on = !strcmp(line, "on");
		if (!on) code[0] = 0;
		int bad = save();
		pthread_mutex_unlock(&mu);
		plog("app link %s", on ? "on" : "off");
		fprintf(o, "result=%s\n", bad ? "error" : "ok");
		if (!bad) { poll(NULL, 0, 300); ctl_info(o); }
		return;
	}
	if (!strcmp(line, "pair")) {
		uint32_t r;
		if (random_bytes(&r, sizeof(r))) { fprintf(o, "result=error\n"); return; }
		pthread_mutex_lock(&mu);
		snprintf(code, sizeof(code), "%06u", r % 1000000u);
		code_until = time(NULL) + PAIR_SECONDS;
		code_tries = 0;
		paired_name[0] = 0;
		pthread_mutex_unlock(&mu);
		fprintf(o, "result=ok\n");
		ctl_info(o);
		return;
	}
	if (!strcmp(line, "pair_stop")) {
		pthread_mutex_lock(&mu);
		code[0] = 0;
		pthread_mutex_unlock(&mu);
		fprintf(o, "result=ok\n");
		return;
	}
	if (!strcmp(line, "forget") && arg && strlen(arg) >= 8) {
		int done = 0;
		pthread_mutex_lock(&mu);
		for (int i = 0; i < ndevs && !done; i++)
			if (!strncmp(devs[i].hash, arg, strlen(arg))) {
				plog("forgot %s", devs[i].name);
				memmove(&devs[i], &devs[i + 1], (size_t)(ndevs - i - 1) * sizeof(devs[0]));
				ndevs--;
				save();
				done = 1;
			}
		pthread_mutex_unlock(&mu);
		fprintf(o, "result=%s\n", done ? "ok" : "no such device");
		return;
	}
	if (!strcmp(line, "rescan")) {
		cJSON *q = cJSON_CreateObject();
		cJSON_AddStringToObject(q, "cmd", "rescan");
		cJSON_Delete(cm_api(q));
		cJSON_Delete(q);
		fprintf(o, "result=ok\n");
		return;
	}
	fprintf(o, "result=unknown command (info, on, off, pair, pair_stop, forget <id>, rescan)\n");
}

static void *ctl_conn(void *arg)
{
	int fd = (int)(intptr_t)arg;
	struct timeval tv = { 3, 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	char line[256];
	size_t n = 0;
	while (n < sizeof(line) - 1) {
		ssize_t k = recv(fd, line + n, sizeof(line) - 1 - n, 0);
		if (k <= 0) break;
		n += (size_t)k;
		if (memchr(line, '\n', n)) break;
	}
	line[n] = 0;
	line[strcspn(line, "\r\n")] = 0;
	char *out = NULL;
	size_t on_ = 0;
	FILE *o = open_memstream(&out, &on_);
	if (o) {
		ctl_line(line, o);
		fclose(o);
		send_all(fd, out, on_);
		free(out);
	}
	close(fd);
	return NULL;
}

static int ctl_listen(void)
{
	int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (s < 0) return -1;
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", ctl_path);
	/* one copy only: another one answering means this one isn't needed */
	if (connect(s, (struct sockaddr *)&a, sizeof(a)) == 0) { close(s); return -2; }
	close(s);
	s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	unlink(ctl_path);
	if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(s, 4) < 0) { close(s); return -1; }
	chmod(ctl_path, 0600);
	return s;
}

/* ---------- main ---------- */

static int client(const char *cmd)
{
	int s = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", ctl_path);
	if (s < 0 || connect(s, (struct sockaddr *)&a, sizeof(a))) { fprintf(stderr, "ss1remote isn't running\n"); return 1; }
	send_all(s, cmd, strlen(cmd));
	send_all(s, "\n", 1);
	char buf[4096];
	ssize_t n;
	while ((n = recv(s, buf, sizeof(buf), 0)) > 0) fwrite(buf, 1, (size_t)n, stdout);
	close(s);
	return 0;
}

/* ss1remote boot on|off: started at boot from MiSTer's linux/user-startup.sh, not only by Console Mode, so the
   app can still reach the SS1 (and roll an update back) when an update broke Console Mode */
#define BOOT_BEGIN "# ss1remote: the app link (Console Mode's PC and phone apps) - begin"
#define BOOT_END   "# ss1remote: end"
static int boot_cmd(const char *how)
{
	const char *us = getenv("SS1_USER_STARTUP");
	if (!us || !*us) us = "/media/fat/linux/user-startup.sh";
	char *old = NULL;
	size_t n = 0;
	FILE *f = fopen(us, "r");
	if (f) {
		fseek(f, 0, SEEK_END);
		long l = ftell(f);
		fseek(f, 0, SEEK_SET);
		old = calloc(1, (size_t)(l > 0 ? l : 0) + 1);
		if (old && l > 0) n = fread(old, 1, (size_t)l, f);
		fclose(f);
	}
	if (!old) old = calloc(1, 1);
	old[n] = 0;
	/* the file without our block */
	char *b = strstr(old, BOOT_BEGIN), *e = b ? strstr(b, BOOT_END) : NULL;
	if (b && e) { e += strlen(BOOT_END); if (*e == '\n') e++; memmove(b, e, strlen(e) + 1); }
	int on_ = !strcmp(how, "on");
	if (!on_ && !(b && e)) { printf("not set up at boot\n"); free(old); return 0; }
	char bak[700];
	snprintf(bak, sizeof(bak), "%s.ss1remote-backup", us);
	if (f && access(bak, F_OK)) {
		FILE *k = fopen(bak, "w");
		if (k) { fputs(old, k); fclose(k); }
	}
	char tmp[700];
	snprintf(tmp, sizeof(tmp), "%s.new", us);
	FILE *w = fopen(tmp, "w");
	if (!w) { perror(tmp); free(old); return 1; }
	if (!*old) fputs("#!/bin/sh\n", w);
	fputs(old, w);
	if (on_) {
		if (*old && old[strlen(old) - 1] != '\n') fputc('\n', w);
		fprintf(w, "%s\n[ -x %s/ss1remote ] && setsid %s/ss1remote serve < /dev/null > /dev/null 2>&1 &\n%s\n", BOOT_BEGIN, home, home, BOOT_END);
	}
	int bad = fflush(w) || fsync(fileno(w));
	bad |= fclose(w);
	if (bad || rename(tmp, us)) { perror(us); unlink(tmp); free(old); return 1; }
	chmod(us, 0755);
	printf(on_ ? "starts at boot (%s)\n" : "no longer started at boot (%s)\n", us);
	free(old);
	return 0;
}

static void usage(void)
{
	fprintf(stderr, "ss1remote %s: the app link (Console Mode's library for the phone and PC apps)\n"
	                "  ss1remote serve           run (normally started at boot or by Console Mode)\n"
	                "  ss1remote info|on|off|pair|pair_stop|rescan\n"
	                "  ss1remote forget <id>\n"
	                "  ss1remote boot on|off       start it at boot (MiSTer's linux/user-startup.sh) or not\n"
	                "  ss1remote api '<json>'     one command, without the network (testing)\n", SS1REMOTE_VERSION);
}

int main(int argc, char **argv)
{
	const char *h = getenv("SS1_HOME");
	if (h && *h) home = h;
	const char *c = getenv("SS1REMOTE_SOCK");
	if (c && *c) ctl_path = c;
	const char *cd = getenv("SS1_CM_DIR");
	if (cd && *cd) cm_dir = cd;
	/* testing: a pretend SD card and USB drive under one folder */
	const char *tr = getenv("SS1_TEST_ROOT");
	static char tfat[600], tusb[600], tcm[700];
	static const char *tdrives[3];
	if (tr && *tr) {
		snprintf(tfat, sizeof(tfat), "%s/fat", tr);
		snprintf(tusb, sizeof(tusb), "%s/usb0", tr);
		snprintf(tcm, sizeof(tcm), "%s/fat/ConsoleMode", tr);
		tdrives[0] = tfat; tdrives[1] = tusb; tdrives[2] = NULL;
		cm_fat = tfat; cm_dir = tcm;
		memcpy((void *)cm_drives, tdrives, sizeof(tdrives));
	}
	snprintf(ini_file, sizeof(ini_file), "%s/remote.ini", home);
	snprintf(log_file, sizeof(log_file), "%s/ss1remote.log", home);
	if (argc < 2) { usage(); return 2; }
	if (!strcmp(argv[1], "api") && argc == 3) {
		cJSON *q = cJSON_Parse(argv[2]);
		if (!q) { fprintf(stderr, "not JSON\n"); return 2; }
		jobs_boot();
		cJSON *o = jobs_api(q);
		if (!o) o = tools_api(q);
		if (!o) o = cm_api(q);
		char *s = cJSON_Print(o);
		puts(s);
		free(s);
		cJSON_Delete(o);
		cJSON_Delete(q);
		return 0;
	}
	if (!strcmp(argv[1], "boot") && argc == 3 && (!strcmp(argv[2], "on") || !strcmp(argv[2], "off"))) return boot_cmd(argv[2]);
	if (strcmp(argv[1], "serve")) {
		char cmd[200];
		snprintf(cmd, sizeof(cmd), "%s%s%s", argv[1], argc > 2 ? " " : "", argc > 2 ? argv[2] : "");
		if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) { usage(); return 0; }
		return client(cmd);
	}
	signal(SIGPIPE, SIG_IGN);
	load();
	int cs = ctl_listen();
	if (cs == -2) return 0;              /* already running */
	if (cs < 0) { fprintf(stderr, "ss1remote: can't open %s\n", ctl_path); return 1; }
	plog("ss1remote %s started (%s)", SS1REMOTE_VERSION, on ? "on" : "off");
	jobs_boot();
	pthread_t t;
	pthread_create(&t, NULL, http_run, NULL);
	for (;;) {
		struct pollfd pf = { cs, POLLIN, 0 };
		if (poll(&pf, 1, 30000) <= 0) { ua_tick(); continue; }
		int fd = accept4(cs, NULL, NULL, SOCK_CLOEXEC);
		if (fd < 0) { if (errno == EINTR) continue; poll(NULL, 0, 200); continue; }
		spawn(ctl_conn, (void *)(intptr_t)fd);
	}
}
