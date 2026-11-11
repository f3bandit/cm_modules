/* cmremote: Update All for the apps The app's Update All works through
 * tools/ua/ua_bridge.py, which runs Update All's own settings code from its update_all.pyz: the whole list as
 * Update All shows it, with a note on the items that are protected (MiSTer's Main and Linux), changes saved the way Update All's menu saves them,
 * and uninstalls through Update All's own database manager.
 * Copyright (C) 2026 f3bandit. All rights reserved; licensed to Retro Remake (see ../LICENSE.md).
 *
 * The bridge is kept running while it's used (Update All's code takes seconds to load on the SuperStation) and stopped
 * after 5 minutes unused. Requests go to it one at a time; the list is kept and sent again until it changes.
 * Loading and uninstalling run in the background: the reply says "loading" or "busy" until they're done. */
#define _GNU_SOURCE
#include "ua.h"
#include "jobs.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
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

static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;     /* the state below */
static pthread_mutex_t io = PTHREAD_MUTEX_INITIALIZER;     /* the bridge (pid, pipes): one request at a time */
static pid_t pid = -1;
static int to_fd = -1, from_fd = -1;
static char *list_json;                                    /* the last good list (the bridge's reply) */
static int loading, busy;
static time_t loading_at;                                  /* when the reading started */                                  /* reading the list / uninstalling */
static char job_title[200], *job_result;
static char load_error[300];
static time_t used_at, failed_at;                          /* last request; when loading last failed */
static time_t stamp;                                       /* the files the list was read from, as of then */

static void bridge_path(char *o, size_t n)
{
	const char *e = getenv("PHOS_UA_BRIDGE");
	if (e && *e) snprintf(o, n, "%s", e);
	else snprintf(o, n, "%s/ua/ua_bridge.py", cmod_home());
}
static void pyz_path(char *o, size_t n)
{
	const char *z = getenv("UA_PYZ");
	if (z && *z) snprintf(o, n, "%s", z);
	else snprintf(o, n, "%s/Scripts/.config/update_all/update_all.pyz", cm_fat);
}
static time_t mtime_of(const char *p)
{
	struct stat sb;
	return stat(p, &sb) ? 0 : sb.st_mtime;
}
/* what the list depends on: Update All's code (it updates itself, from the Scripts menu too), the item
   bridge and notes, and Update All's settings (changed in its own menu or its countdown during a run) */
static time_t files_stamp(void)
{
	char p[700], b[600];
	time_t t = 0;
	pyz_path(p, sizeof(p));
	t += mtime_of(p);
	bridge_path(b, sizeof(b));
	t += mtime_of(b);
	snprintf(p, sizeof(p), "%.*s/status.json", (int)(strrchr(b, '/') ? strrchr(b, '/') - b : 0), b);
	t += mtime_of(p);
	static const char *const cfg[] = { "downloader.ini", "update_arcade-organizer.ini", "MiSTer.ini",
		"Scripts/.config/update_all/update_all.json", "Scripts/.config/downloader/downloader.json", NULL };
	for (int i = 0; cfg[i]; i++) {
		snprintf(p, sizeof(p), "%s/%s", cm_fat, cfg[i]);
		t += mtime_of(p);
	}
	return t;
}

int ua_installed(void)
{
	char p[600], pyz[600];
	bridge_path(p, sizeof(p));
	pyz_path(pyz, sizeof(pyz));
	return file_exists(p) && file_exists(pyz);
}

/* (io held) the bridge and anything it started (the Downloader during an uninstall): its process group */
static void stop_locked(void)
{
	if (pid > 0) {
		kill(-pid, SIGTERM);
		int gone = 0;
		for (int i = 0; i < 20; i++) {
			if (waitpid(pid, NULL, WNOHANG) != 0) { gone = 1; break; }
			usleep(50000);
		}
		if (!gone) {
			kill(-pid, SIGKILL);
			waitpid(pid, NULL, 0);
		}
	}
	if (to_fd >= 0) close(to_fd);
	if (from_fd >= 0) close(from_fd);
	pid = -1;
	to_fd = from_fd = -1;
}

/* a line from the bridge (malloc'd), or NULL after limit seconds / when it's gone */
static char *read_line(int limit)
{
	size_t cap = 1 << 16, len = 0;
	char *b = malloc(cap);
	time_t end = time(NULL) + limit;
	while (b) {
		int left = (int)(end - time(NULL));
		if (left <= 0) break;
		struct pollfd p = { from_fd, POLLIN, 0 };
		int r = poll(&p, 1, left * 1000);
		if (r < 0 && errno == EINTR) continue;
		if (r <= 0) break;
		if (len + 4096 > cap) {
			char *nb = realloc(b, cap * 2);
			if (!nb) break;
			b = nb;
			cap *= 2;
		}
		ssize_t n = read(from_fd, b + len, cap - len - 1);
		if (n <= 0) break;
		len += (size_t)n;
		b[len] = 0;
		char *nl = memchr(b, '\n', len);
		if (nl) { *nl = 0; return b; }          /* (the bridge writes one line per request) */
	}
	free(b);
	return NULL;
}

/* (io held) starts the bridge: Update All's code loads (seconds on the SuperStation). Only from a thread. */
static int start_locked(void)
{
	char script[600];
	bridge_path(script, sizeof(script));
	int in[2], out[2];
	if (pipe2(in, O_CLOEXEC)) return -1;
	if (pipe2(out, O_CLOEXEC)) { close(in[0]); close(in[1]); return -1; }
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, in[0], 0);
	posix_spawn_file_actions_adddup2(&fa, out[1], 1);
	char errlog[600];
	snprintf(errlog, sizeof(errlog), "%s/ua-bridge.log", cmod_tmp());
	posix_spawn_file_actions_addopen(&fa, 2, errlog, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	posix_spawnattr_t at;
	posix_spawnattr_init(&at);
	posix_spawnattr_setpgroup(&at, 0);                /* its own group: stopped with what it starts */
	posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
	const char *argv[] = { "python3", script, "serve", NULL };
	int r = posix_spawnp(&pid, "python3", &fa, &at, (char *const *)argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	posix_spawnattr_destroy(&at);
	close(in[0]);
	close(out[1]);
	if (r) { close(in[1]); close(out[0]); pid = -1; return -1; }
	to_fd = in[1];
	from_fd = out[0];
	char *ready = read_line(90);                  /* Update All's code loading */
	int ok = ready && strstr(ready, "\"ready\"");
	pthread_mutex_lock(&lk);
	if (!ok) snprintf(load_error, sizeof(load_error), "%s", ready && strstr(ready, "\"error\"") ? ready : "Update All's settings didn't load");
	else load_error[0] = 0;
	if (!ok) failed_at = time(NULL);
	pthread_mutex_unlock(&lk);
	if (!ok) stop_locked();
	free(ready);
	return ok ? 0 : -1;
}

/* (io held) one request ("[\"list\"]"...) and its reply (malloc'd) */
static char *ask_locked(const char *args_json, int limit)
{
	size_t n = strlen(args_json);
	char *line = malloc(n + 2);
	if (!line) return NULL;
	memcpy(line, args_json, n);
	line[n] = '\n';
	ssize_t w = write(to_fd, line, n + 1);
	free(line);
	char *r = w == (ssize_t)(n + 1) ? read_line(limit) : NULL;
	if (!r) stop_locked();                        /* gone or stuck: started again next time */
	pthread_mutex_lock(&lk);
	used_at = time(NULL);
	pthread_mutex_unlock(&lk);
	return r;
}
/* from a thread: the bridge started first when it isn't running */
static char *ask(const char *args_json, int limit)
{
	pthread_mutex_lock(&io);
	char *r = NULL;
	if (pid > 0 || start_locked() == 0) r = ask_locked(args_json, limit);
	pthread_mutex_unlock(&io);
	return r;
}

/* (a thread) reads the list again; only a good one replaces the last */
static void refresh_list(void)
{
	time_t st = files_stamp();
	char *l = ask("[\"list\"]", 60);
	cJSON *j = l ? cJSON_Parse(l) : NULL;
	int good = j && cJSON_IsTrue(cJSON_GetObjectItem(j, "ok")) && cJSON_GetObjectItem(j, "menus");
	cJSON_Delete(j);
	pthread_mutex_lock(&lk);
	if (good) { free(list_json); list_json = l; l = NULL; stamp = st; load_error[0] = 0; }
	else {
		failed_at = time(NULL);
		if (!load_error[0]) snprintf(load_error, sizeof(load_error), "Update All's settings couldn't be read");
	}
	pthread_mutex_unlock(&lk);
	free(l);
}

static void *loader(void *a)
{
	int restart = a != NULL;
	if (restart) {                                /* Update All or its settings changed: its code loads again */
		pthread_mutex_lock(&io);
		stop_locked();
		pthread_mutex_unlock(&io);
	}
	refresh_list();
	pthread_mutex_lock(&lk);
	loading = 0;
	pthread_mutex_unlock(&lk);
	return NULL;
}

static int start_thread(void *(*fn)(void *), void *arg)
{
	pthread_t t;
	pthread_attr_t at;
	pthread_attr_init(&at);
	pthread_attr_setstacksize(&at, 256 * 1024);
	pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
	int r = pthread_create(&t, &at, fn, arg);
	pthread_attr_destroy(&at);
	if (r) plog("Update All: couldn't start a thread (%d)", r);
	return r;
}

/* (lk held) starts reading the list in the background, unless it's being read or just failed */
static int load_locked(int restart, int fresh)
{
	/* a read takes at most ~3 minutes (the bridge starting, then the list): longer, and it's gone */
	if (loading && time(NULL) - loading_at < 200) return 1;
	if (!fresh && load_error[0] && failed_at && time(NULL) - failed_at < 60) return 0;   /* (not every poll) */
	loading = 1;
	loading_at = time(NULL);
	if (start_thread(loader, restart ? (void *)1 : NULL)) {
		loading = 0;
		failed_at = time(NULL);
		snprintf(load_error, sizeof(load_error), "Update All's settings couldn't be read (no thread)");
		return 0;
	}
	return 1;
}

static void *uninstaller(void *a)
{
	char *args = a;
	char *r = ask(args, 1800);                    /* the Downloader removing / fetching files: up to 30 min */
	free(args);
	pthread_mutex_lock(&lk);
	stamp = files_stamp();                        /* (its own changes: no reload for them) */
	pthread_mutex_unlock(&lk);
	refresh_list();
	pthread_mutex_lock(&lk);
	free(job_result);
	job_result = r ? r : strdup("{\"ok\":false,\"error\":\"Update All's uninstall didn't answer\"}");
	busy = 0;
	pthread_mutex_unlock(&lk);
	return NULL;
}

static void *refresher(void *a)
{
	(void)a;
	refresh_list();
	pthread_mutex_lock(&lk);
	loading = 0;
	pthread_mutex_unlock(&lk);
	return NULL;
}

/* the bridge's own command as an args array: ["act","menu",3,"ok"] */
static cJSON *args_of(const char *cmd, cJSON *req)
{
	cJSON *a = cJSON_CreateArray();
	cJSON_AddItemToArray(a, cJSON_CreateString(cmd));
	cJSON *rest = cJSON_GetObjectItem(req, "args"), *it;
	cJSON_ArrayForEach(it, rest) {
		if (cJSON_IsString(it)) cJSON_AddItemToArray(a, cJSON_CreateString(it->valuestring));
		else if (cJSON_IsNumber(it)) cJSON_AddItemToArray(a, cJSON_CreateNumber(it->valuedouble));
	}
	return a;
}

static cJSON *wait_reply(const char *why)
{
	cJSON *o = cJSON_CreateObject();
	cJSON_AddBoolToObject(o, "ok", 0);
	cJSON_AddBoolToObject(o, "wait", 1);
	cJSON_AddStringToObject(o, "error", why);
	return o;
}

cJSON *ua_cmd(cJSON *req, const char **err)
{
	if (!ua_installed()) { *err = "Update All isn't installed (Scripts/update_all.sh, run once), or the app link's ua bridge is missing"; return NULL; }
	const char *d = cJSON_GetStringValue(cJSON_GetObjectItem(req, "do"));
	if (!d) d = "list";

	/* never waits for the bridge: the list is read in the background, the last one sent meanwhile */
	if (!strcmp(d, "list") || !strcmp(d, "status")) {
		int fresh = cJSON_IsTrue(cJSON_GetObjectItem(req, "fresh"));
		pthread_mutex_lock(&lk);
		int stale = list_json && !busy && files_stamp() != stamp;
		if (stale) { free(list_json); list_json = NULL; }
		if (!list_json || fresh) load_locked(stale, fresh);
		cJSON *o = cJSON_CreateObject();
		if (list_json && !fresh) {
			cJSON *l = cJSON_Parse(list_json);
			if (l) { cJSON_Delete(o); o = l; }
		}
		cJSON_AddBoolToObject(o, "loading", loading);
		cJSON_AddBoolToObject(o, "busy", busy);
		if (busy) cJSON_AddStringToObject(o, "busy_with", job_title);
		if (job_result && !busy) {
			cJSON *jr = cJSON_Parse(job_result);
			if (jr) cJSON_AddItemToObject(o, "last_job", jr);
		}
		if (load_error[0] && !loading) cJSON_AddStringToObject(o, "load_error", load_error);
		pthread_mutex_unlock(&lk);
		return o;
	}
	/* answered straight away when the bridge is free and up to date; otherwise "wait" (it's loaded meanwhile) */
	if (!strcmp(d, "act") || !strcmp(d, "allow") || !strcmp(d, "preview") || !strcmp(d, "leftovers") || !strcmp(d, "dbs") || !strcmp(d, "clean_ao") ||
	    !strcmp(d, "account") || !strcmp(d, "login_start") || !strcmp(d, "login_poll") || !strcmp(d, "logout") || !strcmp(d, "link_fpga")) {
		pthread_mutex_lock(&lk);
		if (busy) { pthread_mutex_unlock(&lk); *err = "Busy uninstalling; try again when it's done"; return NULL; }
		int stale = files_stamp() != stamp;
		if (stale || loading) {
			if (stale && list_json) { free(list_json); list_json = NULL; }
			load_locked(stale, 1);
			pthread_mutex_unlock(&lk);
			return wait_reply("Update All's settings are being read again; try again in a moment");
		}
		pthread_mutex_unlock(&lk);
		if (pthread_mutex_trylock(&io)) return wait_reply("Update All's settings are busy; try again in a moment");
		if (pid <= 0) {                               /* stopped (unused for a while): started in the background */
			pthread_mutex_unlock(&io);
			pthread_mutex_lock(&lk);
			load_locked(0, 1);
			pthread_mutex_unlock(&lk);
			return wait_reply("Update All's settings are loading; try again in a moment");
		}
		cJSON *a = args_of(d, req);
		char *s = cJSON_PrintUnformatted(a);
		cJSON_Delete(a);
		char *r = s ? ask_locked(s, 60) : NULL;
		pthread_mutex_unlock(&io);
		free(s);
		if (!r) { *err = "Update All's settings didn't answer"; return NULL; }
		cJSON *o = cJSON_Parse(r);
		free(r);
		if (!o) { *err = "Update All's settings answered something unreadable"; return NULL; }
		if (strcmp(d, "preview") && strcmp(d, "leftovers") && strcmp(d, "dbs") && strcmp(d, "account") && strcmp(d, "login_start") && strcmp(d, "login_poll")) {
			pthread_mutex_lock(&lk);
			stamp = files_stamp();                    /* (our own change: not a reason to reload) */
			if (!loading) { loading = 1; loading_at = time(NULL); if (start_thread(refresher, NULL)) loading = 0; }   /* values or the guard changed */
			pthread_mutex_unlock(&lk);
		}
		return o;
	}
	if (!strcmp(d, "run")) {
		/* Update All itself, on the SuperStation, through update-guard: a restore point of MiSTer's Main and Linux
		 * first, then update_all.sh. Its output is kept for the app (ua_run); the list is read again after. */
		pthread_mutex_lock(&lk);
		int b = busy;
		pthread_mutex_unlock(&lk);
		if (b) { *err = "Busy uninstalling; run it when that's done"; return NULL; }
		const char *r = job_start("ua_run", "update", NULL, 0);
		if (r) { *err = r; return NULL; }
		plog("Update All: run started");
		cJSON *o = cJSON_CreateObject();
		cJSON_AddBoolToObject(o, "running", 1);
		return o;
	}
	if (!strcmp(d, "uninstall") || !strcmp(d, "install") || !strcmp(d, "update_db")) {
		cJSON *a = args_of(d, req);
		if (cJSON_GetArraySize(a) < 2) { cJSON_Delete(a); *err = "args: the database ids"; return NULL; }
		char *s = cJSON_PrintUnformatted(a);
		cJSON_Delete(a);
		const char *title = cJSON_GetStringValue(cJSON_GetObjectItem(req, "title"));
		pthread_mutex_lock(&lk);
		if (busy || !s) { pthread_mutex_unlock(&lk); free(s); *err = "Busy with another install or uninstall"; return NULL; }
		busy = 1;
		snprintf(job_title, sizeof(job_title), "%s", title ? title : "");
		free(job_result);
		job_result = NULL;
		pthread_mutex_unlock(&lk);
		plog("Update All: %s %s", !strcmp(d, "install") ? "installing" : !strcmp(d, "update_db") ? "updating" : "uninstalling", title ? title : s);
		if (start_thread(uninstaller, s)) {
			pthread_mutex_lock(&lk);
			busy = 0;
			pthread_mutex_unlock(&lk);
			free(s);
			*err = "couldn't start it: try again";
			return NULL;
		}
		cJSON *o = cJSON_CreateObject();
		cJSON_AddBoolToObject(o, "busy", 1);
		return o;
	}
	*err = "do: list, act (args: menu, index, ok|toggle[, choice]), allow (args: key, on|off), preview (args: db ids), uninstall (args: db ids), install (args: a db id), update_db (args: a db id), dbs (the Database Manager's list), leftovers, clean_ao, account, login_start, login_poll (args: device_code, device_id), logout, link_fpga, run";
	return NULL;
}

void ua_tick(void)
{
	pthread_mutex_lock(&lk);
	int idle = !busy && !loading && used_at && time(NULL) - used_at > 300;
	pthread_mutex_unlock(&lk);
	if (idle && pthread_mutex_trylock(&io) == 0) {
		if (pid > 0) stop_locked();               /* 5 min unused: Update All's code out of memory */
		pthread_mutex_unlock(&io);
	}
}
