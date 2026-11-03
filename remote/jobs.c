/* cmremote: the long jobs and Update All's protection.
 * Copyright (C) 2026 f3bandit. GPL-3.0-or-later (see ../LICENSE); Retro Remake: see ../LICENSE-RETRO-REMAKE.md.
 *
 * update-guard (modules/update-guard) keeps restore points of MiSTer's Main, the menu
 * core, the kernel and Linux, and copies of the settings inside Linux. That's the only protection Update All gets
 * for Console Mode: no item is blocked. Jobs run one at a time, in their own session (they outlive a dropped
 * connection), their output kept in cmod_tmp()/job.out for the app to show while they run.
 *
 *   {"cmd":"backups"}                      the one folder every backup on the SuperStation goes in (tools/backups), by kind
 *   {"cmd":"job"}                          the current or last job: name, busy, ok, exit, at, output (the end of it)
 *   {"cmd":"guard"}                        Linux version, last update, settings reset by it, restore points
 *   {"cmd":"guard","do":"rollback","items":["<point>"]} | "delete" (points) | "restore" (files) | "keep" | "save_point"
 *   {"cmd":"ua",...}                       Update All's settings (ua.c); "do":"run" runs it through update-guard
 *
 * Protecting an Update All started anywhere (the TV's Scripts menu has no wrapper in Console Mode):
 *  - a few minutes after start, the system files as they are now are saved, unless that exact set is saved
 *    already (update-guard.sh current: checksums only, a copy only after something changed them);
 *  - while cmremote runs, a watcher looks for Update All, MiSTer's update.sh or the Downloader starting (every 5 s); the moment one
 *    does, the same is done straight away, before it gets to MiSTer's Main or Linux (it fetches its databases first). */
#define _GNU_SOURCE
#include "jobs.h"
#include "ua.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static char home_[600] = "/media/fat/ConsoleMode/tools";
static int home_set;
const char *cmod_home(void)
{
	if (!home_set) {
		const char *h = getenv("CMOD_HOME");
		if (h && *h) snprintf(home_, sizeof(home_), "%s", h);
		home_set = 1;
	}
	return home_;
}
const char *cmod_tmp(void)
{
	static char t[300];
	if (!t[0]) {
		const char *e = getenv("CMOD_TMP");
		snprintf(t, sizeof(t), "%s", e && *e ? e : "/tmp/cmremote");
		mkdir(t, 0755);
	}
	return t;
}
int file_exists(const char *p) { struct stat sb; return !stat(p, &sb); }

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static struct { char name[24]; pid_t pid; int busy, ok, code; time_t at; } job;

static void guard_script(char *o, size_t n) { snprintf(o, n, "%s/modules/update-guard/update-guard.sh", cmod_home()); }
static void out_path(char *o, size_t n) { snprintf(o, n, "%s/job.out", cmod_tmp()); }

/* update-guard's settings: everything it keeps lives under tools/guard, its settings in tools/update-guard.ini */
static void guard_env(void)
{
	char p[700];
	snprintf(p, sizeof(p), "%s/guard", cmod_home());
	setenv("CM_ROOT", p, 1);
	setenv("CM_TMP", "/tmp/cmguard", 1);
	snprintf(p, sizeof(p), "%s/update-guard.ini", cmod_home());
	setenv("UPDATE_GUARD_INI", p, 1);
	/* what it keeps goes with every other backup: tools/backups (restore points: POINTS_DIR in the ini) */
	snprintf(p, sizeof(p), "%s/backups/linux-settings", cmod_home());
	setenv("UPDATE_GUARD_SAVED", p, 1);
	snprintf(p, sizeof(p), "%s/backups/linux-settings-undo", cmod_home());
	setenv("UPDATE_GUARD_UNDO", p, 1);
	snprintf(p, sizeof(p), "%s/backups/update-guard", cmod_home());
	setenv("UPDATE_GUARD_BACKUPS", p, 1);
}

/* {"cmd":"backups"}: the one folder every backup on the SuperStation goes in, and what's in it */
static long long du(const char *p, int depth)
{
	struct stat sb;
	if (lstat(p, &sb)) return 0;
	if (!S_ISDIR(sb.st_mode)) return (long long)sb.st_size;
	if (depth > 8) return 0;
	DIR *d = opendir(p);
	if (!d) return 0;
	long long t = 0;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
		char c[1200];
		snprintf(c, sizeof(c), "%s/%s", p, e->d_name);
		t += du(c, depth + 1);
	}
	closedir(d);
	return t;
}
static cJSON *cmd_backups(void)
{
	char root[700];
	snprintf(root, sizeof(root), "%s/backups", cmod_home());
	mkdir(root, 0755);
	cJSON *o = cJSON_CreateObject();
	cJSON_AddBoolToObject(o, "ok", 1);
	cJSON_AddStringToObject(o, "folder", root);
	cJSON *arr = cJSON_AddArrayToObject(o, "kinds");
	static const char *const K[][2] = { { "restore-points", "MiSTer & Linux restore points" }, { "linux-settings", "Settings inside Linux" },
	                                   { "memory-cards", "Memory cards" }, { "saves", "Saves" }, { "controllers", "Controllers" } };
	long long all = 0;
	for (size_t i = 0; i < sizeof(K) / sizeof(K[0]); i++) {
		char p[900];
		snprintf(p, sizeof(p), "%s/%s", root, K[i][0]);
		long long b = du(p, 0);
		all += b;
		cJSON *x = cJSON_CreateObject();
		cJSON_AddStringToObject(x, "id", K[i][0]);
		cJSON_AddStringToObject(x, "name", K[i][1]);
		cJSON_AddStringToObject(x, "path", p);
		cJSON_AddBoolToObject(x, "exists", file_exists(p));
		cJSON_AddNumberToObject(x, "bytes", (double)b);
		cJSON_AddItemToArray(arr, x);
	}
	cJSON_AddNumberToObject(o, "bytes", (double)all);
	return o;
}

static void *reaper(void *a)
{
	pid_t pid = (pid_t)(intptr_t)a;
	int st = 0;
	while (waitpid(pid, &st, 0) < 0 && errno == EINTR) ;
	pthread_mutex_lock(&mu);
	job.busy = 0;
	job.code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
	job.ok = job.code == 0;
	job.at = time(NULL);
	plog("job %s finished (exit %d)", job.name, job.code);
	pthread_mutex_unlock(&mu);
	return NULL;
}

/* runs argv as the one job (its output in job.out); guard: with update-guard's settings in the environment */
static const char *job_exec(const char *name, const char *const *argv, int guard)
{
	char out[400];
	pthread_mutex_lock(&mu);
	if (job.busy) { pthread_mutex_unlock(&mu); return "Busy with the last job; try again when it's done"; }
	out_path(out, sizeof(out));
	pid_t p = fork();
	if (p < 0) { pthread_mutex_unlock(&mu); return "couldn't start it"; }
	if (p == 0) {
		setsid();
		int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		int nul = open("/dev/null", O_RDONLY);
		if (nul >= 0) dup2(nul, 0);
		if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); }
		for (int i = 3; i < 256; i++) close(i);
		signal(SIGPIPE, SIG_DFL);
		if (guard) guard_env();
		setenv("TERM", "dumb", 1);
		execvp(argv[0], (char *const *)argv);
		_exit(127);
	}
	snprintf(job.name, sizeof(job.name), "%s", name);
	job.pid = p;
	job.busy = 1;
	job.ok = 0;
	job.code = -1;
	job.at = time(NULL);
	pthread_t t;
	if (pthread_create(&t, NULL, reaper, (void *)(intptr_t)p) == 0) pthread_detach(t);
	pthread_mutex_unlock(&mu);
	plog("job %s started", name);
	return NULL;
}

const char *job_start(const char *name, const char *arg0, const char *const *args, int nargs)
{
	char script[700];
	guard_script(script, sizeof(script));
	if (!file_exists(script)) return "update-guard isn't installed with the app link (tools/modules/update-guard)";
	const char *argv[24];
	int a = 0;
	argv[a++] = "bash";
	argv[a++] = script;
	argv[a++] = arg0;
	for (int i = 0; i < nargs && a < 22; i++) argv[a++] = args[i];
	argv[a] = NULL;
	return job_exec(name, argv, 1);
}

const char *job_run(const char *name, const char *const *argv) { return job_exec(name, argv, 0); }

/* the end of a file (up to max bytes), from the start of a line */
static char *tail_of(const char *p, long max)
{
	FILE *f = fopen(p, "rb");
	if (!f) return strdup("");
	fseek(f, 0, SEEK_END);
	long n = ftell(f), from = n > max ? n - max : 0;
	fseek(f, from, SEEK_SET);
	char *b = malloc((size_t)(n - from) + 1);
	size_t got = b ? fread(b, 1, (size_t)(n - from), f) : 0;
	fclose(f);
	if (!b) return strdup("");
	b[got] = 0;
	char *s = b;
	if (from) { char *nl = strchr(b, '\n'); if (nl) s = nl + 1; }
	/* a terminal's carriage returns (progress lines): keep the last state of each line */
	char *o = malloc(strlen(s) + 1), *w = o, *line = w;
	for (char *c = s; *c; c++) {
		if (*c == '\r') { if (c[1] != '\n') w = line; continue; }
		if (*c == '\033') { /* colours and cursor moves: dropped */
			c++;
			if (*c == '[') { while (c[1] && !((c[1] >= '@' && c[1] <= '~'))) c++; if (c[1]) c++; }
			continue;
		}
		*w++ = *c;
		if (*c == '\n') line = w;
	}
	*w = 0;
	free(b);
	return o;
}

static cJSON *job_json(void)
{
	cJSON *o = cJSON_CreateObject();
	pthread_mutex_lock(&mu);
	cJSON_AddStringToObject(o, "name", job.name);
	cJSON_AddBoolToObject(o, "busy", job.busy);
	cJSON_AddBoolToObject(o, "ok", job.ok);
	cJSON_AddNumberToObject(o, "exit", job.code);
	cJSON_AddNumberToObject(o, "at", (double)job.at);
	pthread_mutex_unlock(&mu);
	char p[400];
	out_path(p, sizeof(p));
	char *t = tail_of(p, 16000);
	cJSON_AddStringToObject(o, "output", t);
	free(t);
	return o;
}

/* update-guard.sh ui-state: its key=value lines */
char *run_capture(const char *const *argv, int limit)
{
	int pp[2];
	if (pipe(pp)) return NULL;
	pid_t p = fork();
	if (p < 0) { close(pp[0]); close(pp[1]); return NULL; }
	if (p == 0) {
		dup2(pp[1], 1);
		int nul = open("/dev/null", O_RDWR);
		if (nul >= 0) { dup2(nul, 0); dup2(nul, 2); }
		for (int i = 3; i < 256; i++) close(i);
		guard_env();
		alarm((unsigned)limit);
		execvp(argv[0], (char *const *)argv);
		_exit(127);
	}
	close(pp[1]);
	size_t cap = 8192, len = 0;
	char *b = malloc(cap);
	ssize_t k;
	while (b && (k = read(pp[0], b + len, cap - len - 1)) > 0) {
		len += (size_t)k;
		if (len + 1024 > cap) { char *nb = realloc(b, cap * 2); if (!nb) break; b = nb; cap *= 2; }
	}
	close(pp[0]);
	waitpid(p, NULL, 0);
	if (b) b[len] = 0;
	return b;
}

static int arg_ok(const char *v)
{
	if (!*v || strlen(v) > 200 || v[0] == '-') return 0;
	for (const char *c = v; *c; c++) if ((unsigned char)*c < 32 || strchr("`$;&|<>\"'\\", *c)) return 0;
	return 1;
}

static cJSON *fail(const char *m)
{
	cJSON *o = cJSON_CreateObject();
	cJSON_AddBoolToObject(o, "ok", 0);
	cJSON_AddStringToObject(o, "error", m);
	return o;
}

static cJSON *cmd_guard(cJSON *req)
{
	char script[700];
	guard_script(script, sizeof(script));
	const char *d = cJSON_GetStringValue(cJSON_GetObjectItem(req, "do"));
	if (d && *d) {
		const char *args[16];
		int n = 0;
		cJSON *it;
		cJSON_ArrayForEach(it, cJSON_GetObjectItem(req, "items")) {
			const char *v = cJSON_GetStringValue(it);
			if (!v || !arg_ok(v) || n >= 16) return fail("that item isn't allowed");
			/* settings files are /etc/... or /root/... (no ..); points are plain names */
			if (!strcmp(d, "restore") ? ((strncmp(v, "/etc/", 5) && strncmp(v, "/root/", 6)) || strstr(v, "..")) : strchr(v, '/') != NULL)
				return fail("that item isn't allowed");
			args[n++] = v;
		}
		const char *why = NULL;
		if (!strcmp(d, "rollback")) { if (n != 1) return fail("rollback: one restore point"); why = job_start("rollback", "rollback", args, 1); }
		else if (!strcmp(d, "delete")) { if (!n) return fail("delete: the restore points"); why = job_start("delete", "delete-point", args, n); }
		else if (!strcmp(d, "restore")) { if (!n) return fail("restore: the settings files"); why = job_start("restore", "restore", args, n); }
		else if (!strcmp(d, "keep")) why = job_start("keep", "keep", NULL, 0);
		else if (!strcmp(d, "save_point")) { const char *l[] = { "saved-from-the-app" }; why = job_start("save_point", "save-point", l, 1); }
		else if (!strcmp(d, "linux_guard_on") || !strcmp(d, "linux_guard_off")) {
			/* the Linux guard: quick (downloader.ini and update-guard.ini), done before answering */
			const char *argv[] = { "bash", script, "guard", !strcmp(d, "linux_guard_on") ? "on" : "off", NULL };
			char *out = run_capture(argv, 20);
			plog("update-guard: %s", out ? out : d);
			free(out);
		}
		else return fail("do: rollback (items: one point), delete (items: points), restore (items: files), keep, save_point, linux_guard_on or linux_guard_off");
		if (why) return fail(why);
	}
	cJSON *o = cJSON_CreateObject();
	cJSON_AddBoolToObject(o, "ok", 1);
	cJSON_AddBoolToObject(o, "installed", file_exists(script));
	cJSON *files = cJSON_AddArrayToObject(o, "files"), *points = cJSON_AddArrayToObject(o, "points");
	const char *argv[] = { "bash", script, "ui-state", NULL };
	char *ui = file_exists(script) ? run_capture(argv, 20) : NULL;
	char *save = NULL;
	for (char *line = ui ? strtok_r(ui, "\n", &save) : NULL; line; line = strtok_r(NULL, "\n", &save)) {
		char *eq = strchr(line, '=');
		if (!eq) continue;
		*eq = 0;
		char *v = eq + 1;
		if (!strcmp(line, "file")) {
			char *bar = strchr(v, '|');
			if (!bar) continue;
			*bar = 0;
			cJSON *f = cJSON_CreateObject();
			cJSON_AddStringToObject(f, "path", v);
			cJSON_AddBoolToObject(f, "reset", bar[1] == '1');
			cJSON_AddItemToArray(files, f);
		} else if (!strcmp(line, "point")) {
			char *fld[5] = { v, "", "", "", "" };
			for (int k = 1; k < 5; k++) { char *b = strchr(fld[k - 1], '|'); if (!b) break; *b = 0; fld[k] = b + 1; }
			cJSON *pt = cJSON_CreateObject();
			cJSON_AddStringToObject(pt, "name", fld[0]);
			cJSON_AddStringToObject(pt, "label", fld[1]);
			cJSON_AddStringToObject(pt, "created", fld[2]);
			cJSON_AddStringToObject(pt, "linux", fld[3]);
			cJSON_AddBoolToObject(pt, "complete", !strcmp(fld[4], "1") || !strcmp(fld[4], "yes") || !strcmp(fld[4], "true"));
			cJSON_AddItemToArray(points, pt);
		} else if (!strcmp(line, "awaiting")) cJSON_AddBoolToObject(o, "awaiting", !strcmp(v, "1"));
		else cJSON_AddStringToObject(o, line, v);
	}
	free(ui);
	cJSON_AddItemToObject(o, "job", job_json());
	return o;
}

cJSON *jobs_api(cJSON *req)
{
	const char *cmd = cJSON_GetStringValue(cJSON_GetObjectItem(req, "cmd"));
	if (!cmd) return NULL;
	if (!strcmp(cmd, "job")) { cJSON *o = cJSON_CreateObject(); cJSON_AddBoolToObject(o, "ok", 1); cJSON_AddItemToObject(o, "job", job_json()); return o; }
	if (!strcmp(cmd, "guard")) return cmd_guard(req);
	if (!strcmp(cmd, "backups")) return cmd_backups();
	if (!strcmp(cmd, "ua")) {
		const char *err = NULL;
		cJSON *o = ua_cmd(req, &err);
		if (!o) return fail(err ? err : "Update All didn't answer");
		if (!cJSON_GetObjectItem(o, "ok")) cJSON_AddBoolToObject(o, "ok", 1);
		/* the run: its job, for the app to follow */
		cJSON_AddItemToObject(o, "job", job_json());
		return o;
	}
	return NULL;
}

/* an Update All or a Downloader running (anyone's, not only ours) */
static int updater_running(void)
{
	DIR *d = opendir("/proc");
	if (!d) return 0;
	struct dirent *e;
	int found = 0;
	while (!found && (e = readdir(d))) {
		if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
		char p[300], buf[1024];
		snprintf(p, sizeof(p), "/proc/%s/cmdline", e->d_name);
		int fd = open(p, O_RDONLY);
		if (fd < 0) continue;
		ssize_t n = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if (n <= 0) continue;
		for (ssize_t i = 0; i < n; i++) if (!buf[i]) buf[i] = ' ';
		buf[n] = 0;
		/* Update All (Scripts/update_all.sh, /tmp/update_all.sh, its .pyz) and MiSTer's own updater
		   (Scripts/update.sh, which runs /tmp/downloader.sh: the Downloader as a zip or its downloader_bin) */
		static const char *const UPDATERS[] = { "update_all.sh", "update_all.pyz", "Scripts/update.sh", "downloader.sh",
		                                        "downloader_latest.zip", "downloader_bin", "downloader.zip", NULL };
		for (int k = 0; UPDATERS[k] && !found; k++) if (strstr(buf, UPDATERS[k])) found = 1;
	}
	closedir(d);
	return found;
}

static pthread_mutex_t cur_mu = PTHREAD_MUTEX_INITIALIZER;
/* update-guard.sh current: one at a time (the watcher and the after-boot one can meet) */
static void save_current(int quiet, const char *why)
{
	char script[700];
	guard_script(script, sizeof(script));
	if (!file_exists(script) || pthread_mutex_trylock(&cur_mu)) return;
	plog("restore point: checking the system files (%s)", why);
	const char *nice_[] = { "nice", "-n", "19", "bash", script, "current", NULL }, *now[] = { "bash", script, "current", NULL };
	char *o = run_capture(quiet ? nice_ : now, 1800);
	if (o) { o[strcspn(o, "\n")] = 0; plog("restore point: %s", o); }
	free(o);
	pthread_mutex_unlock(&cur_mu);
}

static void *boot_run(void *a)
{
	(void)a;
	char script[700];
	/* NVMe TRIM: switched on the way that works, and its schedule (scripts/cmtrim.sh) */
	snprintf(script, sizeof(script), "%s/cmtrim.sh", cmod_home());
	if (file_exists(script)) { const char *tv[] = { "bash", script, "boot", NULL }; free(run_capture(tv, 60)); }
	/* save snapshots when a game is closed and once a day (modules/backup-agent), its own process */
	snprintf(script, sizeof(script), "%s/modules/backup-agent/backup-agent.sh", cmod_home());
	pid_t c = -1;
	if (file_exists(script) && (c = fork()) == 0) {
		setsid();
		if (fork() != 0) _exit(0);                   /* (its own process, not cmremote's child) */
		int nul = open("/dev/null", O_RDWR);
		if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
		for (int i = 3; i < 256; i++) close(i);
		execlp("nice", "nice", "-n", "10", "bash", script, "run", (char *)NULL);
		_exit(127);
	} else if (c > 0) waitpid(c, NULL, 0);
	guard_script(script, sizeof(script));
	if (!file_exists(script)) return NULL;
	const char *argv[] = { "bash", script, "boot", NULL };
	free(run_capture(argv, 600));
	/* a few minutes in (not while the SuperStation is still starting up), the running set kept, unless an update is
	   running already: then the watcher has it */
	sleep(180);
	if (!updater_running()) save_current(1, "after start");
	return NULL;
}

static void *watch_run(void *a)
{
	(void)a;
	int was = 0;
	for (;;) {
		sleep(5);
		int now = updater_running();
		if (now && !was) save_current(0, "Update All started");
		was = now;
	}
	return NULL;
}

void jobs_boot(void)
{
	char p[700];
	snprintf(p, sizeof(p), "%s/ua/status.json", cmod_home());
	setenv("UA_STATUS", p, 0);
	snprintf(p, sizeof(p), "%s/ua_allow.json", cmod_home());
	setenv("UA_ALLOW", p, 0);
	pthread_t t;
	if (pthread_create(&t, NULL, boot_run, NULL) == 0) pthread_detach(t);
	if (pthread_create(&t, NULL, watch_run, NULL) == 0) pthread_detach(t);
}
