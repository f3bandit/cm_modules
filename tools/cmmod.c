/* cmmod: every module from the command line, the way a front end calls them (and for testing on a SuperStation).
 * Copyright (C) 2026 f3bandit. PolyForm Noncommercial 1.0.0 (see LICENSE.md).
 *
 *   cmmod cores                          which cores are installed, which one is running
 *   cmmod mc backup <image>              back up the card in front port 1 (cmjobd; the front end closes)
 *   cmmod mc verify <image>              check the card against an image
 *   cmmod mc write <image>               write an image to the card
 *   cmmod mc saves <image>               the saves on an image (ps1_memcard_lib)
 *   cmmod pad                            the controller tester (Select+Start 2 s ends it)
 *   cmmod pad-decode <report.bin>        decode a controller tester report file
 *   cmmod result                         the last job's result
 *   cmmod repair <status|setup DRIVE|arm|disarm|restart|result|remove>
 *   cmmod remote <info|on|off|pair|pair_stop|forget ID|rescan>   the app link (starts cmremote if needed)
 *   cmmod api '<JSON>'                   any app-link command ({"cmd":"wifi"}...), its JSON reply
 *   cmmod bt [pair|cancel|forget MAC|disconnect MAC]   Bluetooth controllers (the paired ones first)
 *   cmmod wifi [scan|connect SSID [PASSWORD] [hidden]|forget SSID|up SSID|down SSID|undo]   WiFi (status first)
 */
#include "cmmod.h"
#include "mcformat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int usage(void)
{
	fprintf(stderr, "usage: cmmod cores | mc backup|verify|write|saves <image> | pad | pad-decode <file> | result | repair <cmd> [arg] | remote <cmd> [arg] | bt [pair|forget MAC|disconnect MAC] | wifi [scan|connect SSID [PASSWORD] [hidden]|forget SSID|up SSID|down SSID|undo]\n");
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
	unsigned char b[CMOD_PAD_REPORT_SIZE];
	FILE *f = fopen(path, "rb");
	size_t n = f ? fread(b, 1, sizeof b, f) : 0;
	if (f) fclose(f);
	cmod_pad_report r;
	uint32_t id = n >= 12 ? (uint32_t)b[8] | (uint32_t)b[9] << 8 | (uint32_t)b[10] << 16 | (uint32_t)b[11] << 24 : 0;
	int p = cmod_pad_parse(b, n, id, &r);
	if (p != 1) { printf(p == 0 ? "no report yet (the core hasn't answered)\n" : "not a controller tester report\n"); return 1; }
	for (int i = 0; i < 2; i++) {
		cmod_pad_port *q = &r.port[i];
		printf("port %d: ", i + 1);
		if (!q->present) { printf("no controller"); }
		else {
			printf("%s (ID %02X)", cmod_pad_type(q->id), q->id);
			if (q->analog) printf("  L %3d,%3d  R %3d,%3d", q->lx, q->ly, q->rx, q->ry);
			printf("  pressed:");
			for (int b2 = 0; b2 < 16; b2++) if (q->buttons & (1u << b2)) printf(" %s", cmod_pad_button_name((uint16_t)(1u << b2)));
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
		printf("memory card core:   %s\n", cmod_core_installed(CMOD_MC_PREFIX, p, sizeof p) ? "not installed" : p);
		printf("controller tester:  %s\n", cmod_core_installed(CMOD_PAD_PREFIX, p, sizeof p) ? "not installed" : p);
		cmod_core_running(run, sizeof run);
		printf("running:            %s\n", run[0] ? run : "?");
		return 0;
	}
	if (!strcmp(c, "mc") && argc >= 4) {
		if (!strcmp(argv[2], "saves")) return saves(argv[3]);
		cmod_job j = !strcmp(argv[2], "backup") ? CMOD_JOB_MC_BACKUP : !strcmp(argv[2], "verify") ? CMOD_JOB_MC_VERIFY
		          : !strcmp(argv[2], "write") ? CMOD_JOB_MC_WRITE : (cmod_job)-1;
		if ((int)j < 0) return usage();
		if (cmod_job_start(j, argv[3], 0, NULL, 0)) { fprintf(stderr, "couldn't start cmjobd (%s)\n", CMOD_JOBD); return 1; }
		printf("started: the core takes over the TV; cmmod result shows how it went\n");
		return 0;
	}
	if (!strcmp(c, "pad")) {
		if (cmod_job_start(CMOD_JOB_PADTEST, NULL, 0, NULL, 0)) { fprintf(stderr, "couldn't start cmjobd (%s)\n", CMOD_JOBD); return 1; }
		printf("started: hold Select+Start for 2 seconds on a controller to go back\n");
		return 0;
	}
	if (!strcmp(c, "pad-decode") && argc >= 3) return pad_decode(argv[2]);
	if (!strcmp(c, "result")) {
		static const char *keys[] = { "job", "state", "message", "result", "card", "ok", "retried", "failed", "differ", "written", "file", "at", "error", NULL };
		char v[512];
		int any = 0;
		for (int i = 0; keys[i]; i++) if (!cmod_job_result(keys[i], v, sizeof v)) { printf("%s=%s\n", keys[i], v); any = 1; }
		if (!any) printf("no job has run yet\n");
		return 0;
	}
	if (!strcmp(c, "repair") && argc >= 3) {
		static char out[16384];
		if (cmod_repair_run(argv[2], argc > 3 ? argv[3] : NULL, out, sizeof out)) { fprintf(stderr, "the repair script didn't run\n"); return 1; }
		fputs(out, stdout);
		return 0;
	}
	if (!strcmp(c, "remote") && argc >= 3) {
		static char out[8192];
		char cmd[200];
		snprintf(cmd, sizeof cmd, "%s%s%s", argv[2], argc > 3 ? " " : "", argc > 3 ? argv[3] : "");
		if (cmod_remote_start()) { fprintf(stderr, "cmremote isn't running and didn't start (%s)\n", CMOD_REMOTE); return 1; }
		if (cmod_remote_cmd(cmd, out, sizeof out)) { fprintf(stderr, "cmremote didn't answer\n"); return 1; }
		fputs(out, stdout);
		return 0;
	}
	if (!strcmp(c, "api") && argc == 3) {
		static char out[1 << 20];
		if (cmod_remote_start() || cmod_remote_api(argv[2], out, sizeof out, 60)) { fprintf(stderr, "cmremote didn't answer\n"); return 1; }
		puts(out);
		return 0;
	}
	if (!strcmp(c, "bt")) {
		const char *d = argc > 2 ? argv[2] : "";
		char err[200] = "";
		if (!strcmp(d, "pair")) {
			int r = cmod_bt_pair_start();
			if (r == -1) { fprintf(stderr, "no Bluetooth adapter\n"); return 1; }
			if (r == -2) { fprintf(stderr, "Console Mode (or MiSTer) is pairing already\n"); return 1; }
			if (r == -3) { fprintf(stderr, "SS1 Tool is changing the Bluetooth pairings; try again in a moment\n"); return 1; }
			for (int i = 0; i < 950; i++) {              /* (up to 95 s) */
				cmod_bt_state_t s;
				cmod_bt_state(&s);
				if (!s.busy) break;
				usleep(100000);
			}
		} else if (!strcmp(d, "forget") && argc > 3) { if (cmod_bt_forget(argv[3], err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; } }
		else if (!strcmp(d, "disconnect") && argc > 3) { if (cmod_bt_disconnect(argv[3], err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; } }
		else if (*d) return usage();
		cmod_bt_state_t s;
		cmod_bt_state(&s);
		printf("present=%d\nbusy=%d\nok=%d\nfound=%s\nmessage=%s\n", s.present, s.busy, s.ok, s.found, s.message);
		cmod_bt_dev_t dv[CMOD_BT_MAX];
		cmod_bt_refresh_soon();
		int n = cmod_bt_devices(dv, CMOD_BT_MAX);
		for (int i = 0; i < 50 && n == 0 && s.present; i++) { usleep(100000); n = cmod_bt_devices(dv, CMOD_BT_MAX); }
		for (int i = 0; i < n; i++) printf("device=%s|%s|%s|%s\n", dv[i].mac, dv[i].name, dv[i].connected ? "connected" : "-", dv[i].icon);
		return 0;
	}
	if (!strcmp(c, "wifi")) {
		const char *d = argc > 2 ? argv[2] : "";
		char err[200] = "";
		int rc = 0;
		if (!strcmp(d, "scan")) {
			cmod_wifi_scan_start();
			for (int i = 0; i < 300; i++) {           /* (up to 30 s) */
				int busy;
				time_t at;
				cmod_wifi_net_t n[1];
				cmod_wifi_scan_results(n, 0, &busy, &at, err, sizeof err);
				if (!busy) break;
				usleep(100000);
			}
		} else if (!strcmp(d, "connect") && argc > 3) rc = cmod_wifi_connect(argv[3], argc > 4 ? argv[4] : NULL, argc > 5 && !strcmp(argv[5], "hidden"), err, sizeof err);
		else if (!strcmp(d, "forget") && argc > 3) rc = cmod_wifi_forget(argv[3], err, sizeof err);
		else if ((!strcmp(d, "up") || !strcmp(d, "down")) && argc > 3) rc = cmod_wifi_move(argv[3], !strcmp(d, "up") ? -1 : 1, err, sizeof err);
		else if (!strcmp(d, "undo")) rc = cmod_wifi_undo(err, sizeof err);
		else if (*d) return usage();
		if (rc) { fprintf(stderr, "%s\n", err); return 1; }
		cmod_wifi_status_t st;
		cmod_wifi_status(&st);
		printf("present=%d\nconnected=%d\nssid=%s\nsignal=%d\nfreq=%d\nip=%s\nsupplicant=%d\n", st.present, st.connected, st.ssid, st.signal, st.freq, st.ip, st.supplicant);
		cmod_wifi_saved_t sv[CMOD_WIFI_SAVED_MAX];
		int ns = cmod_wifi_saved(sv, CMOD_WIFI_SAVED_MAX);
		for (int i = 0; i < ns; i++) printf("saved=%s|%s\n", sv[i].ssid, sv[i].open ? "open" : "secure");
		cmod_wifi_net_t nets[CMOD_WIFI_MAX];
		int busy = 0;
		time_t at = 0;
		int nn = cmod_wifi_scan_results(nets, CMOD_WIFI_MAX, &busy, &at, err, sizeof err);
		for (int i = 0; i < nn; i++) printf("network=%s|%d|%s|%s\n", nets[i].ssid, nets[i].signal, nets[i].bands == 3 ? "2.4+5" : nets[i].bands == 2 ? "5" : "2.4", nets[i].secure ? "secure" : "open");
		if (err[0]) printf("scan_error=%s\n", err);
		return 0;
	}
	return usage();
}
