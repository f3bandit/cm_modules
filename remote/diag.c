/* cmremote: the whole-system checks (modules/diag) in the background, and the support report for a bug report.
 * Copyright (C) 2026 f3bandit. GPL-3.0-or-later (see ../LICENSE); Retro Remake: see ../LICENSE-RETRO-REMAKE.md.
 *
 * diag.sh --kv prints one "area.key=level|message" line per check and a "summary=level|text" line; it takes up to
 * ~20 s (network checks), so it runs in a thread and the app asks again. "bundle" makes the support .tar.gz
 * (passwords removed) in tools/logs, which a paired app fetches with GET /backup/logs/<file>. */
#define _GNU_SOURCE
#include "diag.h"
#include "jobs.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static int busy;                 /* 0 idle, 1 checking, 2 making a report */
static char *last;
static time_t last_at;
static char bundle[600], err_text[200];

static void script(char *o, size_t n) { snprintf(o, n, "%s/modules/diag/diag.sh", cmod_home()); }

static void *worker(void *arg)
{
	int what = (int)(long)arg;
	char s[700];
	script(s, sizeof(s));
	const char *a1[] = { "bash", s, "--kv", NULL }, *a2[] = { "bash", s, "bundle", NULL };
	char *out = run_capture(what == 1 ? a1 : a2, what == 1 ? 180 : 240);
	pthread_mutex_lock(&lk);
	if (what == 1) {
		if (out) { free(last); last = out; out = NULL; last_at = time(NULL); err_text[0] = 0; }
		else snprintf(err_text, sizeof(err_text), "The checks didn't run");
	} else {
		bundle[0] = 0;
		char *p = out ? strstr(out, ".tar.gz") : NULL;
		if (p) {
			char *b = p;
			while (b > out && b[-1] != '\n') b--;
			snprintf(bundle, sizeof(bundle), "%.*s", (int)(p + 7 - b), b);
		} else snprintf(err_text, sizeof(err_text), "Couldn't make the report");
	}
	busy = 0;
	pthread_mutex_unlock(&lk);
	free(out);
	plog("diag: %s", what == 1 ? "checks finished" : bundle[0] ? bundle : "report failed");
	return NULL;
}

static cJSON *fail(const char *m) { cJSON *o = cJSON_CreateObject(); cJSON_AddBoolToObject(o, "ok", 0); cJSON_AddStringToObject(o, "error", m); return o; }

cJSON *diag_api(cJSON *req)
{
	const char *d = cJSON_GetStringValue(cJSON_GetObjectItem(req, "do"));
	char s[700];
	script(s, sizeof(s));
	if (!file_exists(s)) return fail("The diagnostics module isn't installed with the app link (tools/modules/diag)");
	if (d && (!strcmp(d, "run") || !strcmp(d, "bundle"))) {
		pthread_mutex_lock(&lk);
		if (!busy) {
			busy = !strcmp(d, "run") ? 1 : 2;
			if (busy == 2) bundle[0] = 0;
			pthread_t t;
			pthread_attr_t at;
			pthread_attr_init(&at);
			pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
			if (pthread_create(&t, &at, worker, (void *)(long)busy)) busy = 0;
			pthread_attr_destroy(&at);
		}
		pthread_mutex_unlock(&lk);
	} else if (d && *d) return fail("do: run or bundle");
	cJSON *o = cJSON_CreateObject();
	pthread_mutex_lock(&lk);
	cJSON_AddBoolToObject(o, "busy", busy != 0);
	cJSON_AddStringToObject(o, "busy_with", busy == 1 ? "checks" : busy == 2 ? "report" : "");
	cJSON_AddNumberToObject(o, "at", (double)last_at);
	if (bundle[0]) { const char *b = strrchr(bundle, '/'); cJSON_AddStringToObject(o, "bundle", b ? b + 1 : bundle); }
	if (err_text[0]) cJSON_AddStringToObject(o, "error_text", err_text);
	cJSON *arr = cJSON_AddArrayToObject(o, "results");
	char *t = last ? strdup(last) : NULL, *save = NULL;
	pthread_mutex_unlock(&lk);
	for (char *l = t ? strtok_r(t, "\n", &save) : NULL; l; l = strtok_r(NULL, "\n", &save)) {
		char *eq = strchr(l, '='), *bar = eq ? strchr(eq, '|') : NULL;
		if (!eq || !bar) continue;
		*eq = 0; *bar = 0;
		if (!strcmp(l, "summary")) {
			cJSON *sm = cJSON_AddObjectToObject(o, "summary");
			cJSON_AddStringToObject(sm, "level", eq + 1);
			cJSON_AddStringToObject(sm, "text", bar + 1);
			continue;
		}
		cJSON *x = cJSON_CreateObject();
		char *dot = strchr(l, '.');
		cJSON_AddStringToObject(x, "area", dot ? (*dot = 0, l) : l);
		cJSON_AddStringToObject(x, "key", dot ? dot + 1 : "");
		cJSON_AddStringToObject(x, "level", eq + 1);
		cJSON_AddStringToObject(x, "message", bar + 1);
		cJSON_AddItemToArray(arr, x);
	}
	free(t);
	cJSON_AddBoolToObject(o, "ok", 1);
	return o;
}
