/* ss1mod: every module from the command line, the way a front end calls them (and for testing on an SS1).
 * Copyright (C) 2026 f3bandit. GPL-3.0-or-later (see LICENSE); Retro Remake: see LICENSE-RETRO-REMAKE.md.
 *
 *   ss1mod cores                          which cores are installed, which one is running
 *   ss1mod mc backup <image>              back up the card in front port 1 (ss1jobd; the front end closes)
 *   ss1mod mc verify <image>              check the card against an image
 *   ss1mod mc write <image>               write an image to the card
 *   ss1mod mc saves <image>               the saves on an image (ps1_memcard_lib)
 *   ss1mod pad                            the controller tester (Select+Start 2 s ends it)
 *   ss1mod pad-decode <report.bin>        decode a controller tester report file
 *   ss1mod result                         the last job's result
 *   ss1mod repair <status|setup DRIVE|arm|disarm|restart|result|remove>
 */
#include "ss1mod.h"
#include "mcformat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int usage(void)
{
	fprintf(stderr, "usage: ss1mod cores | mc backup|verify|write|saves <image> | pad | pad-decode <file> | result | repair <cmd> [arg]\n");
	return 2;
}

static int saves(const char *path)
{
	static unsigned char img[MC_SIZE];
	FILE *f = fopen(path, "rb");
	if (!f || fread(img, 1, MC_SIZE, f) != MC_SIZE) { fprintf(stderr, "%s: not a 128 KB card image\n", path); if (f) fclose(f); return 1; }
	fclose(f);
	mc_card_t c;
	if (mc_parse(img, &c) || !c.formatted) { printf("not formatted\n"); return 0; }
	printf("%d saves, %d blocks free\n", c.nsaves, c.free);
	for (int i = 0; i < c.nsaves; i++)
		printf("  block %2d  %d block(s)  %-10s %-6s %s%s\n", c.saves[i].slot, c.saves[i].nblocks, c.saves[i].product,
		       c.saves[i].region, c.saves[i].title, c.saves[i].broken ? "  (broken)" : "");
	return 0;
}

static int pad_decode(const char *path)
{
	unsigned char b[SS1_PAD_REPORT_SIZE];
	FILE *f = fopen(path, "rb");
	size_t n = f ? fread(b, 1, sizeof b, f) : 0;
	if (f) fclose(f);
	ss1_pad_report r;
	uint32_t id = n >= 12 ? (uint32_t)b[8] | (uint32_t)b[9] << 8 | (uint32_t)b[10] << 16 | (uint32_t)b[11] << 24 : 0;
	int p = ss1_pad_parse(b, n, id, &r);
	if (p != 1) { printf(p == 0 ? "no report yet (the core hasn't answered)\n" : "not a controller tester report\n"); return 1; }
	for (int i = 0; i < 2; i++) {
		ss1_pad_port *q = &r.port[i];
		printf("port %d: ", i + 1);
		if (!q->present) { printf("no controller"); }
		else {
			printf("%s (ID %02X)", ss1_pad_type(q->id), q->id);
			if (q->analog) printf("  L %3d,%3d  R %3d,%3d", q->lx, q->ly, q->rx, q->ry);
			printf("  pressed:");
			for (int b2 = 0; b2 < 16; b2++) if (q->buttons & (1u << b2)) printf(" %s", ss1_pad_button_name((uint16_t)(1u << b2)));
		}
		printf("  card: %s\n", q->card == 1 ? "formatted" : q->card == 2 ? "not formatted" : "none");
	}
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) return usage();
	const char *c = argv[1];
	if (!strcmp(c, "cores")) {
		char p[300], run[64];
		printf("memory card core:   %s\n", ss1_core_installed(SS1_MC_PREFIX, p, sizeof p) ? "not installed" : p);
		printf("controller tester:  %s\n", ss1_core_installed(SS1_PAD_PREFIX, p, sizeof p) ? "not installed" : p);
		ss1_core_running(run, sizeof run);
		printf("running:            %s\n", run[0] ? run : "?");
		return 0;
	}
	if (!strcmp(c, "mc") && argc >= 4) {
		if (!strcmp(argv[2], "saves")) return saves(argv[3]);
		ss1_job j = !strcmp(argv[2], "backup") ? SS1_JOB_MC_BACKUP : !strcmp(argv[2], "verify") ? SS1_JOB_MC_VERIFY
		          : !strcmp(argv[2], "write") ? SS1_JOB_MC_WRITE : (ss1_job)-1;
		if ((int)j < 0) return usage();
		if (ss1_job_start(j, argv[3], 0, NULL, 0)) { fprintf(stderr, "couldn't start ss1jobd (%s)\n", SS1_JOBD); return 1; }
		printf("started: the core takes over the TV; ss1mod result shows how it went\n");
		return 0;
	}
	if (!strcmp(c, "pad")) {
		if (ss1_job_start(SS1_JOB_PADTEST, NULL, 0, NULL, 0)) { fprintf(stderr, "couldn't start ss1jobd (%s)\n", SS1_JOBD); return 1; }
		printf("started: hold Select+Start for 2 seconds on a controller to go back\n");
		return 0;
	}
	if (!strcmp(c, "pad-decode") && argc >= 3) return pad_decode(argv[2]);
	if (!strcmp(c, "result")) {
		static const char *keys[] = { "job", "state", "message", "result", "card", "ok", "retried", "failed", "differ", "written", "file", "at", "error", NULL };
		char v[512];
		int any = 0;
		for (int i = 0; keys[i]; i++) if (!ss1_job_result(keys[i], v, sizeof v)) { printf("%s=%s\n", keys[i], v); any = 1; }
		if (!any) printf("no job has run yet\n");
		return 0;
	}
	if (!strcmp(c, "repair") && argc >= 3) {
		static char out[16384];
		if (ss1_repair_run(argv[2], argc > 3 ? argv[3] : NULL, out, sizeof out)) { fprintf(stderr, "the repair script didn't run\n"); return 1; }
		fputs(out, stdout);
		return 0;
	}
	return usage();
}
