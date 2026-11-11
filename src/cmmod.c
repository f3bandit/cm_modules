/* cmmod: see include/cmmod.h.
 * Copyright (C) 2026 f3bandit. All rights reserved; licensed to Retro Remake (see LICENSE.md).
 * The controller tester and the memory card manager are f3bandit's idea and design. */
#include "cmmod.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static void sleep_ms(int ms)
{
	struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L };
	while (nanosleep(&t, &t) && errno == EINTR) {}
}

/* ------------------------------------------------------------------ MiSTer */

int cmod_core_installed(const char *prefix, char *path, size_t n)
{
	DIR *d = opendir(CMOD_FAT "/_Utility");
	if (!d) return -1;
	char best[256] = "";
	struct dirent *e;
	size_t pl = strlen(prefix);
	while ((e = readdir(d))) {
		size_t l = strlen(e->d_name);
		if (strncmp(e->d_name, prefix, pl) || l < 5 || strcasecmp(e->d_name + l - 4, ".rbf")) continue;
		if (strcmp(e->d_name, best) > 0) snprintf(best, sizeof best, "%s", e->d_name);   /* <prefix><yyyymmdd>.rbf: newest last */
	}
	closedir(d);
	if (!best[0]) return -1;
	snprintf(path, n, "%s/_Utility/%s", CMOD_FAT, best);
	return 0;
}

void cmod_core_running(char *name, size_t n)
{
	name[0] = 0;
	FILE *f = fopen("/tmp/CORENAME", "r");
	if (!f) return;
	if (fgets(name, (int)n, f)) name[strcspn(name, "\r\n")] = 0;
	fclose(f);
}

int cmod_load(const char *what)
{
	/* the pipe blocks when nothing reads it: never wait for MiSTer for more than a few seconds */
	int fd = -1;
	for (int i = 0; i < 30 && fd < 0; i++) {
		fd = open("/dev/MiSTer_cmd", O_WRONLY | O_NONBLOCK);
		if (fd < 0) sleep_ms(100);
	}
	if (fd < 0) return -1;
	char cmd[600];
	int l = snprintf(cmd, sizeof cmd, "load_core %s\n", what);
	int r = write(fd, cmd, (size_t)l) == l ? 0 : -1;
	close(fd);
	return r;
}

int cmod_back_to_menu(const char *core)
{
	/* MiSTer can miss the command while it's still busy with the core (right after a write, say) */
	for (int t = 0; t < 4; t++) {
		sleep_ms(1000);
		if (cmod_load("menu.rbf")) continue;
		for (int i = 0; i < 8; i++) {
			char now[64];
			sleep_ms(1000);
			cmod_core_running(now, sizeof now);
			if (!core || strcasecmp(now, core)) return 0;
		}
	}
	return -1;
}

/* ------------------------------------------------------------------ memory cards */

void cmod_mc_request(uint8_t req[CMOD_MC_JOB_SIZE], int job, int flags, uint32_t id, const int *force, int nforce)
{
	memset(req, 0, CMOD_MC_JOB_SIZE);
	memcpy(req, "SMCQ", 4);
	req[4] = (uint8_t)job;
	req[6] = (uint8_t)flags;
	put32(req + 8, id);
	for (int i = 0; i < nforce; i++)
		if (force[i] >= 0 && force[i] < 1024) req[512 + force[i]] = 1;
}

int cmod_mc_parse(const uint8_t *b, size_t len, uint32_t id, cmod_mc_report *r)
{
	if (len < CMOD_MC_JOB_SIZE || memcmp(b, "SMC", 3)) return -1;
	if (b[3] != 'P' && b[3] != 'R') return 0;              /* still our request ('Q') */
	if (le32(b + 8) != id) return 0;                       /* an older job's report */
	memset(r, 0, sizeof *r);
	r->finished = b[3] == 'R';
	r->version = b[4]; r->job = b[5]; r->result = b[6]; r->card = b[7];
	r->id = id;
	r->ok = le16(b + 12); r->retried = le16(b + 14); r->failed = le16(b + 16); r->differ = le16(b + 18);
	r->retries = le16(b + 20); r->done = le16(b + 22);
	r->slow = b[24]; r->phase = b[25]; r->sector = le16(b + 26); r->written = le16(b + 28);
	for (int i = 0; i < 1024; i++) r->map[i] = b[512 + i] > 5 ? 0 : b[512 + i];
	return 1;
}

const char *cmod_mc_result_text(int job, int result)
{
	switch (result) {
	case CMOD_MC_OK: return job == CMOD_MC_VERIFY ? "The card matches the file" : job == CMOD_MC_BACKUP ? "Card backed up"
	                     : "The card already matched the file: nothing was written";
	case CMOD_MC_OK_RETRIED: return job == CMOD_MC_VERIFY ? "The card matches the file (some sectors needed retries)" : "Card backed up (some sectors needed retries)";
	case CMOD_MC_FAILED: return "Some sectors couldn't be read";
	case CMOD_MC_NO_CARD: return "No card in front port 1";
	case CMOD_MC_DIFFERS: return "The card and the file are different";
	case CMOD_MC_BAD_FILE: return "The card file isn't 128 KB";
	case CMOD_MC_READ_ONLY: return "The card file is read-only";
	case CMOD_MC_SAVED_FAILED: return "Saved, but some sectors failed";
	case CMOD_MC_WRITTEN: return "The card was updated and checked";
	default: return "Unknown result";
	}
}

/* ------------------------------------------------------------------ controllers */

void cmod_pad_request(uint8_t req[CMOD_PAD_REPORT_SIZE], uint32_t id)
{
	memset(req, 0, CMOD_PAD_REPORT_SIZE);
	memcpy(req, "SCTQ", 4);
	put32(req + 8, id);
}

int cmod_pad_parse(const uint8_t *b, size_t len, uint32_t id, cmod_pad_report *r)
{
	if (len < 80 || memcmp(b, "SCT", 3)) return -1;
	if (b[3] != 'R' || le32(b + 8) != id) return 0;
	memset(r, 0, sizeof *r);
	r->version = b[4];
	r->id = id;
	for (int p = 0; p < 2; p++) {
		const uint8_t *o = b + (p ? 48 : 32);
		cmod_pad_port *q = &r->port[p];
		q->present = o[1] != 0;
		q->id = o[2];
		q->analog = q->present && (o[2] & 0x0F) >= 3;
		q->buttons = le16(o + 3);
		q->rx = o[5]; q->ry = o[6]; q->lx = o[7]; q->ly = o[8];
		q->reads = o[9];
		q->card = o[10];
		q->analog_asked = o[11] != 0;
		if (!q->present) q->buttons = 0;
	}
	r->analog_switches = b[44];
	r->pins_port1 = b[64]; r->pins_port2 = b[65]; r->pins_sel6 = b[66];
	r->sel6_answered = b[67] != 0; r->sel6_id = b[68];
	return 1;
}

const char *cmod_pad_type(int id)
{
	switch (id >> 4) {
	case 4: return "Digital pad";
	case 7: return "Analog (DualShock)";
	case 5: return "Analog joystick";
	case 2: return "NeGcon";
	case 1: return "Mouse";
	case 6: return "Light gun";
	default: return "Other controller";
	}
}

const char *cmod_pad_button_name(uint16_t bit)
{
	switch (bit) {
	case CMOD_PAD_SELECT: return "Select"; case CMOD_PAD_L3: return "L3"; case CMOD_PAD_R3: return "R3";
	case CMOD_PAD_START: return "Start"; case CMOD_PAD_UP: return "Up"; case CMOD_PAD_RIGHT: return "Right";
	case CMOD_PAD_DOWN: return "Down"; case CMOD_PAD_LEFT: return "Left"; case CMOD_PAD_L2: return "L2";
	case CMOD_PAD_R2: return "R2"; case CMOD_PAD_L1: return "L1"; case CMOD_PAD_R1: return "R1";
	case CMOD_PAD_TRIANGLE: return "Triangle"; case CMOD_PAD_CIRCLE: return "Circle";
	case CMOD_PAD_CROSS: return "Cross"; case CMOD_PAD_SQUARE: return "Square";
	default: return "";
	}
}

/* ------------------------------------------------------------------ jobs */

int cmod_job_start(cmod_job job, const char *arg, int flags, const int *force, int nforce)
{
	static const char *names[] = { "backup", "verify", "write", "padtest" };
	if ((int)job < 0 || job > CMOD_JOB_PADTEST) return -1;
	if (job != CMOD_JOB_PADTEST && (!arg || !*arg)) return -1;
	char fl[16], fo[2048] = "";
	snprintf(fl, sizeof fl, "%d", flags);
	for (int i = 0, o = 0; i < nforce && o < (int)sizeof fo - 8; i++)
		o += snprintf(fo + o, sizeof fo - (size_t)o, i ? ",%d" : "%d", force[i]);
	pid_t p = fork();
	if (p < 0) return -1;
	if (p == 0) {
		/* the grandchild runs cmjobd in its own session, so it lives on when the front end is closed */
		if (fork() != 0) _exit(0);
		setsid();
		int nul = open("/dev/null", O_RDWR);
		if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); if (nul > 2) close(nul); }
		for (int fd = 3; fd < 256; fd++) close(fd);
		signal(SIGHUP, SIG_IGN);
		execl(CMOD_JOBD, "cmjobd", names[job], arg ? arg : "-", fl, fo[0] ? fo : "-", (char *)NULL);
		_exit(127);
	}
	int st;
	waitpid(p, &st, 0);
	return 0;
}

int cmod_kv(const char *text, const char *key, char *val, size_t n)
{
	size_t kl = strlen(key);
	for (const char *l = text; l && *l; ) {
		const char *e = strchr(l, '\n');
		size_t ll = e ? (size_t)(e - l) : strlen(l);
		if (ll > kl && !strncmp(l, key, kl) && l[kl] == '=') {
			size_t vl = ll - kl - 1;
			if (vl >= n) vl = n - 1;
			memcpy(val, l + kl + 1, vl);
			val[vl] = 0;
			return 0;
		}
		l = e ? e + 1 : NULL;
	}
	return -1;
}

int cmod_job_result(const char *key, char *val, size_t n)
{
	char buf[4096];
	FILE *f = fopen(CMOD_RESULT, "r");
	if (!f) return -1;
	size_t r = fread(buf, 1, sizeof buf - 1, f);
	fclose(f);
	buf[r] = 0;
	return cmod_kv(buf, key, val, n);
}

/* ------------------------------------------------------------------ drive repair */

int cmod_repair_run(const char *cmd, const char *arg, char *out, size_t n)
{
	static const char *ok[] = { "status", "setup", "arm", "disarm", "restart", "result", "remove", NULL };
	int known = 0;
	for (int i = 0; ok[i]; i++) if (!strcmp(cmd, ok[i])) known = 1;
	if (!known || n == 0) return -1;
	int pfd[2];
	if (pipe(pfd)) return -1;
	pid_t p = fork();
	if (p < 0) { close(pfd[0]); close(pfd[1]); return -1; }
	if (p == 0) {
		dup2(pfd[1], 1);
		int nul = open("/dev/null", O_RDWR);
		if (nul >= 0) { dup2(nul, 0); dup2(nul, 2); }
		close(pfd[0]); close(pfd[1]);
		execl("/bin/bash", "bash", CMOD_HOME "/sd_offline_repair.sh", cmd, arg ? arg : (char *)NULL, (char *)NULL);
		_exit(127);
	}
	close(pfd[1]);
	size_t got = 0;
	ssize_t r;
	while ((r = read(pfd[0], out + got, n - 1 - got)) > 0 && got < n - 1) got += (size_t)r;
	if (got == n - 1) { char sink[512]; while (read(pfd[0], sink, sizeof sink) > 0) {} }
	out[got] = 0;
	close(pfd[0]);
	int st;
	waitpid(p, &st, 0);
	return WIFEXITED(st) && WEXITSTATUS(st) != 127 ? 0 : -1;
}

/* ------------------------------------------------------------------ the app link */

int cmod_remote_cmd(const char *cmd, char *out, size_t n)
{
	if (!n) return -1;
	out[0] = 0;
	int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (s < 0) return -1;
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	const char *path = getenv("CMREMOTE_SOCK");
	snprintf(a.sun_path, sizeof a.sun_path, "%s", path && *path ? path : CMOD_REMOTE_SOCK);
	struct timeval tv = { 3, 0 };
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
	if (connect(s, (struct sockaddr *)&a, sizeof a)) { close(s); return -1; }
	char line[300];
	int l = snprintf(line, sizeof line, "%s\n", cmd);
	if (l <= 0 || l >= (int)sizeof line || send(s, line, (size_t)l, MSG_NOSIGNAL) != l) { close(s); return -1; }
	size_t got = 0;
	ssize_t k;
	while (got < n - 1 && (k = recv(s, out + got, n - 1 - got, 0)) > 0) got += (size_t)k;
	out[got] = 0;
	close(s);
	return got ? 0 : -1;
}

/* any app-link command as JSON ({"cmd":"wifi"} ...), its JSON reply in out; waits up to timeout_s */
int cmod_remote_api(const char *json, char *out, size_t n, int timeout_s)
{
	if (!n || !json) return -1;
	out[0] = 0;
	int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (s < 0) return -1;
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	const char *path = getenv("CMREMOTE_SOCK");
	snprintf(a.sun_path, sizeof a.sun_path, "%s", path && *path ? path : CMOD_REMOTE_SOCK);
	struct timeval tv = { timeout_s > 0 ? timeout_s : 10, 0 };
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
	if (connect(s, (struct sockaddr *)&a, sizeof a)) { close(s); return -1; }
	size_t l = strlen(json);
	if (send(s, "api ", 4, MSG_NOSIGNAL) != 4 || send(s, json, l, MSG_NOSIGNAL) != (ssize_t)l || send(s, "\n", 1, MSG_NOSIGNAL) != 1) { close(s); return -1; }
	size_t got = 0;
	ssize_t k;
	while (got < n - 1 && (k = recv(s, out + got, n - 1 - got, 0)) > 0) got += (size_t)k;
	out[got] = 0;
	close(s);
	if (got && out[got - 1] == '\n') out[got - 1] = 0;
	return got ? 0 : -1;
}

int cmod_remote_start(void)
{
	char tmp[64];
	if (!cmod_remote_cmd("info", tmp, sizeof tmp)) return 0;
	pid_t p = fork();
	if (p < 0) return -1;
	if (p == 0) {
		if (fork() != 0) _exit(0);
		setsid();
		int nul = open("/dev/null", O_RDWR);
		if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); if (nul > 2) close(nul); }
		for (int fd = 3; fd < 256; fd++) close(fd);
		signal(SIGHUP, SIG_IGN);
		execl(CMOD_REMOTE, "cmremote", "serve", (char *)NULL);
		_exit(127);
	}
	int st;
	waitpid(p, &st, 0);
	/* up in a moment */
	for (int i = 0; i < 20; i++) {
		if (!cmod_remote_cmd("info", tmp, sizeof tmp)) return 0;
		usleep(100 * 1000);
	}
	return -1;
}

/* ------------------------------------------------------------------ the front ports' card bus (cmmod.h) */

int cmod_mc_busy(char *holder, size_t n)
{
	char t[160];
	ssize_t l = readlink(CMOD_MC_BUSY, t, sizeof t - 1);
	if (holder && n) holder[0] = 0;
	if (l <= 0) return 0;
	t[l] = 0;
	long pid = atol(t);
	if (pid <= 0 || (kill((pid_t)pid, 0) && errno == ESRCH)) {   /* its owner is gone: free */
		unlink(CMOD_MC_BUSY);
		return 0;
	}
	const char *who = strchr(t, ' ');
	if (holder && n) snprintf(holder, n, "%s", who ? who + 1 : t);
	return 1;
}

int cmod_mc_lock(const char *who, int wait_ms, char *holder, size_t n)
{
	char t[160];
	snprintf(t, sizeof t, "%ld %s", (long)getpid(), who && *who ? who : "?");
	for (int waited = 0;; waited += 100) {
		if (!symlink(t, CMOD_MC_BUSY)) return 0;
		if (!cmod_mc_busy(holder, n)) continue;           /* a stale one was just cleared: try again at once */
		if (waited >= wait_ms) return 1;
		usleep(100000);
	}
}

void cmod_mc_unlock(void)
{
	char t[160];
	ssize_t l = readlink(CMOD_MC_BUSY, t, sizeof t - 1);
	if (l <= 0) return;
	t[l] = 0;
	if (atol(t) == (long)getpid()) unlink(CMOD_MC_BUSY);
}
