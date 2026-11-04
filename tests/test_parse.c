/* Host tests: the report formats both cores write, built the way their README tables describe.
 * Copyright (C) 2026 f3bandit. PolyForm Noncommercial 1.0.0 (see LICENSE.md). */
#include "cmmod.h"

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); fails++; } } while (0)

int main(void)
{
	/* memory card: request, then the core's progress and report in the same file */
	static uint8_t b[CMOD_MC_JOB_SIZE];
	int force[] = { 0, 5, 1023, 2000 };
	cmod_mc_request(b, CMOD_MC_WRITE, CMOD_MC_STEP_COPY2, 0x12345678, force, 4);
	CHECK(!memcmp(b, "SMCQ", 4) && b[4] == 3 && b[6] == 2 && b[8] == 0x78 && b[11] == 0x12);
	CHECK(b[512] == 1 && b[517] == 1 && b[512 + 1023] == 1 && b[513] == 0);
	cmod_mc_report r;
	CHECK(cmod_mc_parse(b, sizeof b, 0x12345678, &r) == 0);                    /* still the request */
	b[3] = 'P'; b[5] = 3; b[12] = 10; b[22] = 0x34; b[23] = 0x01; b[25] = 3; b[512 + 7] = CMOD_MAP_RETRIED;
	CHECK(cmod_mc_parse(b, sizeof b, 0x12345678, &r) == 1 && !r.finished && r.ok == 10 && r.done == 0x134 && r.phase == 3);
	CHECK(r.map[7] == CMOD_MAP_RETRIED && r.map[0] == 1);
	CHECK(cmod_mc_parse(b, sizeof b, 0x99, &r) == 0);                           /* someone else's job */
	b[3] = 'R'; b[6] = CMOD_MC_WRITTEN; b[7] = 1; b[28] = 7;
	CHECK(cmod_mc_parse(b, sizeof b, 0x12345678, &r) == 1 && r.finished && r.result == 9 && r.card == 1 && r.written == 7);
	CHECK(cmod_mc_parse(b, 100, 0x12345678, &r) == -1);
	CHECK(strstr(cmod_mc_result_text(CMOD_MC_BACKUP, CMOD_MC_NO_CARD), "No card"));

	/* controller tester */
	static uint8_t p[CMOD_PAD_REPORT_SIZE];
	cmod_pad_request(p, 0xCAFE0001);
	cmod_pad_report pr;
	CHECK(!memcmp(p, "SCTQ", 4) && cmod_pad_parse(p, sizeof p, 0xCAFE0001, &pr) == 0);
	p[3] = 'R'; p[4] = 1;
	uint8_t *o = p + 32;                                                      /* port 1: a DualShock, Start + Cross */
	o[0] = 1; o[1] = 1; o[2] = 0x73; o[3] = 0x08; o[4] = 0x40; o[5] = 10; o[6] = 20; o[7] = 128; o[8] = 255; o[9] = 77; o[10] = 1; o[11] = 1;
	p[48] = 2;                                                                /* port 2: nothing */
	p[64] = 0x18;
	CHECK(cmod_pad_parse(p, sizeof p, 0xCAFE0001, &pr) == 1);
	CHECK(pr.port[0].present && pr.port[0].analog && pr.port[0].id == 0x73);
	CHECK(pr.port[0].buttons == (CMOD_PAD_START | CMOD_PAD_CROSS));
	CHECK(pr.port[0].rx == 10 && pr.port[0].ry == 20 && pr.port[0].lx == 128 && pr.port[0].ly == 255);
	CHECK(pr.port[0].card == 1 && pr.port[0].analog_asked && pr.port[0].reads == 77);
	CHECK(!pr.port[1].present && pr.port[1].buttons == 0 && pr.pins_port1 == 0x18);
	CHECK(!strcmp(cmod_pad_type(0x73), "Analog (DualShock)") && !strcmp(cmod_pad_type(0x41), "Digital pad"));
	CHECK(!strcmp(cmod_pad_button_name(CMOD_PAD_TRIANGLE), "Triangle"));

	/* key=value */
	char v[64];
	CHECK(cmod_kv("a=1\narmed=0\nlast_result=repaired\n", "last_result", v, sizeof v) == 0 && !strcmp(v, "repaired"));
	CHECK(cmod_kv("armed=1", "arm", v, sizeof v) == -1);
	CHECK(cmod_kv("drive=x\n", "drive", v, 3) == 0 && !strcmp(v, "x"));

	printf(fails ? "%d failed\n" : "all passed\n", fails);
	return fails != 0;
}
