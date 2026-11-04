/* cmpair: the pairing screen on the TV, drawn the way Console Mode draws its own (black, white text in its Akrobat
 * font, the title and clock along the top, the breadcrumb and button hints along the bottom): the 6-digit code the
 * PC or phone app asks for, the SuperStation's address and the time left. Until the app pairs, two minutes go by,
 * or any button or key is pressed. Pair_Console_Mode_App.sh --tv runs it over Console Mode's own screen, with
 * Console Mode's drawing paused: it keeps a copy of what was on the screen and puts it back when it's done.
 *
 *   cmpair                 the screen (a new code from the app link)
 *   cmpair --png out.png   one frame into a PNG instead (testing: --code, --left, --paired NAME, --addr A)
 *   cmpair --vt-free N [M] Console Mode's text screen N back to normal after it was closed from outside (it leaves
 *                          the screen in its own switching mode, locked), then text screen M (1, MiSTer's desktop)
 *
 * Copyright (C) 2026 f3bandit. PolyForm Noncommercial 1.0.0 (see ../LICENSE.md).
 * stb_truetype and stb_image_write (Sean Barrett) are public domain / MIT. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <dirent.h>
#include <linux/input.h>
#include <poll.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "../remote/vendor/stb_truetype.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../remote/vendor/stb_image_write.h"

static const char *CM = "/media/fat/ConsoleMode";
static int W = 1280, H = 720;
static uint32_t *px;              /* the frame, 0xRRGGBB */

/* ---------- drawing ---------- */

static void fill(int x, int y, int w, int h, uint32_t c)
{
	for (int j = y < 0 ? 0 : y; j < y + h && j < H; j++)
		for (int i = x < 0 ? 0 : x; i < x + w && i < W; i++) px[j * W + i] = c;
}

static uint32_t mix(uint32_t a, uint32_t b, int t)   /* t 0..255 of b over a */
{
	int r = ((a >> 16 & 255) * (255 - t) + (b >> 16 & 255) * t) / 255;
	int g = ((a >> 8 & 255) * (255 - t) + (b >> 8 & 255) * t) / 255;
	int l = ((a & 255) * (255 - t) + (b & 255) * t) / 255;
	return (uint32_t)(r << 16 | g << 8 | l);
}

/* a rounded rectangle, its edges smoothed */
static void rrect(int x, int y, int w, int h, int r, uint32_t c)
{
	for (int j = 0; j < h; j++)
		for (int i = 0; i < w; i++) {
			float dx = 0, dy = 0;
			if (i < r) dx = r - i - .5f; else if (i >= w - r) dx = i - (w - r) + .5f;
			if (j < r) dy = r - j - .5f; else if (j >= h - r) dy = j - (h - r) + .5f;
			float d = sqrtf(dx * dx + dy * dy) - r;
			int a = d <= -1 ? 255 : d >= 0 ? 0 : (int)(-d * 255);
			if (!a || x + i < 0 || x + i >= W || y + j < 0 || y + j >= H) continue;
			uint32_t *p = &px[(y + j) * W + x + i];
			*p = mix(*p, c, a);
		}
}

static void disc(int cx, int cy, int r, uint32_t c) { rrect(cx - r, cy - r, 2 * r, 2 * r, r, c); }

typedef struct { stbtt_fontinfo f; unsigned char *data; int ok; } font_t;
static font_t reg, bold;

static int font_load(font_t *f, const char *path)
{
	FILE *fp = fopen(path, "rb");
	if (!fp) return -1;
	fseek(fp, 0, SEEK_END);
	long n = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	f->data = malloc((size_t)n);
	if (!f->data || fread(f->data, 1, (size_t)n, fp) != (size_t)n) { fclose(fp); return -1; }
	fclose(fp);
	f->ok = stbtt_InitFont(&f->f, f->data, stbtt_GetFontOffsetForIndex(f->data, 0));
	return f->ok ? 0 : -1;
}

/* the width of s at size px (with extra space between letters) */
static int text_w(font_t *f, const char *s, float size, float track)
{
	float sc = stbtt_ScaleForPixelHeight(&f->f, size), x = 0;
	for (const char *c = s; *c; c++) {
		int adv, lsb;
		stbtt_GetCodepointHMetrics(&f->f, (unsigned char)*c, &adv, &lsb);
		x += adv * sc + track;
		if (c[1]) x += stbtt_GetCodepointKernAdvance(&f->f, (unsigned char)*c, (unsigned char)c[1]) * sc;
	}
	return (int)(x - (*s ? track : 0) + .5f);
}

/* s with its baseline at y, starting at x (align: 0 left, 1 centred on x, 2 ending at x) */
static void text(font_t *f, const char *s, float size, int x, int y, uint32_t col, int align, float track)
{
	if (!f->ok) return;
	if (align == 1) x -= text_w(f, s, size, track) / 2;
	else if (align == 2) x -= text_w(f, s, size, track);
	float sc = stbtt_ScaleForPixelHeight(&f->f, size), pen = (float)x;
	for (const char *c = s; *c; c++) {
		int cp = (unsigned char)*c, adv, lsb, w, h, xo, yo;
		stbtt_GetCodepointHMetrics(&f->f, cp, &adv, &lsb);
		float sx = pen - floorf(pen);
		unsigned char *bm = stbtt_GetCodepointBitmapSubpixel(&f->f, sc, sc, sx, 0, cp, &w, &h, &xo, &yo);
		int bx = (int)floorf(pen) + xo, by = y + yo;
		for (int j = 0; j < h; j++)
			for (int i = 0; i < w; i++) {
				int a = bm[j * w + i], X = bx + i, Y = by + j;
				if (a && X >= 0 && X < W && Y >= 0 && Y < H) px[Y * W + X] = mix(px[Y * W + X], col, a);
			}
		stbtt_FreeBitmap(bm, NULL);
		pen += adv * sc + track;
		if (c[1]) pen += stbtt_GetCodepointKernAdvance(&f->f, cp, (unsigned char)c[1]) * sc;
	}
}

/* a button hint the way Console Mode draws one: a white disc with the letter in black, then its label */
static int hint(int xr, int y, const char *letter, const char *label)
{
	int lw = text_w(&reg, label, 23, 0);
	text(&reg, label, 23, xr, y, 0xffffff, 2, 0);
	int cx = xr - lw - 8 - 9;
	disc(cx, y - 8, 9, 0xffffff);
	text(&bold, letter, 15, cx, y - 3, 0x000000, 1, 0);
	return cx - 9 - 22;
}

/* ---------- the screen ---------- */

#define WHITE 0xffffff
#define MUTED 0xb8b8b8
#define QUIET 0x6e6e6e
#define BLUE  0x2d6cdf      /* Console Mode's progress blue */

typedef struct { char code[8], paired[160], addr[64]; int left, total, gone; } state_t;

static void draw(const state_t *s)
{
	fill(0, 0, W, H, 0x000000);
	/* the top line: the title, and the clock */
	text(&reg, "Console Mode", 23, 60, 44, WHITE, 0, 0);
	char clk[40];
	time_t now = time(NULL);
	struct tm t;
	localtime_r(&now, &t);
	strftime(clk, sizeof(clk), "%b %d %I:%M %p", &t);
	if (clk[7] == '0') memmove(clk + 7, clk + 8, strlen(clk + 8) + 1);   /* 09:41 -> 9:41 */
	text(&reg, clk, 23, W - 60, 44, WHITE, 2, 0);
	/* the bottom line: where this is, and the button */
	text(&reg, "Settings / Pair a device", 23, 60, H - 30, WHITE, 0, 0);
	if (s->paired[0] || s->gone) hint(W - 60, H - 30, "A", "OK");
	else hint(W - 60, H - 30, "B", "Back");

	if (s->paired[0]) {
		disc(W / 2, 260, 44, WHITE);
		/* a check mark in the disc */
		for (int k = -2; k <= 2; k++) {
			for (int i = 0; i < 18; i++) fill(W / 2 - 22 + i, 262 + i + k, 3, 3, 0x000000);
			for (int i = 0; i < 34; i++) fill(W / 2 - 5 + i, 279 - i + k, 3, 3, 0x000000);
		}
		text(&bold, "Paired", 46, W / 2, 380, WHITE, 1, 0);
		text(&reg, s->paired, 26, W / 2, 424, MUTED, 1, 0);
		return;
	}
	if (s->gone) {
		text(&bold, "The code ran out", 46, W / 2, 320, WHITE, 1, 0);
		text(&reg, "Choose the SuperStation in the app again for a new one.", 26, W / 2, 366, MUTED, 1, 0);
		return;
	}
	text(&bold, "Pair a PC or phone", 46, W / 2, 196, WHITE, 1, 0);
	text(&reg, "In Console Mode for Windows or the phone app, type this code.", 26, W / 2, 240, MUTED, 1, 0);

	/* the code: six tiles, three and three, the selection's white rounding */
	int tw = 92, th = 124, gap = 14, mid = 40, total = 6 * tw + 4 * gap + mid, x0 = (W - total) / 2, y0 = 284;
	for (int i = 0; i < 6; i++) {
		int x = x0 + i * (tw + gap) + (i >= 3 ? mid - gap : 0);
		rrect(x, y0, tw, th, 12, 0x1a1a1a);
		rrect(x + 1, y0 + 1, tw - 2, th - 2, 11, 0x1a1a1a);
		char d[2] = { s->code[i], 0 };
		text(&bold, d, 104, x + tw / 2, y0 + th - 24, WHITE, 1, 0);
	}
	if (s->addr[0]) {
		char a[96];
		snprintf(a, sizeof(a), "SuperStation  %s", s->addr);
		text(&reg, a, 24, W / 2, 470, MUTED, 1, 0);
	} else text(&reg, "Not on a network yet: connect WiFi or a cable in Settings.", 24, W / 2, 470, MUTED, 1, 0);

	/* the time left, as Console Mode's progress bar */
	int bw = 420, bx = (W - bw) / 2, by = 512;
	rrect(bx, by, bw, 8, 4, 0x2a2a2a);
	int f = s->total > 0 ? bw * s->left / s->total : 0;
	if (f > 8) rrect(bx, by, f, 8, 4, BLUE);
	char l[48];
	snprintf(l, sizeof(l), "The code works for %d:%02d", s->left / 60, s->left % 60);
	text(&reg, l, 22, W / 2, 552, QUIET, 1, 0);
}

/* ---------- the app link ---------- */

static char tools[300];

static int run_cmremote(const char *arg, char *out, size_t n)
{
	char cmd[400];
	snprintf(cmd, sizeof(cmd), "%s/cmremote %s 2>/dev/null", tools, arg);
	FILE *p = popen(cmd, "r");
	if (!p) return -1;
	size_t k = fread(out, 1, n - 1, p);
	out[k] = 0;
	return pclose(p);
}

static void kv(const char *t, const char *key, char *out, size_t n)
{
	out[0] = 0;
	size_t kl = strlen(key);
	for (const char *l = t; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL)
		if (!strncmp(l, key, kl) && l[kl] == '=') {
			const char *e = strchr(l, '\n');
			size_t m = e ? (size_t)(e - l - kl - 1) : strlen(l + kl + 1);
			if (m >= n) m = n - 1;
			memcpy(out, l + kl + 1, m);
			out[m] = 0;
			return;
		}
}

static void read_state(state_t *s)
{
	static char buf[8192];
	char v[160];
	if (run_cmremote("info", buf, sizeof(buf)) < 0) return;
	kv(buf, "code", v, sizeof(v));
	if (strlen(v) == 6) memcpy(s->code, v, 7);
	else if (!s->paired[0]) s->gone = 1;
	kv(buf, "code_left", v, sizeof(v));
	s->left = atoi(v);
	kv(buf, "paired", v, sizeof(v));
	if (v[0]) { snprintf(s->paired, sizeof(s->paired), "%s", v); s->gone = 0; }
	kv(buf, "address", v, sizeof(v));
	char *c = strrchr(v, ':');
	if (c) *c = 0;
	snprintf(s->addr, sizeof(s->addr), "%s", v);
}

/* ---------- the frame buffer ---------- */

static int fbfd = -1, ttyfd = -1;
static uint8_t *fbmem;
static struct fb_var_screeninfo vi;
static struct fb_fix_screeninfo fi;
static struct termios old_tio;
static int have_tio;

static uint8_t *saved;          /* what was on the screen before: put back at the end */
static size_t saved_n;

static void restore(void)
{
	if (saved && fbmem) { memcpy(fbmem, saved, saved_n); free(saved); saved = NULL; }
	if (ttyfd >= 0) { ioctl(ttyfd, KDSETMODE, KD_TEXT); if (have_tio) tcsetattr(ttyfd, TCSANOW, &old_tio); ttyfd = -1; }
}
static void on_sig(int s) { (void)s; restore(); _exit(0); }

static void blit(void)
{
	int bpp = (int)vi.bits_per_pixel / 8;
	for (int y = 0; y < H && y < (int)vi.yres; y++) {
		uint8_t *row = fbmem + (size_t)(y + vi.yoffset) * fi.line_length;
		for (int x = 0; x < W && x < (int)vi.xres; x++) {
			uint32_t c = px[y * W + x], r = c >> 16 & 255, g = c >> 8 & 255, b = c & 255;
			uint32_t v = (r >> (8 - vi.red.length)) << vi.red.offset | (g >> (8 - vi.green.length)) << vi.green.offset | (b >> (8 - vi.blue.length)) << vi.blue.offset;
			if (vi.transp.length) v |= ((1u << vi.transp.length) - 1) << vi.transp.offset;
			if (bpp == 4) memcpy(row + x * 4, &v, 4);
			else if (bpp == 2) { uint16_t h = (uint16_t)v; memcpy(row + x * 2, &h, 2); }
		}
	}
}

/* any button or key, on any controller or keyboard */
static int evfd[32], nev;

static void ev_open(void)
{
	DIR *d = opendir("/dev/input");
	struct dirent *e;
	while (d && (e = readdir(d)) && nev < 32) {
		if (strncmp(e->d_name, "event", 5)) continue;
		char p[300];
		snprintf(p, sizeof(p), "/dev/input/%s", e->d_name);
		int fd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd >= 0) evfd[nev++] = fd;
	}
	if (d) closedir(d);
}

static int key_pressed(void)
{
	char c;
	if (ttyfd >= 0 && read(ttyfd, &c, 1) == 1) return 1;
	for (int i = 0; i < nev; i++) {
		struct input_event ev[16];
		ssize_t n;
		while ((n = read(evfd[i], ev, sizeof(ev))) > 0)
			for (size_t k = 0; k < (size_t)n / sizeof(ev[0]); k++)
				if (ev[k].type == EV_KEY && ev[k].value == 1) return 1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *png = NULL;
	state_t s = { "000000", "", "", 120, 120, 0 };
	int test = 0;
	if (argc > 2 && !strcmp(argv[1], "--vt-free")) {
		/* Console Mode, closed from outside, leaves its text screen in its own switching mode and switching
		   locked: put that screen back to normal and go to the one given (MiSTer's desktop is on 1) */
		char t[32];
		snprintf(t, sizeof(t), "/dev/tty%d", atoi(argv[2]));
		int fd = open(t, O_RDWR | O_NOCTTY);
		if (fd < 0) return 1;
		struct vt_mode m = { .mode = VT_AUTO };
		ioctl(fd, VT_SETMODE, &m);
		ioctl(fd, KDSETMODE, KD_TEXT);
		ioctl(fd, VT_UNLOCKSWITCH, 1);
		int to = argc > 3 ? atoi(argv[3]) : 1;
		int r = ioctl(fd, VT_ACTIVATE, to);
		if (!r) ioctl(fd, VT_WAITACTIVE, to);
		return r ? 1 : 0;
	}
	for (int a = 1; a < argc; a++) {
		if (!strcmp(argv[a], "--png") && a + 1 < argc) { png = argv[++a]; test = 1; }
		else if (!strcmp(argv[a], "--code") && a + 1 < argc) snprintf(s.code, sizeof(s.code), "%s", argv[++a]);
		else if (!strcmp(argv[a], "--left") && a + 1 < argc) s.left = atoi(argv[++a]);
		else if (!strcmp(argv[a], "--paired") && a + 1 < argc) snprintf(s.paired, sizeof(s.paired), "%s", argv[++a]);
		else if (!strcmp(argv[a], "--gone")) s.gone = 1;
		else if (!strcmp(argv[a], "--addr") && a + 1 < argc) snprintf(s.addr, sizeof(s.addr), "%s", argv[++a]);
		else if (!strcmp(argv[a], "--fonts") && a + 1 < argc) CM = argv[++a];
	}
	char p[400];
	snprintf(p, sizeof(p), "%s/themeconfig/resources/Akrobat-SemiBold.ttf", CM);
	if (font_load(&reg, p)) { snprintf(p, sizeof(p), "%s/Akrobat-SemiBold.ttf", CM); font_load(&reg, p); }
	snprintf(p, sizeof(p), "%s/themeconfig/resources/Akrobat-Bold.ttf", CM);
	if (font_load(&bold, p)) { snprintf(p, sizeof(p), "%s/Akrobat-Bold.ttf", CM); font_load(&bold, p); }
	if (!bold.ok) bold = reg;
	if (!reg.ok) { fprintf(stderr, "cmpair: no Console Mode font (%s/themeconfig/resources)\n", CM); return 1; }

	if (test) {
		px = calloc((size_t)W * H, 4);
		draw(&s);
		unsigned char *rgb = malloc((size_t)W * H * 3);
		for (int i = 0; i < W * H; i++) { rgb[i * 3] = px[i] >> 16; rgb[i * 3 + 1] = px[i] >> 8; rgb[i * 3 + 2] = px[i]; }
		return !stbi_write_png(png, W, H, 3, rgb, W * 3);
	}

	snprintf(tools, sizeof(tools), "%s/tools", CM);
	fbfd = open("/dev/fb0", O_RDWR);
	if (fbfd < 0 || ioctl(fbfd, FBIOGET_VSCREENINFO, &vi) || ioctl(fbfd, FBIOGET_FSCREENINFO, &fi)) { fprintf(stderr, "cmpair: no frame buffer\n"); return 1; }
	W = (int)vi.xres; H = (int)vi.yres;
	fbmem = mmap(NULL, fi.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
	if (fbmem == MAP_FAILED) { fprintf(stderr, "cmpair: frame buffer: %s\n", strerror(errno)); return 1; }
	px = calloc((size_t)W * H, 4);

	saved_n = fi.smem_len;
	saved = malloc(saved_n);
	if (saved) memcpy(saved, fbmem, saved_n);
	ev_open();
	/* on a text screen of its own: no text drawn over the picture, keys read one at a time */
	ttyfd = isatty(0) ? 0 : -1;
	if (ttyfd >= 0) {
		struct termios t;
		if (!tcgetattr(ttyfd, &old_tio)) {
			have_tio = 1;
			t = old_tio;
			t.c_lflag &= ~(unsigned)(ICANON | ECHO);
			t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 0;
			tcsetattr(ttyfd, TCSANOW, &t);
		}
		tcflush(ttyfd, TCIFLUSH);
		ioctl(ttyfd, KDSETMODE, KD_GRAPHICS);
	}
	atexit(restore);
	signal(SIGTERM, on_sig); signal(SIGINT, on_sig); signal(SIGHUP, on_sig);

	char out[4096];
	run_cmremote("pair", out, sizeof(out));
	read_state(&s);
	s.total = s.left > 0 ? s.left : 120;
	time_t end = time(NULL) + s.total + 5, shown_done = 0;
	char last[400] = "";
	while (time(NULL) < end) {
		read_state(&s);
		char sig[400];
		snprintf(sig, sizeof(sig), "%s|%s|%s|%d|%d|%ld", s.code, s.paired, s.addr, s.left, s.gone, (long)(time(NULL) / 60));
		if (strcmp(sig, last)) { draw(&s); blit(); snprintf(last, sizeof(last), "%s", sig); }   /* only when something changed */
		if ((s.paired[0] || s.gone) && !shown_done) { shown_done = time(NULL); }
		if (shown_done && time(NULL) - shown_done >= 3) break;
		for (int i = 0; i < 4; i++) {
			if (key_pressed()) {
				if (!s.paired[0]) run_cmremote("pair_stop", out, sizeof(out));
				goto done;
			}
			usleep(125000);
		}
	}
	if (!s.paired[0]) run_cmremote("pair_stop", out, sizeof(out));
done:
	restore();
	return 0;
}
