/* The apps' remote control: a virtual keyboard and an Xbox 360-style controller on the SuperStation, made with
 * uinput when the app first presses something and closed again after 3 minutes without a press. MiSTer sees
 * them like any USB keyboard and controller (Console Mode's menu, MiSTer's menu, the cores).
 * Copyright (C) 2026 f3bandit. PolyForm Noncommercial 1.0.0 (see ../LICENSE.md); Retro Remake: see ../LICENSE-RETRO-REMAKE.md.
 *
 *   {cmd:"vinput"}                                    {on, keyboard, pad}: the devices are there
 *   {cmd:"vinput", key:<evdev code>, do:"tap"|"down"|"up"[, shift:true]}
 *   {cmd:"vinput", pad:"a"|"b"|"x"|"y"|"lb"|"rb"|"back"|"start"|"guide"|"up"|"down"|"left"|"right", do:...}
 *   {cmd:"vinput", text:"..."}                        types it (letters, digits, space and US punctuation)
 *   {cmd:"vinput", do:"release"}                      lets go of everything held
 *   {cmd:"vinput", do:"close"}                        removes the devices now
 * A key or button held ("down") is let go by itself after 10 s without its "up", in case the app went away.
 */
#include "vinput.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t vmu = PTHREAD_MUTEX_INITIALIZER;
static int kfd = -1, pfd = -1;
static time_t last_use;
static int kheld[KEY_MAX + 1], pheld[16];
static time_t kheld_at[KEY_MAX + 1], pheld_at[16];
static int watcher;

static const char *uinput_path(void) { const char *p = getenv("CMOD_UINPUT"); return p && *p ? p : "/dev/uinput"; }

static void emit(int fd, int type, int code, int val)
{
	if (fd < 0) return;
	struct input_event e;
	memset(&e, 0, sizeof e);
	e.type = (unsigned short)type;
	e.code = (unsigned short)code;
	e.value = val;
	if (write(fd, &e, sizeof e) < 0) { /* the device went away: made again on the next press */ }
}
static void syn(int fd) { emit(fd, EV_SYN, SYN_REPORT, 0); }

/* the legacy uinput_user_dev setup (MiSTer's kernel takes it) */
static int dev_create(int fd, const char *name, unsigned short vendor, unsigned short product, int pad)
{
	struct uinput_user_dev u;
	memset(&u, 0, sizeof u);
	snprintf(u.name, sizeof u.name, "%s", name);
	u.id.bustype = BUS_USB;
	u.id.vendor = vendor;
	u.id.product = product;
	u.id.version = 1;
	if (pad) {
		u.absmin[ABS_X] = -32768; u.absmax[ABS_X] = 32767;
		u.absmin[ABS_Y] = -32768; u.absmax[ABS_Y] = 32767;
		u.absmin[ABS_HAT0X] = -1; u.absmax[ABS_HAT0X] = 1;
		u.absmin[ABS_HAT0Y] = -1; u.absmax[ABS_HAT0Y] = 1;
	}
	if (write(fd, &u, sizeof u) != (ssize_t)sizeof u) return -1;
	/* tests: CMOD_UINPUT is a plain file (no ioctls), the events are written to it */
	return ioctl(fd, UI_DEV_CREATE) && !getenv("CMOD_UINPUT") ? -1 : 0;
}

static int open_uinput(void)
{
	int fd = open(uinput_path(), O_WRONLY | O_NONBLOCK | (getenv("CMOD_UINPUT") ? O_APPEND : 0));
	if (fd < 0 && errno == ENOENT && !getenv("CMOD_UINPUT")) {
		if (system("modprobe uinput >/dev/null 2>&1") == 0) usleep(300000);
		fd = open(uinput_path(), O_WRONLY | O_NONBLOCK);
	}
	return fd;
}

static void close_all(void)
{
	for (int k = 0; k <= KEY_MAX; k++) if (kheld[k]) { emit(kfd, EV_KEY, k, 0); kheld[k] = 0; }
	syn(kfd);
	if (kfd >= 0) { ioctl(kfd, UI_DEV_DESTROY); close(kfd); kfd = -1; }
	if (pfd >= 0) { ioctl(pfd, UI_DEV_DESTROY); close(pfd); pfd = -1; }
	memset(pheld, 0, sizeof pheld);
}

static void pad_set(int i, int down);

static void *idle_watch(void *a)
{
	(void)a;
	for (;;) {
		sleep(2);
		pthread_mutex_lock(&vmu);
		time_t now = time(NULL);
		/* a held key or button whose "up" never came */
		for (int k = 0; k <= KEY_MAX; k++) if (kheld[k] && now - kheld_at[k] > 10) { emit(kfd, EV_KEY, k, 0); syn(kfd); kheld[k] = 0; }
		for (int i = 0; i < 13; i++) if (pheld[i] && now - pheld_at[i] > 10) pad_set(i, 0);
		if ((kfd >= 0 || pfd >= 0) && now - last_use > 180) close_all();
		pthread_mutex_unlock(&vmu);
	}
	return NULL;
}

/* the devices, made when first needed; 0 when both are there */
static int ensure(const char **err)
{
	if (!watcher) {
		pthread_t t;
		if (pthread_create(&t, NULL, idle_watch, NULL) == 0) { pthread_detach(t); watcher = 1; }
	}
	last_use = time(NULL);
	if (kfd >= 0 && pfd >= 0) return 0;
	int made = 0;
	if (kfd < 0) {
		int fd = open_uinput();
		if (fd < 0) { *err = "the SuperStation has no virtual input (uinput)"; return -1; }
		ioctl(fd, UI_SET_EVBIT, EV_KEY);
		ioctl(fd, UI_SET_EVBIT, EV_SYN);
		for (int k = 1; k < 256; k++) ioctl(fd, UI_SET_KEYBIT, k);
		if (dev_create(fd, "Console Mode Remote Keyboard", 0x1d6b, 0x0104, 0)) { close(fd); *err = "couldn't make the remote keyboard"; return -1; }
		kfd = fd;
		made = 1;
	}
	if (pfd < 0) {
		int fd = open_uinput();
		if (fd < 0) { *err = "the SuperStation has no virtual input (uinput)"; return -1; }
		ioctl(fd, UI_SET_EVBIT, EV_KEY);
		ioctl(fd, UI_SET_EVBIT, EV_ABS);
		ioctl(fd, UI_SET_EVBIT, EV_SYN);
		static const int B[] = { BTN_A, BTN_B, BTN_X, BTN_Y, BTN_TL, BTN_TR, BTN_SELECT, BTN_START, BTN_MODE };
		for (size_t i = 0; i < sizeof B / sizeof B[0]; i++) ioctl(fd, UI_SET_KEYBIT, B[i]);
		static const int A[] = { ABS_X, ABS_Y, ABS_HAT0X, ABS_HAT0Y };
		for (size_t i = 0; i < sizeof A / sizeof A[0]; i++) ioctl(fd, UI_SET_ABSBIT, A[i]);
		/* an Xbox 360 pad's ids: MiSTer and Console Mode map it without asking */
		if (dev_create(fd, "Microsoft X-Box 360 pad", 0x045e, 0x028e, 1)) { close(fd); *err = "couldn't make the remote controller"; return -1; }
		pfd = fd;
		made = 1;
	}
	if (made) usleep(1200000);   /* MiSTer picks up new input devices about once a second */
	return 0;
}

/* the controller's buttons: name -> BTN code, or a D-pad direction (hat axis, value) */
static const struct { const char *n; int code, axis, val; } PADB[] = {
	{ "a", BTN_A, -1, 0 }, { "b", BTN_B, -1, 0 }, { "x", BTN_X, -1, 0 }, { "y", BTN_Y, -1, 0 },
	{ "lb", BTN_TL, -1, 0 }, { "rb", BTN_TR, -1, 0 }, { "back", BTN_SELECT, -1, 0 }, { "start", BTN_START, -1, 0 },
	{ "guide", BTN_MODE, -1, 0 },
	{ "up", 0, ABS_HAT0Y, -1 }, { "down", 0, ABS_HAT0Y, 1 }, { "left", 0, ABS_HAT0X, -1 }, { "right", 0, ABS_HAT0X, 1 },
};

static void pad_set(int i, int down)
{
	if (PADB[i].axis >= 0) emit(pfd, EV_ABS, PADB[i].axis, down ? PADB[i].val : 0);
	else emit(pfd, EV_KEY, PADB[i].code, down);
	syn(pfd);
	pheld[i] = down;
	pheld_at[i] = time(NULL);
}

static void key_set(int code, int down)
{
	emit(kfd, EV_KEY, code, down);
	syn(kfd);
	kheld[code] = down;
	kheld_at[code] = time(NULL);
}

/* US layout: the key for a character, and whether it needs Shift */
static int char_key(unsigned char c, int *shift)
{
	static const char *row1 = "1234567890", *row1s = "!@#$%^&*()";
	static const int digits[] = { KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0 };
	static const int letters[] = { KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J, KEY_K, KEY_L, KEY_M,
	                               KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z };
	static const struct { char c, s; int k; } P[] = {
		{ '-', '_', KEY_MINUS }, { '=', '+', KEY_EQUAL }, { '[', '{', KEY_LEFTBRACE }, { ']', '}', KEY_RIGHTBRACE },
		{ '\\', '|', KEY_BACKSLASH }, { ';', ':', KEY_SEMICOLON }, { '\'', '"', KEY_APOSTROPHE }, { '`', '~', KEY_GRAVE },
		{ ',', '<', KEY_COMMA }, { '.', '>', KEY_DOT }, { '/', '?', KEY_SLASH },
	};
	*shift = 0;
	if (c >= 'a' && c <= 'z') return letters[c - 'a'];
	if (c >= 'A' && c <= 'Z') { *shift = 1; return letters[c - 'A']; }
	for (int i = 0; i < 10; i++) { if (c == (unsigned char)row1[i]) return digits[i]; if (c == (unsigned char)row1s[i]) { *shift = 1; return digits[i]; } }
	for (size_t i = 0; i < sizeof P / sizeof P[0]; i++) { if (c == (unsigned char)P[i].c) return P[i].k; if (c == (unsigned char)P[i].s) { *shift = 1; return P[i].k; } }
	if (c == ' ') return KEY_SPACE;
	if (c == '\n') return KEY_ENTER;
	if (c == '\t') return KEY_TAB;
	return 0;
}

cJSON *vinput_command(cJSON *req, const char **err)
{
	const char *d = cJSON_GetStringValue(cJSON_GetObjectItem(req, "do"));
	const char *padn = cJSON_GetStringValue(cJSON_GetObjectItem(req, "pad"));
	const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(req, "text"));
	cJSON *keyj = cJSON_GetObjectItem(req, "key");
	int shift = cJSON_IsTrue(cJSON_GetObjectItem(req, "shift"));
	int down = d && !strcmp(d, "down"), up = d && !strcmp(d, "up"), tapping = !d || !strcmp(d, "tap");
	pthread_mutex_lock(&vmu);
	cJSON *o = NULL;
	if (d && !strcmp(d, "close")) { close_all(); goto reply; }
	if (d && !strcmp(d, "release")) {
		for (int k = 0; k <= KEY_MAX; k++) if (kheld[k]) key_set(k, 0);
		for (size_t i = 0; i < sizeof PADB / sizeof PADB[0]; i++) if (pheld[i]) pad_set((int)i, 0);
		goto reply;
	}
	if (!padn && !text && !cJSON_IsNumber(keyj)) goto reply;   /* just the state */
	if (!down && !up && !tapping) { *err = "do: tap, down, up, release or close"; goto out; }
	if (ensure(err)) goto out;
	if (padn) {
		int i = -1;
		for (size_t j = 0; j < sizeof PADB / sizeof PADB[0]; j++) if (!strcmp(PADB[j].n, padn)) i = (int)j;
		if (i < 0) { *err = "pad: a, b, x, y, lb, rb, back, start, guide, up, down, left or right"; goto out; }
		if (down || tapping) pad_set(i, 1);
		if (tapping) usleep(80000);
		if (up || tapping) pad_set(i, 0);
	} else if (text) {
		if (strlen(text) > 256) { *err = "text: up to 256 characters"; goto out; }
		for (const unsigned char *c = (const unsigned char *)text; *c; c++) {
			int sh, k = char_key(*c, &sh);
			if (!k) continue;
			if (sh) key_set(KEY_LEFTSHIFT, 1);
			key_set(k, 1);
			usleep(15000);
			key_set(k, 0);
			if (sh) key_set(KEY_LEFTSHIFT, 0);
			usleep(10000);
		}
	} else {
		int k = keyj->valueint;
		if (k < 1 || k > 255) { *err = "key: an evdev key code (1-255)"; goto out; }
		if (shift && (down || tapping)) key_set(KEY_LEFTSHIFT, 1);
		if (down || tapping) key_set(k, 1);
		if (tapping) usleep(30000);
		if (up || tapping) key_set(k, 0);
		if (shift && (up || tapping)) key_set(KEY_LEFTSHIFT, 0);
	}
reply:
	o = cJSON_CreateObject();
	cJSON_AddBoolToObject(o, "ok", 1);
	cJSON_AddBoolToObject(o, "keyboard", kfd >= 0);
	cJSON_AddBoolToObject(o, "pad", pfd >= 0);
out:
	pthread_mutex_unlock(&vmu);
	return o;
}
