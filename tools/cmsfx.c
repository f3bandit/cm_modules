/* cmsfx: the level of a system sound (WAV), for the sound-level module.
 * Copyright (C) 2026 f3bandit. All rights reserved; licensed to Retro Remake (see LICENSE.md).
 *
 *   cmsfx info <file.wav>                    format=pcm bits=16 channels=2 rate=44100 frames=N peak=0.734
 *   cmsfx gain <percent> <in.wav> <out.wav>  in at <percent> (1..400) into out; every other chunk kept as is
 *
 * PCM 8/16/24/32-bit and 32-bit float, plain or WAVE_FORMAT_EXTENSIBLE. Above 100% a soft limiter keeps peaks
 * from clipping: samples up to 0.89 of full scale pass straight, louder ones are bent toward full scale.
 * Exit 0 on success, 2 for a file it doesn't handle (not WAV, compressed WAV), 1 for other errors. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

typedef struct {
	uint8_t *buf;
	size_t len;
	int fmt;  /* 1 PCM, 3 float */
	int bits, ch, rate;
	size_t data, dlen; /* the data chunk's samples */
} wav;

static int load(const char *path, wav *w) {
	memset(w, 0, sizeof *w);
	FILE *f = fopen(path, "rb");
	if (!f) { perror(path); return 1; }
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n < 12 || n > 64L * 1024 * 1024) { fclose(f); fprintf(stderr, "%s: not a WAV file\n", path); return 2; }
	w->buf = malloc((size_t)n);
	w->len = (size_t)n;
	if (!w->buf || fread(w->buf, 1, w->len, f) != w->len) { fclose(f); fprintf(stderr, "%s: can't read it\n", path); return 1; }
	fclose(f);
	if (memcmp(w->buf, "RIFF", 4) || memcmp(w->buf + 8, "WAVE", 4)) { fprintf(stderr, "%s: not a WAV file\n", path); return 2; }
	size_t p = 12;
	int havefmt = 0;
	while (p + 8 <= w->len) {
		uint32_t sz = rd32(w->buf + p + 4);
		size_t body = p + 8;
		if (sz > w->len - body) sz = (uint32_t)(w->len - body); /* a short last chunk: take what's there */
		if (!memcmp(w->buf + p, "fmt ", 4) && sz >= 16) {
			w->fmt = rd16(w->buf + body);
			w->ch = rd16(w->buf + body + 2);
			w->rate = (int)rd32(w->buf + body + 4);
			w->bits = rd16(w->buf + body + 14);
			if (w->fmt == 0xFFFE && sz >= 26) w->fmt = rd16(w->buf + body + 24); /* extensible: the subformat */
			havefmt = 1;
		} else if (!memcmp(w->buf + p, "data", 4)) {
			w->data = body;
			w->dlen = sz;
		}
		p = body + sz + (sz & 1);
	}
	if (!havefmt || !w->data) { fprintf(stderr, "%s: no fmt or data chunk\n", path); return 2; }
	if (!(w->fmt == 1 && (w->bits == 8 || w->bits == 16 || w->bits == 24 || w->bits == 32)) && !(w->fmt == 3 && w->bits == 32)) {
		fprintf(stderr, "%s: compressed or unusual WAV (format %d, %d bits)\n", path, w->fmt, w->bits);
		return 2;
	}
	if (w->ch < 1) { fprintf(stderr, "%s: no channels\n", path); return 2; }
	return 0;
}

static double get(const wav *w, const uint8_t *s) {
	switch (w->bits) {
	case 8: return (s[0] - 128) / 128.0;
	case 16: return (int16_t)rd16(s) / 32768.0;
	case 24: { int32_t v = s[0] | s[1] << 8 | s[2] << 16; if (v & 0x800000) v -= 0x1000000; return v / 8388608.0; }
	default:
		if (w->fmt == 3) { float f; memcpy(&f, s, 4); return f; }
		return (int32_t)rd32(s) / 2147483648.0;
	}
}

static void put(const wav *w, uint8_t *s, double x) {
	if (x > 1) x = 1;
	if (x < -1) x = -1;
	switch (w->bits) {
	case 8: { long v = lround(x * 127.0) + 128; s[0] = (uint8_t)v; break; }
	case 16: { long v = lround(x * 32767.0); s[0] = (uint8_t)v; s[1] = (uint8_t)(v >> 8); break; }
	case 24: { long v = lround(x * 8388607.0); s[0] = (uint8_t)v; s[1] = (uint8_t)(v >> 8); s[2] = (uint8_t)(v >> 16); break; }
	default:
		if (w->fmt == 3) { float f = (float)x; memcpy(s, &f, 4); break; }
		{ long long v = llround(x * 2147483647.0); uint32_t u = (uint32_t)v; s[0] = (uint8_t)u; s[1] = (uint8_t)(u >> 8); s[2] = (uint8_t)(u >> 16); s[3] = (uint8_t)(u >> 24); }
	}
}

/* straight up to the knee, then bent smoothly toward full scale: never clips */
static double limit(double x) {
	const double knee = 0.89;
	double a = fabs(x);
	if (a <= knee) return x;
	double y = knee + (1 - knee) * tanh((a - knee) / (1 - knee));
	return x < 0 ? -y : y;
}

int main(int argc, char **argv) {
	if (argc == 3 && !strcmp(argv[1], "info")) {
		wav w;
		int r = load(argv[2], &w);
		if (r) return r;
		size_t bps = (size_t)w.bits / 8, n = w.dlen / bps;
		double peak = 0;
		for (size_t i = 0; i < n; i++) { double v = fabs(get(&w, w.buf + w.data + i * bps)); if (v > peak) peak = v; }
		printf("format=%s bits=%d channels=%d rate=%d frames=%zu peak=%.3f\n", w.fmt == 3 ? "float" : "pcm", w.bits, w.ch, w.rate, n / (size_t)w.ch, peak);
		return 0;
	}
	if (argc == 5 && !strcmp(argv[1], "gain")) {
		char *end;
		long pct = strtol(argv[2], &end, 10);
		if (*end || pct < 1 || pct > 400) { fprintf(stderr, "the level is 1 to 400 (percent)\n"); return 1; }
		wav w;
		int r = load(argv[3], &w);
		if (r) return r;
		double g = pct / 100.0;
		size_t bps = (size_t)w.bits / 8, n = w.dlen / bps;
		for (size_t i = 0; i < n; i++) {
			uint8_t *s = w.buf + w.data + i * bps;
			double x = get(&w, s) * g;
			put(&w, s, g > 1 ? limit(x) : x);
		}
		char tmp[4096];
		snprintf(tmp, sizeof tmp, "%s.cmsfx-tmp", argv[4]);
		FILE *f = fopen(tmp, "wb");
		if (!f) { perror(tmp); return 1; }
		if (fwrite(w.buf, 1, w.len, f) != w.len || fflush(f) || fclose(f)) { remove(tmp); fprintf(stderr, "%s: write failed\n", tmp); return 1; }
		if (rename(tmp, argv[4])) { perror(argv[4]); remove(tmp); return 1; }
		return 0;
	}
	fprintf(stderr, "usage: cmsfx info <file.wav> | cmsfx gain <percent> <in.wav> <out.wav>\n");
	return 1;
}
