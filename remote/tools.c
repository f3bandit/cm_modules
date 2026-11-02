/* cmremote: Power, Memory cards and the Controller tester for the apps (through the modules: cmmod, cmjobd).
 * Copyright (C) 2026 f3bandit. GPL-3.0-or-later (see ../LICENSE); Retro Remake: see ../LICENSE-RETRO-REMAKE.md.
 *
 *   {"cmd":"power","do":"reboot"|"shutdown"|"menu"}   restart the SuperStation safely, switch it off, or back to Console Mode
 *   {"cmd":"mc"}                                      the cores installed, the last job's result, the backups
 *   {"cmd":"mc","do":"backup"}                        back up the card in front port 1 into backups/memory-cards
 *   {"cmd":"mc","do":"verify"|"write","file":"<name>"} check the card against a backup, or write a backup to it
 *   {"cmd":"mc","do":"saves","file":"<name>"}          the saves in a backup (names, product codes, blocks)
 *   {"cmd":"pad","do":"start"}                        the controller tester core (Select+Start 2 s on a pad: back)
 *   {"cmd":"wifi"[,"do":"scan"|"connect"|"forget"|"up"|"down"|"undo","ssid","password","hidden"]}
 *                                                     WiFi: status, saved networks (first tried first), the last scan's
 *                                                     networks (strongest first); passwords are never sent back
 *   {"cmd":"trim"[,"do":"trim"|"schedule","schedule":"weekly"|"daily"|"deletes"|"never"]}
 *                                                     the dock's NVMe TRIM: status (the bridge, the way it takes TRIM, the
 *                                                     last trim, what Linux said), trim now, its schedule
 *   {"cmd":"repair"[,"do":"setup"(drive)|"arm"|"disarm"|"restart"|"result"|"remove"]}
 *                                                     the offline SD card repair: the repair drive, the last result
 *   {"cmd":"diag"[,"do":"run"|"bundle"]}               the whole-system checks (in the background) and the support report
 *   {"cmd":"scraper_accounts"[,"do":"set"(ssid, sspassword, tgdb_key)|"clear"]}   Console Mode's box-art logins
 *                                                     (screenscraper.txt, tgdb_apikey.txt); passwords never sent back
 *   {"cmd":"snapshots"[,"do":"now"|"preview"|"restore","name","only"]}   save snapshots (modules/backup-agent), as jobs
 *   {"cmd":"sysbk"[,"get":id,"offset" | "put":id,"offset","size","hash","data"]}   MiSTer & Linux backups for the PC
 *                                                     (sysbk.c): the system files and settings, read and put back in pieces
 *   {"cmd":"bluetooth"[,"do":"pair"|"cancel"|"forget"|"disconnect","mac"]}
 *                                                     Bluetooth controllers: the paired ones, and pairing (up to 90 s)
 *   {"cmd":"controllers"[,"do":"style"|"reset"|"backup"|"restore","vidpid","style","name"] | "live":"eventN"}
 *                                                     USB and Bluetooth controllers: each one's buttons, what MiSTer maps
 *                                                     it with (config/inputs), backups of the mappings; live: what's pressed
 *   {"cmd":"vset"[,"do":"try"|"keep"|"revert"|"undo",...]}  MiSTer.ini's video settings, tried with Keep (vtry.c)
 *   {"cmd":"ini"[,"file","do":"save"|"restore","text"|"name"]}  MiSTer.ini / MiSTer_*.ini, each save kept first
 *   {"cmd":"network"} {"cmd":"usb"}                   what the network and USB look like (read only)
 *   {"cmd":"scripts"[,"do":"run","name"]}             the Scripts folder; run one (its output: {"cmd":"job"})
 * The memory card and controller jobs load their core, so Console Mode closes until they're done: the app shows
 * "on the TV now" and reads the result after (last_job.txt). */
#define _GNU_SOURCE
#include "tools.h"
#include "jobs.h"
#include "cmlib.h"
#include "dash.h"
#include "ctrl.h"
#include "vtry.h"
#include "extras.h"
#include "diag.h"
#include "sysbk.h"
#include "../include/cmmod.h"
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

static cJSON *kv_json(const char *text);
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
		/* the SuperStation's own shutdown script when it's there (it switches the SuperStation off cleanly), else poweroff */
		later("sleep 1; sync; if [ -x /media/fat/Scripts/shutdown.sh ]; then /media/fat/Scripts/shutdown.sh; else poweroff; fi");
	} else if (!strcmp(d, "menu")) {
		if (cmod_load("menu.rbf")) return fail("MiSTer didn't take the command");
	} else return fail("do: reboot, shutdown or menu");
	return ok();
}

/* ---------- memory cards ---------- */

static void mc_dir(char *o, size_t n) { snprintf(o, n, "%s/backups/memory-cards", cmod_home()); }

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
	cJSON_AddBoolToObject(o, "core", !cmod_core_installed(CMOD_MC_PREFIX, p, sizeof p));
	cJSON_AddBoolToObject(o, "jobd", access(CMOD_JOBD, X_OK) == 0);
	/* the last job, as cmjobd left it */
	cJSON *j = cJSON_AddObjectToObject(o, "last");
	static const char *const K[] = { "job", "state", "message", "result", "card", "ok", "retried", "failed", "differ", "written", "file", "at", "error", NULL };
	for (int i = 0; K[i]; i++) { char v[512]; if (!cmod_job_result(K[i], v, sizeof v)) cJSON_AddStringToObject(j, K[i], v); }
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
	if (access(CMOD_JOBD, X_OK)) return fail("cmjobd isn't installed with the app link");
	char core[700];
	if (cmod_core_installed(CMOD_MC_PREFIX, core, sizeof core)) return fail("The Memory Card Tool core isn't installed (games/SS1_MemCard)");
	int job;
	if (!strcmp(d, "backup")) {
		job = CMOD_JOB_MC_BACKUP;
		char stamp[32];
		time_t t = time(NULL);
		strftime(stamp, sizeof stamp, "%Y-%m-%d %H%M%S", localtime(&t));
		snprintf(p, sizeof p, "%s/card %s.mcd", dir, stamp);
		/* folders made here: cmjobd copies the backup into it when the core is done */
		char mk[800];
		snprintf(mk, sizeof mk, "%s/backups", cmod_home());
		mkdir(mk, 0755);
		mkdir(dir, 0755);
	} else if (!strcmp(d, "verify") || !strcmp(d, "write")) {
		if (!file || !name_ok(file)) return fail("file: a backup's name");
		snprintf(p, sizeof p, "%s/%s", dir, file);
		if (access(p, R_OK)) return fail("no such backup");
		job = !strcmp(d, "verify") ? CMOD_JOB_MC_VERIFY : CMOD_JOB_MC_WRITE;
	} else return fail("do: backup, verify, write or saves");
	if (cmod_job_start(job, p, 0, NULL, 0)) return fail("couldn't start cmjobd");
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
	cJSON_AddBoolToObject(o, "core", !cmod_core_installed(CMOD_PAD_PREFIX, core, sizeof core));
	cJSON_AddBoolToObject(o, "jobd", access(CMOD_JOBD, X_OK) == 0);
	if (!d || !*d) return o;
	if (strcmp(d, "start")) { cJSON_Delete(o); return fail("do: start"); }
	if (access(CMOD_JOBD, X_OK)) { cJSON_Delete(o); return fail("cmjobd isn't installed with the app link"); }
	if (cmod_core_installed(CMOD_PAD_PREFIX, core, sizeof core)) { cJSON_Delete(o); return fail("The Controller Tester core isn't installed (games/SS1_Controller_Tester)"); }
	if (cmod_job_start(CMOD_JOB_PADTEST, NULL, 0, NULL, 0)) { cJSON_Delete(o); return fail("couldn't start cmjobd"); }
	plog("controller tester started from an app");
	cJSON_AddBoolToObject(o, "on_tv", 1);
	return o;
}

/* ---------- save snapshots (modules/backup-agent: saves, save states and settings, when a game is closed and
   once a day, into backups/saves) ---------- */

static cJSON *cmd_snapshots(cJSON *req)
{
	const char *d = cJSON_GetStringValue(cJSON_GetObjectItem(req, "do"));
	const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(req, "name"));
	const char *only = cJSON_GetStringValue(cJSON_GetObjectItem(req, "only"));
	char script[700];
	snprintf(script, sizeof(script), "%s/modules/backup-agent/backup-agent.sh", cmod_home());
	if (!file_exists(script)) return fail("Save snapshots aren't installed with the app link (tools/modules/backup-agent)");
	if (d && !strcmp(d, "now")) {
		const char *argv[] = { "bash", script, "now", NULL };
		const char *why = job_run("Snapshot of the saves", argv);
		if (why) return fail(why);
	} else if (d && (!strcmp(d, "restore") || !strcmp(d, "preview"))) {
		if (!name || !*name || strchr(name, '/') || strstr(name, "..")) return fail("name: a snapshot");
		if (only && (strchr(only, '/') || strstr(only, ".."))) return fail("only: saves, savestates...");
		const char *argv[10];
		int a = 0;
		argv[a++] = "bash"; argv[a++] = script; argv[a++] = "restore"; argv[a++] = name;
		if (only && *only) { argv[a++] = "--only"; argv[a++] = only; }
		if (!strcmp(d, "preview")) argv[a++] = "--dry-run";
		else argv[a++] = "--force";
		argv[a] = NULL;
		const char *why = job_run(!strcmp(d, "preview") ? "What a restore would put back" : "Putting a snapshot back", argv);
		if (why) return fail(why);
	} else if (d && *d) return fail("do: now, preview (name, only) or restore (name, only)");
	const char *as[] = { "bash", script, "status", NULL }, *al[] = { "bash", script, "list", NULL };
	char *st = run_capture(as, 30), *ls = run_capture(al, 30);
	cJSON *o = kv_json(st), *arr = cJSON_AddArrayToObject(o, "snapshots");
	char *save = NULL;
	for (char *l = ls ? strtok_r(ls, "\n", &save) : NULL; l; l = strtok_r(NULL, "\n", &save)) {
		char *tab = strchr(l, '\t');
		if (!tab) continue;
		*tab = 0;
		cJSON *x = cJSON_CreateObject();
		cJSON_AddStringToObject(x, "name", l);
		cJSON_AddNumberToObject(x, "kb", atof(tab + 1));
		cJSON_AddItemToArray(arr, x);
	}
	free(st);
	free(ls);
	cJSON_AddBoolToObject(o, "ok", 1);
	return o;
}

/* ---------- Console Mode's box-art logins (ConsoleMode/screenscraper.txt, tgdb_apikey.txt): set from an app,
   never sent back (only whether each is set, and the ScreenScraper user name) ---------- */

static int write_file_atomic(const char *path, const char *text)
{
	char tmp[800];
	snprintf(tmp, sizeof(tmp), "%s.cm-new", path);
	FILE *f = fopen(tmp, "w");
	if (!f) return -1;
	fputs(text, f);
	if (fclose(f)) { unlink(tmp); return -1; }
	return rename(tmp, path);
}

static int field_ok(const char *v, size_t max) { if (!v || strlen(v) > max) return 0; for (const char *c = v; *c; c++) if (*c == '\n' || *c == '\r' || *c == '&') return 0; return 1; }

static cJSON *cmd_scraper(cJSON *req)
{
	char ss[700], tg[700], line[400];
	snprintf(ss, sizeof(ss), "%s/screenscraper.txt", cm_dir);
	snprintf(tg, sizeof(tg), "%s/tgdb_apikey.txt", cm_dir);
	const char *d = cJSON_GetStringValue(cJSON_GetObjectItem(req, "do"));
	if (d && !strcmp(d, "set")) {
		const char *user = cJSON_GetStringValue(cJSON_GetObjectItem(req, "ssid"));
		const char *pass = cJSON_GetStringValue(cJSON_GetObjectItem(req, "sspassword"));
		const char *key = cJSON_GetStringValue(cJSON_GetObjectItem(req, "tgdb_key"));
		if (user || pass) {
			if (!field_ok(user, 100) || !field_ok(pass, 200) || !*user) return fail("ssid and sspassword: your ScreenScraper user name and password");
			char t[400];
			snprintf(t, sizeof(t), "ssid=%s\nsspassword=%s\n", user, pass);
			if (write_file_atomic(ss, t)) return fail("couldn't write screenscraper.txt");
			plog("scraper: ScreenScraper login set from an app (user %s)", user);
		}
		if (key) {
			if (!field_ok(key, 200)) return fail("tgdb_key: TheGamesDB API key");
			char t[260];
			snprintf(t, sizeof(t), "%s\n", key);
			if (*key ? write_file_atomic(tg, t) : unlink(tg)) return fail("couldn't write tgdb_apikey.txt");
			plog("scraper: TheGamesDB key %s from an app", *key ? "set" : "removed");
		}
	} else if (d && !strcmp(d, "clear")) {
		unlink(ss);
		plog("scraper: ScreenScraper login removed from an app");
	} else if (d && *d) return fail("do: set (ssid, sspassword, tgdb_key) or clear");
	cJSON *o = ok();
	char user[120] = "";
	int haspass = 0;
	FILE *f = fopen(ss, "r");
	while (f && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (!strncmp(line, "ssid=", 5)) snprintf(user, sizeof(user), "%s", line + 5);
		else if (!strncmp(line, "sspassword=", 11)) haspass = line[11] != 0;
	}
	if (f) fclose(f);
	cJSON_AddStringToObject(o, "ss_user", user);
	cJSON_AddBoolToObject(o, "ss_password_set", haspass);
	struct stat sb;
	cJSON_AddBoolToObject(o, "tgdb_key_set", stat(tg, &sb) == 0 && sb.st_size > 8);
	return o;
}

/* ---------- the offline SD card repair (scripts/sd_offline_repair.sh: a copy of Linux on a USB/NVMe drive
   repairs the SD card at the next warm restart) ---------- */

static cJSON *cmd_repair(cJSON *req)
{
	const char *d = cJSON_GetStringValue(cJSON_GetObjectItem(req, "do"));
	const char *drive = cJSON_GetStringValue(cJSON_GetObjectItem(req, "drive"));
	char script[700];
	snprintf(script, sizeof(script), "%s/sd_offline_repair.sh", cmod_home());
	if (!file_exists(script)) return fail("The SD card repair isn't installed with the app link (tools/sd_offline_repair.sh)");
	static char out[16384];
	if (d && !strcmp(d, "setup")) {                    /* copies Linux to the drive: minutes, so a job */
		if (!drive || (strncmp(drive, "/media/usb", 10) && strcmp(drive, "/media/repair")) || strchr(drive, '\'') || strstr(drive, "..")) return fail("drive: /media/usbN");
		const char *argv[] = { "bash", script, "setup", drive, NULL };
		const char *why = job_run("Setting up the repair drive", argv);
		if (why) return fail(why);
	} else if (d && (!strcmp(d, "arm") || !strcmp(d, "disarm") || !strcmp(d, "result") || !strcmp(d, "remove") || !strcmp(d, "restart"))) {
		if (cmod_repair_run(d, NULL, out, sizeof(out))) return fail("the repair script didn't run");
		plog("repair: %s from an app", d);
	} else if (d && *d) return fail("do: setup (drive), arm, disarm, restart, result or remove");
	if (cmod_repair_run("status", NULL, out, sizeof(out))) return fail("the repair script didn't run");
	cJSON *o = cJSON_CreateObject(), *cand = cJSON_AddArrayToObject(o, "candidates"), *plug = cJSON_AddArrayToObject(o, "plugged");
	char *t = strdup(out), *save = NULL;
	for (char *l = strtok_r(t, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
		char *eq = strchr(l, '=');
		if (!eq || eq == l) continue;
		*eq = 0;
		if (!strcmp(l, "candidate") || !strcmp(l, "plugged")) {
			/* mount|partuuid|model|free */
			char *f[4] = { 0 }, *s2 = NULL;
			int k = 0;
			for (char *x = strtok_r(eq + 1, "|", &s2); x && k < 4; x = strtok_r(NULL, "|", &s2)) f[k++] = x;
			cJSON *c = cJSON_CreateObject();
			cJSON_AddStringToObject(c, "mount", f[0] ? f[0] : "");
			cJSON_AddStringToObject(c, "partuuid", f[1] ? f[1] : "");
			cJSON_AddStringToObject(c, "model", f[2] ? f[2] : "");
			cJSON_AddStringToObject(c, "free", f[3] ? f[3] : "");
			cJSON_AddItemToArray(!strcmp(l, "candidate") ? cand : plug, c);
		} else if (!cJSON_GetObjectItem(o, l)) cJSON_AddStringToObject(o, l, eq + 1);
	}
	free(t);
	cJSON_AddBoolToObject(o, "ok", 1);
	return o;
}

/* ---------- NVMe TRIM (scripts/cmtrim.sh + cmtrimfs: exFAT trimmed even where Linux's driver can't) ---------- */

/* key=value lines as an object; keys that repeat (linux=...) become arrays */
static cJSON *kv_json(const char *text)
{
	cJSON *o = cJSON_CreateObject();
	if (!text) return o;
	char *t = strdup(text), *save = NULL;
	for (char *l = strtok_r(t, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
		char *eq = strchr(l, '=');
		if (!eq || eq == l) continue;
		*eq = 0;
		cJSON *old = cJSON_GetObjectItem(o, l);
		if (!strcmp(l, "linux")) { if (!old) old = cJSON_AddArrayToObject(o, l); cJSON_AddItemToArray(old, cJSON_CreateString(eq + 1)); }
		else if (!old) cJSON_AddStringToObject(o, l, eq + 1);
	}
	free(t);
	return o;
}

static cJSON *cmd_trim(cJSON *req)
{
	const char *d = cJSON_GetStringValue(cJSON_GetObjectItem(req, "do"));
	const char *sch = cJSON_GetStringValue(cJSON_GetObjectItem(req, "schedule"));
	char script[700];
	snprintf(script, sizeof(script), "%s/cmtrim.sh", cmod_home());
	if (!file_exists(script)) return fail("NVMe TRIM isn't installed with the app link (tools/cmtrim.sh)");
	cJSON *done = NULL;
	if (d && !strcmp(d, "trim")) {
		const char *argv[] = { "bash", script, "trim", NULL };
		char *out = run_capture(argv, 600);
		done = kv_json(out);
		if (out && strstr(out, "error=")) { cJSON_Delete(done); char m[400]; snprintf(m, sizeof(m), "%s", strstr(out, "error=") + 6); m[strcspn(m, "\n")] = 0; free(out); return fail(m); }
		free(out);
		plog("trim: from an app");
	} else if (d && !strcmp(d, "schedule")) {
		if (!sch || (strcmp(sch, "weekly") && strcmp(sch, "daily") && strcmp(sch, "deletes") && strcmp(sch, "never"))) return fail("schedule: weekly, daily, deletes or never");
		const char *argv[] = { "bash", script, "schedule", sch, NULL };
		free(run_capture(argv, 30));
		if (strcmp(sch, "never")) { const char *bv[] = { "bash", script, "boot", NULL }; free(run_capture(bv, 30)); }
		else { const char *rv[] = { "bash", script, "remove", NULL }; free(run_capture(rv, 30)); }
	} else if (d && *d) return fail("do: trim or schedule");
	const char *argv[] = { "bash", script, "status", NULL };
	char *out = run_capture(argv, 30);
	cJSON *o = kv_json(out);
	free(out);
	if (done) cJSON_AddItemToObject(o, "done", done);
	cJSON_AddBoolToObject(o, "ok", 1);
	return o;
}

/* ---------- Bluetooth controllers (src/cmbt.c: MiSTer's btctl and bluetoothctl, as Console Mode uses) ---------- */

static cJSON *cmd_bluetooth(cJSON *req)
{
	char err[200] = "";
	const char *d = cJSON_GetStringValue(cJSON_GetObjectItem(req, "do"));
	const char *mac = cJSON_GetStringValue(cJSON_GetObjectItem(req, "mac"));
	if (d && *d) {
		if (!strcmp(d, "pair")) {
			int r = cmod_bt_pair_start();
			if (r == -1) return fail("No Bluetooth adapter");
			if (r == -2) return fail("Console Mode is pairing a controller already: finish that one on the TV first");
		}
		else if (!strcmp(d, "cancel")) cmod_bt_pair_cancel();
		else if (!strcmp(d, "forget")) { if (cmod_bt_forget(mac ? mac : "", err, sizeof(err))) return fail(err); }
		else if (!strcmp(d, "disconnect")) { if (cmod_bt_disconnect(mac ? mac : "", err, sizeof(err))) return fail(err); }
		else return fail("do: pair, cancel, forget or disconnect");
	}
	cmod_bt_state_t s;
	cmod_bt_state(&s);
	cmod_bt_dev_t dev[CMOD_BT_MAX];
	int n = cmod_bt_devices(dev, CMOD_BT_MAX);
	cJSON *o = ok();
	cJSON_AddBoolToObject(o, "present", s.present);
	cJSON *arr = cJSON_AddArrayToObject(o, "devices");
	for (int i = 0; i < n; i++) {
		cJSON *x = cJSON_CreateObject();
		cJSON_AddStringToObject(x, "mac", dev[i].mac);
		cJSON_AddStringToObject(x, "name", dev[i].name);
		cJSON_AddBoolToObject(x, "connected", dev[i].connected);
		cJSON_AddBoolToObject(x, "input", dev[i].input);
		cJSON_AddStringToObject(x, "icon", dev[i].icon);
		cJSON_AddItemToArray(arr, x);
	}
	cJSON *p = cJSON_AddObjectToObject(o, "pairing");
	cJSON_AddBoolToObject(p, "busy", s.busy);
	cJSON_AddBoolToObject(p, "ok", s.ok);
	cJSON_AddNumberToObject(p, "at", (double)s.at);
	cJSON_AddStringToObject(p, "found", s.found);
	cJSON_AddStringToObject(p, "message", s.message);
	return o;
}

/* ---------- WiFi (src/cmwifi.c: the same wpa_supplicant.conf Console Mode's WiFi menu uses) ---------- */

static cJSON *cmd_wifi(cJSON *req)
{
	const char *act = cJSON_GetStringValue(cJSON_GetObjectItem(req, "do"));
	const char *ssid = cJSON_GetStringValue(cJSON_GetObjectItem(req, "ssid"));
	const char *pass = cJSON_GetStringValue(cJSON_GetObjectItem(req, "password"));
	int hidden = cJSON_IsTrue(cJSON_GetObjectItem(req, "hidden"));
	char err[200] = "";
	int rc = 0;
	if (act && !strcmp(act, "scan")) cmod_wifi_scan_start();
	else if (act && !strcmp(act, "connect")) rc = cmod_wifi_connect(ssid, pass, hidden, err, sizeof(err));
	else if (act && !strcmp(act, "forget")) rc = cmod_wifi_forget(ssid ? ssid : "", err, sizeof(err));
	else if (act && (!strcmp(act, "up") || !strcmp(act, "down"))) rc = cmod_wifi_move(ssid ? ssid : "", !strcmp(act, "up") ? -1 : 1, err, sizeof(err));
	else if (act && !strcmp(act, "undo")) rc = cmod_wifi_undo(err, sizeof(err));
	else if (act && *act) return fail("do: scan, connect (ssid, password, hidden), forget, up, down or undo");
	if (rc) return fail(err[0] ? err : "that didn't work");
	if (act && strcmp(act, "scan")) plog("wifi: %s %s", act, ssid ? ssid : "");
	cmod_wifi_status_t st;
	cmod_wifi_status(&st);
	cJSON *o = ok(), *s = cJSON_AddObjectToObject(o, "status");
	cJSON_AddStringToObject(s, "iface", st.iface);
	cJSON_AddBoolToObject(s, "present", st.present);
	cJSON_AddBoolToObject(s, "connected", st.connected);
	cJSON_AddStringToObject(s, "ssid", st.ssid);
	cJSON_AddNumberToObject(s, "signal", st.signal);
	cJSON_AddNumberToObject(s, "freq", st.freq);
	cJSON_AddStringToObject(s, "ip", st.ip);
	cJSON_AddBoolToObject(s, "supplicant", st.supplicant);
	cmod_wifi_saved_t sv[CMOD_WIFI_SAVED_MAX];
	int ns = cmod_wifi_saved(sv, CMOD_WIFI_SAVED_MAX);
	cJSON *sa = cJSON_AddArrayToObject(o, "saved");
	for (int i = 0; i < ns; i++) {
		cJSON *x = cJSON_CreateObject();
		cJSON_AddStringToObject(x, "ssid", sv[i].ssid);
		cJSON_AddBoolToObject(x, "open", sv[i].open);
		cJSON_AddItemToArray(sa, x);
	}
	cmod_wifi_net_t nets[CMOD_WIFI_MAX];
	int busy = 0;
	time_t at = 0;
	char serr[160] = "";
	int nn = cmod_wifi_scan_results(nets, CMOD_WIFI_MAX, &busy, &at, serr, sizeof(serr));
	cJSON_AddBoolToObject(o, "scanning", busy);
	cJSON_AddNumberToObject(o, "scanned_at", (double)at);
	if (serr[0]) cJSON_AddStringToObject(o, "scan_error", serr);
	cJSON *na = cJSON_AddArrayToObject(o, "networks");
	for (int i = 0; i < nn; i++) {
		cJSON *x = cJSON_CreateObject();
		cJSON_AddStringToObject(x, "ssid", nets[i].ssid);
		cJSON_AddNumberToObject(x, "signal", nets[i].signal);
		cJSON_AddNumberToObject(x, "bands", nets[i].bands);
		cJSON_AddBoolToObject(x, "secure", nets[i].secure);
		cJSON_AddStringToObject(x, "security", nets[i].enterprise ? "enterprise" : nets[i].wpa3 && !nets[i].psk ? "wpa3" : nets[i].secure ? "wpa" : "open");
		int saved = 0;
		for (int k = 0; k < ns; k++) if (!strcmp(sv[k].ssid, nets[i].ssid)) saved = 1;
		cJSON_AddBoolToObject(x, "saved", saved);
		cJSON_AddBoolToObject(x, "current", st.connected && !strcmp(st.ssid, nets[i].ssid));
		cJSON_AddItemToArray(na, x);
	}
	return o;
}

cJSON *tools_api(cJSON *req)
{
	const char *cmd = cJSON_GetStringValue(cJSON_GetObjectItem(req, "cmd"));
	if (!cmd) return NULL;
	if (!strcmp(cmd, "power")) return cmd_power(req);
	if (!strcmp(cmd, "mc")) return cmd_mc(req);
	if (!strcmp(cmd, "pad")) return cmd_pad(req);
	if (!strcmp(cmd, "wifi")) return cmd_wifi(req);
	if (!strcmp(cmd, "trim")) return cmd_trim(req);
	if (!strcmp(cmd, "repair")) return cmd_repair(req);
	if (!strcmp(cmd, "diag")) return diag_api(req);
	if (!strcmp(cmd, "scraper_accounts")) return cmd_scraper(req);
	if (!strcmp(cmd, "snapshots")) return cmd_snapshots(req);
	if (!strcmp(cmd, "sysbk")) {
		const char *e = NULL;
		cJSON *o = sysbk_cmd(req, &e);
		if (!o) return fail(e ? e : "that didn't work");
		cJSON_AddBoolToObject(o, "ok", 1);
		return o;
	}
	if (!strcmp(cmd, "bluetooth")) return cmd_bluetooth(req);
	if (!strcmp(cmd, "vset")) {
		const char *e = NULL;
		cJSON *o = vt_cmd(req, &e);
		if (!o) return fail(e ? e : "that didn't work");
		cJSON_AddBoolToObject(o, "ok", 1);
		return o;
	}
	if (!strcmp(cmd, "network") || !strcmp(cmd, "usb") || !strcmp(cmd, "ini") || !strcmp(cmd, "scripts")) {
		const char *e = NULL;
		cJSON *o = !strcmp(cmd, "network") ? ex_network() : !strcmp(cmd, "usb") ? ex_usb() : !strcmp(cmd, "ini") ? ex_ini(req, &e) : ex_scripts(req, &e);
		if (!o) return fail(e ? e : "that didn't work");
		cJSON_AddBoolToObject(o, "ok", 1);
		return o;
	}
	if (!strcmp(cmd, "controllers")) {
		const char *e = NULL;
		cJSON *o = ctrl_command(req, &e);
		if (!o) return fail(e ? e : "that didn't work");
		cJSON_AddBoolToObject(o, "ok", 1);
		return o;
	}
	/* Diagnostics: temperatures, CPU (all and each core), memory, load, drives, network, the busiest
	   processes (ours too: cmremote, cmjobd, Update All's bridge), the kernel log's last lines */
	if (!strcmp(cmd, "dashboard")) {
		cJSON *o = dash_json(cJSON_IsTrue(cJSON_GetObjectItem(req, "kernel")));
		cJSON_AddBoolToObject(o, "ok", 1);
		return o;
	}
	return NULL;
}
