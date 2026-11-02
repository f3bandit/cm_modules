/* ss1remote: Power, Memory cards and the Controller tester for the apps (through the modules: ss1mod, ss1jobd).
 * Copyright (C) 2026 f3bandit. GPL-3.0-or-later (see ../LICENSE); Retro Remake: see ../LICENSE-RETRO-REMAKE.md.
 *
 *   {"cmd":"power","do":"reboot"|"shutdown"|"menu"}   restart the SS1 safely, switch it off, or back to Console Mode
 *   {"cmd":"mc"}                                      the cores installed, the last job's result, the backups
 *   {"cmd":"mc","do":"backup"}                        back up the card in front port 1 into backups/memory-cards
 *   {"cmd":"mc","do":"verify"|"write","file":"<name>"} check the card against a backup, or write a backup to it
 *   {"cmd":"mc","do":"saves","file":"<name>"}          the saves in a backup (names, product codes, blocks)
 *   {"cmd":"pad","do":"start"}                        the controller tester core (Select+Start 2 s on a pad: back)
 * The memory card and controller jobs load their core, so Console Mode closes until they're done: the app shows
 * "on the TV now" and reads the result after (last_job.txt). */
#define _GNU_SOURCE
#include "tools.h"
#include "jobs.h"
#include "dash.h"
#include "../include/ss1mod.h"
#include "../vendor/mcformat.h"
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static cJSON *fail(const char *m)
{
	cJSON *o = cJSON_CreateObject();
	cJSON_AddBoolToObject(o, "ok", 0);
	cJSON_AddStringToObject(o, "error", m);
	return o;
}
static cJSON *ok(void) { cJSON *o = cJSON_CreateObject(); cJSON_AddBoolToObject(o, "ok", 1); return o; }

/* ---------- power ---------- */

static void later(const char *sh)
{
	/* after the answer went out: in its own session, a moment later */
	if (fork() == 0) {
		setsid();
		int nul = open("/dev/null", O_RDWR);
		if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
		for (int i = 3; i < 256; i++) close(i);
		execl("/bin/sh", "sh", "-c", sh, (char *)NULL);
		_exit(127);
	}
}

static cJSON *cmd_power(cJSON *req)
{
	const char *d = cJSON_GetStringValue(cJSON_GetObjectItem(req, "do"));
	if (!d) return fail("do: reboot, shutdown or menu");
	if (!strcmp(d, "reboot")) { plog("power: restart from an app"); later("sleep 1; sync; reboot"); }
	else if (!strcmp(d, "shutdown")) {
		plog("power: shut down from an app");
		/* the SS1's own shutdown script when it's there (it switches the SuperStation off cleanly), else poweroff */
		later("sleep 1; sync; if [ -x /media/fat/Scripts/shutdown.sh ]; then /media/fat/Scripts/shutdown.sh; else poweroff; fi");
	} else if (!strcmp(d, "menu")) {
		if (ss1_load("menu.rbf")) return fail("MiSTer didn't take the command");
	} else return fail("do: reboot, shutdown or menu");
	return ok();
}

/* ---------- memory cards ---------- */

static void mc_dir(char *o, size_t n) { snprintf(o, n, "%s/backups/memory-cards", ss1_home()); }

/* a backup's file name: letters, digits, - _ . and spaces, ending .mcd, no folders */
static int name_ok(const char *s)
{
	size_t l = strlen(s);
	if (l < 5 || l > 120 || s[0] == '.' || strcmp(s + l - 4, ".mcd")) return 0;
	for (const char *c = s; *c; c++) if (!(isalnum((unsigned char)*c) || strchr("-_. ()", *c))) return 0;
	return 1;
}

static int cmp_name_desc(const void *a, const void *b) { return -strcmp(*(char *const *)a, *(char *const *)b); }

static cJSON *mc_state(void)
{
	cJSON *o = ok();
	char p[700], d[700];
	cJSON_AddBoolToObject(o, "core", !ss1_core_installed(SS1_MC_PREFIX, p, sizeof p));
	cJSON_AddBoolToObject(o, "jobd", access(SS1_JOBD, X_OK) == 0);
	/* the last job, as ss1jobd left it */
	cJSON *j = cJSON_AddObjectToObject(o, "last");
	static const char *const K[] = { "job", "state", "message", "result", "card", "ok", "retried", "failed", "differ", "written", "file", "at", "error", NULL };
	for (int i = 0; K[i]; i++) { char v[512]; if (!ss1_job_result(K[i], v, sizeof v)) cJSON_AddStringToObject(j, K[i], v); }
	mc_dir(d, sizeof d);
	cJSON_AddStringToObject(o, "folder", d);
	cJSON *arr = cJSON_AddArrayToObject(o, "backups");
	DIR *dd = opendir(d);
	char *names[512];
	int n = 0;
	struct dirent *e;
	while (dd && (e = readdir(dd)) && n < 512) if (name_ok(e->d_name)) names[n++] = strdup(e->d_name);
	if (dd) closedir(dd);
	qsort(names, (size_t)n, sizeof(char *), cmp_name_desc);
	for (int i = 0; i < n; i++) {
		snprintf(p, sizeof p, "%s/%s", d, names[i]);
		struct stat sb;
		cJSON *x = cJSON_CreateObject();
		cJSON_AddStringToObject(x, "name", names[i]);
		cJSON_AddNumberToObject(x, "at", stat(p, &sb) ? 0 : (double)sb.st_mtime);
		cJSON_AddBoolToObject(x, "card_image", !stat(p, &sb) && sb.st_size == MC_SIZE);
		cJSON_AddItemToArray(arr, x);
		free(names[i]);
	}
	return o;
}

static cJSON *mc_saves(const char *file)
{
	char d[700], p[900];
	mc_dir(d, sizeof d);
	snprintf(p, sizeof p, "%s/%s", d, file);
	static unsigned char img[MC_SIZE];
	FILE *f = fopen(p, "rb");
	if (!f) return fail("no such backup");
	size_t got = fread(img, 1, sizeof img, f);
	fclose(f);
	if (got != MC_SIZE) return fail("that isn't a memory card image (128 KB)");
	mc_card_t c;
	cJSON *o = ok();
	if (mc_parse(img, &c)) { cJSON_AddBoolToObject(o, "formatted", 0); return o; }
	cJSON_AddBoolToObject(o, "formatted", c.formatted);
	cJSON_AddNumberToObject(o, "free", c.free);
	cJSON *arr = cJSON_AddArrayToObject(o, "saves");
	for (int i = 0; i < c.nsaves; i++) {
		cJSON *x = cJSON_CreateObject();
		cJSON_AddStringToObject(x, "title", c.saves[i].title[0] ? c.saves[i].title : c.saves[i].name);
		cJSON_AddStringToObject(x, "product", c.saves[i].product);
		cJSON_AddStringToObject(x, "region", c.saves[i].region);
		cJSON_AddNumberToObject(x, "blocks", c.saves[i].nblocks);
		cJSON_AddBoolToObject(x, "broken", c.saves[i].broken);
		cJSON_AddItemToArray(arr, x);
	}
	return o;
}

static cJSON *cmd_mc(cJSON *req)
{
	const char *d = cJSON_GetStringValue(cJSON_GetObjectItem(req, "do"));
	const char *file = cJSON_GetStringValue(cJSON_GetObjectItem(req, "file"));
	if (!d || !*d) return mc_state();
	char dir[700], p[900];
	mc_dir(dir, sizeof dir);
	if (!strcmp(d, "saves")) { if (!file || !name_ok(file)) return fail("file: a backup's name"); return mc_saves(file); }
	if (access(SS1_JOBD, X_OK)) return fail("ss1jobd isn't installed with the app link");
	char core[700];
	if (ss1_core_installed(SS1_MC_PREFIX, core, sizeof core)) return fail("The SS1 Memory Card Tool core isn't installed (games/SS1_MemCard)");
	int job;
	if (!strcmp(d, "backup")) {
		job = SS1_JOB_MC_BACKUP;
		char stamp[32];
		time_t t = time(NULL);
		strftime(stamp, sizeof stamp, "%Y-%m-%d %H%M%S", localtime(&t));
		snprintf(p, sizeof p, "%s/card %s.mcd", dir, stamp);
		/* folders made here: ss1jobd copies the backup into it when the core is done */
		char mk[800];
		snprintf(mk, sizeof mk, "%s/backups", ss1_home());
		mkdir(mk, 0755);
		mkdir(dir, 0755);
	} else if (!strcmp(d, "verify") || !strcmp(d, "write")) {
		if (!file || !name_ok(file)) return fail("file: a backup's name");
		snprintf(p, sizeof p, "%s/%s", dir, file);
		if (access(p, R_OK)) return fail("no such backup");
		job = !strcmp(d, "verify") ? SS1_JOB_MC_VERIFY : SS1_JOB_MC_WRITE;
	} else return fail("do: backup, verify, write or saves");
	if (ss1_job_start(job, p, 0, NULL, 0)) return fail("couldn't start ss1jobd");
	plog("memory card: %s %s", d, p);
	cJSON *o = ok();
	cJSON_AddBoolToObject(o, "on_tv", 1);
	return o;
}

static cJSON *cmd_pad(cJSON *req)
{
	const char *d = cJSON_GetStringValue(cJSON_GetObjectItem(req, "do"));
	char core[700];
	cJSON *o = ok();
	cJSON_AddBoolToObject(o, "core", !ss1_core_installed(SS1_PAD_PREFIX, core, sizeof core));
	cJSON_AddBoolToObject(o, "jobd", access(SS1_JOBD, X_OK) == 0);
	if (!d || !*d) return o;
	if (strcmp(d, "start")) { cJSON_Delete(o); return fail("do: start"); }
	if (access(SS1_JOBD, X_OK)) { cJSON_Delete(o); return fail("ss1jobd isn't installed with the app link"); }
	if (ss1_core_installed(SS1_PAD_PREFIX, core, sizeof core)) { cJSON_Delete(o); return fail("The SS1 Controller Tester core isn't installed (games/SS1_Controller_Tester)"); }
	if (ss1_job_start(SS1_JOB_PADTEST, NULL, 0, NULL, 0)) { cJSON_Delete(o); return fail("couldn't start ss1jobd"); }
	plog("controller tester started from an app");
	cJSON_AddBoolToObject(o, "on_tv", 1);
	return o;
}

cJSON *tools_api(cJSON *req)
{
	const char *cmd = cJSON_GetStringValue(cJSON_GetObjectItem(req, "cmd"));
	if (!cmd) return NULL;
	if (!strcmp(cmd, "power")) return cmd_power(req);
	if (!strcmp(cmd, "mc")) return cmd_mc(req);
	if (!strcmp(cmd, "pad")) return cmd_pad(req);
	/* Diagnostics: temperatures, CPU (all and each core), memory, load, drives, network, the busiest
	   processes (ours too: ss1remote, ss1jobd, Update All's bridge), the kernel log's last lines */
	if (!strcmp(cmd, "dashboard")) {
		cJSON *o = dash_json(cJSON_IsTrue(cJSON_GetObjectItem(req, "kernel")));
		cJSON_AddBoolToObject(o, "ok", 1);
		return o;
	}
	return NULL;
}
