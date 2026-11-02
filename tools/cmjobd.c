/* cmjobd: runs a front-port job while the front end is closed, then brings the front end back.
 * Copyright (C) 2026 f3bandit. GPL-3.0-or-later (see LICENSE); Retro Remake: see LICENSE-RETRO-REMAKE.md.
 * The controller tester and the memory card manager are f3bandit's idea and design.
 *
 *   cmjobd backup  <image out>  <flags> <force|->    card in front port 1 -> 128 KB image
 *   cmjobd verify  <image>      <flags> -            card vs image
 *   cmjobd write   <image>      <flags> <force|->    image -> card (only sectors that differ, each read back)
 *   cmjobd padtest -            0       -            the controller tester until Select+Start is held 2 s
 *
 * Started by cmod_job_start() (detached). Writes CMOD_RESULT (key=value) when it starts and when it ends; logs
 * to CMOD_HOME/cmjobd.log. The steps: the job file is mounted first (index 1), the card
 * file after it (index 0); a core that missed the job (a slow first start) is started once more. */
#include "cmmod.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MC_JOB   CMOD_MC_DIR "/cm_job.bin"
#define MC_CARD  CMOD_MC_DIR "/cm_card.mcd"
#define MC_MGL   CMOD_MC_DIR "/cm_job.mgl"
#define PAD_REP  CMOD_PAD_DIR "/cm_report.bin"
#define PAD_MGL  CMOD_PAD_DIR "/cm_tester.mgl"
#define CARD_SIZE (128 * 1024)

static const char *g_job = "", *g_file = "";

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void msleep(int ms) { struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&t, NULL); }

static void logf_(const char *fmt, ...)
{
	FILE *f = fopen(CMOD_HOME "/cmjobd.log", "a");
	if (!f) return;
	time_t t = time(NULL);
	char ts[32];
	strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&t));
	fprintf(f, "%s %s: ", ts, g_job);
	va_list a;
	va_start(a, fmt);
	vfprintf(f, fmt, a);
	va_end(a);
	fputc('\n', f);
	fclose(f);
}

/* the result file, written whole and renamed into place (the front end never sees half of it) */
static void result(const char *state, const cmod_mc_report *r, const char *message, const char *error)
{
	FILE *f = fopen(CMOD_RESULT ".tmp", "w");
	if (!f) return;
	fprintf(f, "job=%s\nstate=%s\nat=%ld\nfile=%s\n", g_job, state, (long)time(NULL), g_file);
	if (r) fprintf(f, "result=%d\ncard=%d\nok=%d\nretried=%d\nfailed=%d\ndiffer=%d\nwritten=%d\n",
	               r->result, r->card, r->ok, r->retried, r->failed, r->differ, r->written);
	if (message) fprintf(f, "message=%s\n", message);
	if (error) fprintf(f, "error=%s\n", error);
	fclose(f);
	rename(CMOD_RESULT ".tmp", CMOD_RESULT);
	sync();
}

static int write_file(const char *path, const void *data, size_t n)
{
	char tmp[600];
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	FILE *f = fopen(tmp, "wb");
	if (!f) return -1;
	int ok = fwrite(data, 1, n, f) == n;
	ok = fclose(f) == 0 && ok;
	if (!ok || rename(tmp, path)) { unlink(tmp); return -1; }
	return 0;
}

static long read_file(const char *path, uint8_t *buf, size_t n)
{
	FILE *f = fopen(path, "rb");
	if (!f) return -1;
	size_t r = fread(buf, 1, n, f);
	fclose(f);
	return (long)r;
}

static int running(const char *core)
{
	char n[64];
	cmod_core_running(n, sizeof n);
	return !strcasecmp(n, core);
}

static int fail(const char *core, const char *why)
{
	logf_("failed: %s", why);
	result("failed", NULL, NULL, why);
	if (core && running(core)) cmod_back_to_menu(core);
	else if (core) cmod_load("menu.rbf");
	return 1;
}

/* ------------------------------------------------------------------ memory card jobs */

static int memcard(int job, const char *file, int flags, const char *force_list)
{
	static uint8_t req[CMOD_MC_JOB_SIZE], buf[CMOD_MC_JOB_SIZE], card[CARD_SIZE];
	const char *core = "SS1_MemCard";
	char rbf[300];
	if (cmod_core_installed(CMOD_MC_PREFIX, rbf, sizeof rbf))
		return fail(NULL, "the memory card core isn't installed (_Utility/SS1_MemCard_*.rbf)");
	mkdir(CMOD_MC_DIR, 0755);

	/* the card file: the image to write or check, or a blank one a backup fills in */
	memset(card, 0, sizeof card);
	if (job != CMOD_MC_BACKUP && read_file(file, card, sizeof card) != CARD_SIZE)
		return fail(NULL, "the image isn't a 128 KB memory card file");
	int force[1024], nforce = 0;
	if (force_list && strcmp(force_list, "-"))
		for (const char *p = force_list; *p && nforce < 1024; ) {
			force[nforce++] = atoi(p);
			p = strchr(p, ',');
			if (!p) break;
			p++;
		}
	uint32_t id = (uint32_t)time(NULL) ^ (uint32_t)getpid() << 16;
	cmod_mc_request(req, job, flags, id, force, nforce);
	char mgl[700];
	int ml = snprintf(mgl, sizeof mgl,
		"<mistergamedescription>\n\t<rbf>_Utility/SS1_MemCard</rbf>\n"
		"\t<file delay=\"2\" type=\"s\" index=\"1\" path=\"%s\"/>\n"
		"\t<file delay=\"3\" type=\"s\" index=\"0\" path=\"%s\"/>\n</mistergamedescription>\n", MC_JOB, MC_CARD);
	if (write_file(MC_CARD, card, sizeof card) || write_file(MC_JOB, req, sizeof req) || write_file(MC_MGL, mgl, (size_t)ml))
		return fail(NULL, "couldn't write the core's files");
	result("running", NULL, NULL, NULL);
	logf_("starting %s (id %08x)", rbf, id);
	if (cmod_load(MC_MGL)) return fail(NULL, "MiSTer didn't take the command");

	cmod_mc_report r;
	int relaunched = 0, got = 0;
	double start = now(), last = now();
	for (;;) {
		msleep(400);
		long n = read_file(MC_JOB, buf, sizeof buf);
		int p = n > 0 ? cmod_mc_parse(buf, (size_t)n, id, &r) : -1;
		if (p == 1) {
			last = now();
			got = 1;
			if (r.finished) break;
			continue;
		}
		double since = now() - start;
		/* the first start after the core was installed can be slow enough for the job file to be mounted
		 * before the core is ready: the job is lost (it just backs the card up). Start it once more. */
		if (!got && !relaunched && since > 10 && running(core)) {
			relaunched = 1;
			logf_("the core missed the job: starting it again");
			if (write_file(MC_CARD, card, sizeof card) || write_file(MC_JOB, req, sizeof req) || cmod_load(MC_MGL))
				return fail(core, "couldn't start the core again");
			start = now();
			continue;
		}
		if (!got && since > 25 && !running(core)) return fail(core, "the memory card core didn't load");
		if (!got && since > 180) return fail(core, "the core didn't start the job: is there a memory card in front port 1?");
		if (got && !running(core)) return fail(NULL, "the core was closed before the job finished");
		if (now() - last > 180) return fail(core, "the core stopped reporting progress");
	}
	logf_("result %d: %s (ok %d, retried %d, failed %d, differ %d, written %d)", r.result,
	      cmod_mc_result_text(job, r.result), r.ok, r.retried, r.failed, r.differ, r.written);

	const char *err = NULL;
	if (job == CMOD_MC_BACKUP && (r.result == CMOD_MC_OK || r.result == CMOD_MC_OK_RETRIED || r.result == CMOD_MC_SAVED_FAILED)) {
		if (read_file(MC_CARD, card, sizeof card) != CARD_SIZE) err = "the backup file isn't 128 KB";
		else if (write_file(file, card, sizeof card)) err = "couldn't save the backup where it was asked for";
	}
	cmod_back_to_menu(core);
	result(err ? "failed" : "done", &r, cmod_mc_result_text(job, r.result), err);
	return err ? 1 : 0;
}

/* ------------------------------------------------------------------ controller tester */

static int padtest(void)
{
	static uint8_t req[CMOD_PAD_REPORT_SIZE], buf[CMOD_PAD_REPORT_SIZE];
	const char *core = "SS1_Controller_Tester";
	char rbf[300];
	if (cmod_core_installed(CMOD_PAD_PREFIX, rbf, sizeof rbf))
		return fail(NULL, "the controller tester core isn't installed (_Utility/SS1_Controller_Tester_*.rbf)");
	mkdir(CMOD_PAD_DIR, 0755);
	uint32_t id = (uint32_t)time(NULL) ^ (uint32_t)getpid() << 16;
	cmod_pad_request(req, id);
	char mgl[600];
	int ml = snprintf(mgl, sizeof mgl,
		"<mistergamedescription>\n\t<rbf>_Utility/SS1_Controller_Tester</rbf>\n"
		"\t<file delay=\"2\" type=\"s\" index=\"0\" path=\"%s\"/>\n</mistergamedescription>\n", PAD_REP);
	if (write_file(PAD_REP, req, sizeof req) || write_file(PAD_MGL, mgl, (size_t)ml))
		return fail(NULL, "couldn't write the core's files");
	result("running", NULL, NULL, NULL);
	logf_("starting %s (id %08x)", rbf, id);
	if (cmod_load(PAD_MGL)) return fail(NULL, "MiSTer didn't take the command");

	cmod_pad_report r;
	int relaunched = 0, got = 0;
	double start = now(), held = 0;
	for (;;) {
		msleep(100);
		long n = read_file(PAD_REP, buf, sizeof buf);
		if (n > 0 && cmod_pad_parse(buf, (size_t)n, id, &r) == 1) {
			got = 1;
			/* Select+Start held 2 s on either controller: back to the front end */
			int both = 0;
			for (int p = 0; p < 2; p++)
				if (r.port[p].present && (r.port[p].buttons & (CMOD_PAD_SELECT | CMOD_PAD_START)) == (CMOD_PAD_SELECT | CMOD_PAD_START)) both = 1;
			if (!both) held = 0;
			else if (!held) held = now();
			else if (now() - held >= 2) { logf_("Select+Start held: closing"); break; }
		}
		double since = now() - start;
		if (!got && !relaunched && since > 10 && running(core)) {
			relaunched = 1;
			logf_("the core missed the request: starting it again");
			if (write_file(PAD_REP, req, sizeof req) || cmod_load(PAD_MGL)) return fail(core, "couldn't start the core again");
			start = now();
			continue;
		}
		if (!got && since > 25) return fail(core, "the controller tester core didn't start");
		/* closed from MiSTer's menu (or another core loaded): nothing to bring back */
		if (got && !running(core)) { logf_("the core was closed"); result("done", NULL, "Controller test closed", NULL); return 0; }
	}
	cmod_back_to_menu(core);
	result("done", NULL, "Controller test finished", NULL);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: cmjobd backup|verify|write <image> [flags] [force,list] | padtest\n");
		return 2;
	}
	mkdir(CMOD_HOME, 0755);
	g_job = argv[1];
	g_file = argc > 2 && strcmp(argv[2], "-") ? argv[2] : "";
	int flags = argc > 3 ? atoi(argv[3]) : 0;
	const char *force = argc > 4 ? argv[4] : "-";
	if (!strcmp(argv[1], "padtest")) return padtest();
	int job = !strcmp(argv[1], "backup") ? CMOD_MC_BACKUP : !strcmp(argv[1], "verify") ? CMOD_MC_VERIFY
	        : !strcmp(argv[1], "write") ? CMOD_MC_WRITE : 0;
	if (!job || !g_file[0]) { fprintf(stderr, "cmjobd: unknown job or no image\n"); return 2; }
	return memcard(job, g_file, flags, force);
}
