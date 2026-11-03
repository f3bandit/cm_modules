/* cmremote: the Diagnostics dashboard's numbers for Console Mode for Windows. Read only, from
 * /proc and /sys, nothing kept: counters are cumulative, the app takes the differences between two
 * asks. Temperatures, CPU (all and each core), memory, uptime and load, the running core, drives,
 * network addresses and traffic, disk traffic, the busiest processes, and the kernel log's last lines.
 * Copyright (C) 2026 f3bandit. GPL-3.0-or-later (see ../LICENSE); Retro Remake: see ../LICENSE-RETRO-REMAKE.md. */
#define _GNU_SOURCE
#include "dash.h"
#include "jobs.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/klog.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

static long read_long(const char *path, int *ok)
{
	FILE *f = fopen(path, "r");
	long v = 0;
	*ok = f && fscanf(f, "%ld", &v) == 1;
	if (f) fclose(f);
	return v;
}

static void read_line(const char *path, char *out, size_t n)
{
	out[0] = 0;
	FILE *f = fopen(path, "r");
	if (!f) return;
	if (fgets(out, (int)n, f)) out[strcspn(out, "\r\n")] = 0;
	fclose(f);
}

/* the SuperStation has no CPU temperature sensor: the NVMe drive in the dock is the one temperature
   ("temps" is empty when there's no NVMe drive) */
static void nvme_temps(cJSON *arr);
static void temps(cJSON *o) { nvme_temps(cJSON_AddArrayToObject(o, "temps")); }

/* the NVMe drive in the dock: behind its JMicron USB bridge, read with cmnvme (asked at most every
 * 20 s; the drive doesn't need waking up any more often than that) */
static void nvme_temps(cJSON *arr)
{
	static time_t at;
	static int n;
	static struct { char dev[16]; int c; } cache[4];
	time_t now = time(NULL);
	if (now - at >= 20 || at == 0) {
		at = now;
		n = 0;
		char tool[300];
		/* the NVMe temperature tool, installed with the app link */
		snprintf(tool, sizeof(tool), "%s/cmnvme", cmod_home());
		DIR *d = opendir("/sys/block");
		struct dirent *e;
		while (d && (e = readdir(d)) && n < 4) {
			if (strncmp(e->d_name, "sd", 2)) continue;
			char p[300], v[64];
			snprintf(p, sizeof(p), "/sys/block/%s/device/vendor", e->d_name);
			read_line(p, v, sizeof(v));
			if (!strcasestr(v, "jmicron") || access(tool, X_OK)) continue;
			char cmd[500], out[64] = "";
			snprintf(cmd, sizeof(cmd), "'%s' -temp /dev/%s 2>/dev/null", tool, e->d_name);
			FILE *f = popen(cmd, "r");
			if (!f) continue;
			if (!fgets(out, sizeof(out), f)) out[0] = 0;
			pclose(f);
			if (!out[0]) continue;
			snprintf(cache[n].dev, sizeof(cache[n].dev), "%s", e->d_name);
			cache[n].c = atoi(out);
			n++;
		}
		if (d) closedir(d);
	}
	for (int i = 0; i < n; i++) {
		cJSON *x = cJSON_CreateObject();
		cJSON_AddStringToObject(x, "name", "NVMe drive");
		cJSON_AddNumberToObject(x, "c", cache[i].c);
		cJSON_AddItemToArray(arr, x);
	}
}

static void cpu(cJSON *o)
{
	FILE *f = fopen("/proc/stat", "r");
	cJSON *arr = cJSON_AddArrayToObject(o, "cpu");      /* [name, busy, total] cumulative jiffies */
	char line[512];
	while (f && fgets(line, sizeof(line), f)) {
		if (strncmp(line, "cpu", 3)) continue;
		char name[16];
		unsigned long long v[10] = { 0 }, total = 0;
		int n = sscanf(line, "%15s %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu", name, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]);
		if (n < 5) continue;
		for (int i = 0; i < 8; i++) total += v[i];       /* guest time is already in user */
		cJSON *x = cJSON_CreateArray();
		cJSON_AddItemToArray(x, cJSON_CreateString(name));
		cJSON_AddItemToArray(x, cJSON_CreateNumber((double)(total - v[3] - v[4])));
		cJSON_AddItemToArray(x, cJSON_CreateNumber((double)total));
		cJSON_AddItemToArray(arr, x);
	}
	if (f) fclose(f);
	char model[160] = "";
	f = fopen("/proc/cpuinfo", "r");
	while (f && fgets(line, sizeof(line), f)) {
		char *c = strchr(line, ':');
		if (!c) continue;
		if (!strncmp(line, "model name", 10) || !strncmp(line, "Hardware", 8)) {
			c++;
			while (*c == ' ' || *c == '\t') c++;
			c[strcspn(c, "\r\n")] = 0;
			if (!model[0] || !strncmp(line, "model name", 10)) snprintf(model, sizeof(model), "%s", c);
		}
	}
	if (f) fclose(f);
	cJSON_AddStringToObject(o, "cpu_model", model);
	read_line("/proc/loadavg", line, sizeof(line));
	double l1 = 0, l5 = 0, l15 = 0;
	sscanf(line, "%lf %lf %lf", &l1, &l5, &l15);
	cJSON *ld = cJSON_AddArrayToObject(o, "load");
	cJSON_AddItemToArray(ld, cJSON_CreateNumber(l1));
	cJSON_AddItemToArray(ld, cJSON_CreateNumber(l5));
	cJSON_AddItemToArray(ld, cJSON_CreateNumber(l15));
	read_line("/proc/uptime", line, sizeof(line));
	cJSON_AddNumberToObject(o, "uptime", atof(line));
}

static void mem(cJSON *o)
{
	FILE *f = fopen("/proc/meminfo", "r");
	cJSON *m = cJSON_AddObjectToObject(o, "mem");        /* kB */
	char k[64];
	unsigned long long v;
	while (f && fscanf(f, "%63[^:]: %llu kB\n", k, &v) == 2) {
		if (!strcmp(k, "MemTotal")) cJSON_AddNumberToObject(m, "total", (double)v);
		else if (!strcmp(k, "MemAvailable")) cJSON_AddNumberToObject(m, "available", (double)v);
		else if (!strcmp(k, "Cached")) cJSON_AddNumberToObject(m, "cached", (double)v);
		else if (!strcmp(k, "SwapTotal")) cJSON_AddNumberToObject(m, "swap_total", (double)v);
		else if (!strcmp(k, "SwapFree")) cJSON_AddNumberToObject(m, "swap_free", (double)v);
	}
	if (f) fclose(f);
}

static void disks(cJSON *o)
{
	cJSON *arr = cJSON_AddArrayToObject(o, "disks");
	FILE *f = fopen("/proc/mounts", "r");
	char dev[256], mnt[256], fs[64];
	while (f && fscanf(f, "%255s %255s %63s %*[^\n]\n", dev, mnt, fs) == 3) {
		if (strcmp(mnt, "/media/fat") && strncmp(mnt, "/media/usb", 10) && strcmp(mnt, "/media/fat/cifs")) continue;
		struct statvfs s;
		if (statvfs(mnt, &s)) continue;
		cJSON *x = cJSON_CreateObject();
		cJSON_AddStringToObject(x, "path", mnt);
		cJSON_AddStringToObject(x, "fs", fs);
		cJSON_AddNumberToObject(x, "size_kb", (double)s.f_blocks * s.f_frsize / 1024);
		cJSON_AddNumberToObject(x, "free_kb", (double)s.f_bavail * s.f_frsize / 1024);
		cJSON_AddItemToArray(arr, x);
	}
	if (f) fclose(f);
	/* reads and writes (sectors) of the drives themselves */
	cJSON *io = cJSON_AddArrayToObject(o, "io");
	f = fopen("/proc/diskstats", "r");
	char line[512];
	while (f && fgets(line, sizeof(line), f)) {
		char name[64];
		unsigned long long rd, wr;
		if (sscanf(line, "%*u %*u %63s %*u %*u %llu %*u %*u %*u %llu", name, &rd, &wr) != 3) continue;
		int disk = (!strncmp(name, "mmcblk", 6) && !strchr(name + 6, 'p')) || (!strncmp(name, "sd", 2) && strlen(name) == 3) ||
		           (!strncmp(name, "nvme", 4) && !strchr(name + 4, 'p'));
		if (!disk) continue;
		cJSON *x = cJSON_CreateArray();
		cJSON_AddItemToArray(x, cJSON_CreateString(name));
		cJSON_AddItemToArray(x, cJSON_CreateNumber((double)rd * 512));
		cJSON_AddItemToArray(x, cJSON_CreateNumber((double)wr * 512));
		cJSON_AddItemToArray(io, x);
	}
	if (f) fclose(f);
}

static void net(cJSON *o)
{
	cJSON *ips = cJSON_AddArrayToObject(o, "ips");
	struct ifaddrs *ifs, *i;
	if (!getifaddrs(&ifs)) {
		for (i = ifs; i; i = i->ifa_next) {
			if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK)) continue;
			char a[32];
			inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, a, sizeof(a));
			cJSON *x = cJSON_CreateObject();
			cJSON_AddStringToObject(x, "if", i->ifa_name);
			cJSON_AddStringToObject(x, "ip", a);
			cJSON_AddItemToArray(ips, x);
		}
		freeifaddrs(ifs);
	}
	cJSON *arr = cJSON_AddArrayToObject(o, "net");       /* [interface, rx bytes, tx bytes] */
	FILE *f = fopen("/proc/net/dev", "r");
	char line[512];
	for (int n = 0; f && fgets(line, sizeof(line), f); n++) {
		if (n < 2) continue;
		char *c = strchr(line, ':');
		if (!c) continue;
		*c = 0;
		char *name = line;
		while (*name == ' ') name++;
		if (!strcmp(name, "lo")) continue;
		unsigned long long rx = 0, tx = 0;
		if (sscanf(c + 1, "%llu %*u %*u %*u %*u %*u %*u %*u %llu", &rx, &tx) != 2) continue;
		cJSON *x = cJSON_CreateArray();
		cJSON_AddItemToArray(x, cJSON_CreateString(name));
		cJSON_AddItemToArray(x, cJSON_CreateNumber((double)rx));
		cJSON_AddItemToArray(x, cJSON_CreateNumber((double)tx));
		cJSON_AddItemToArray(arr, x);
	}
	if (f) fclose(f);
}

/* every process: [pid, name, cpu jiffies (cumulative), memory kB] */
static void procs(cJSON *o)
{
	cJSON *arr = cJSON_AddArrayToObject(o, "procs");
	DIR *d = opendir("/proc");
	struct dirent *e;
	long pg = sysconf(_SC_PAGESIZE) / 1024;
	while (d && (e = readdir(d))) {
		if (!isdigit((unsigned char)e->d_name[0])) continue;
		char p[300], buf[1024];
		snprintf(p, sizeof(p), "/proc/%s/stat", e->d_name);
		FILE *f = fopen(p, "r");
		if (!f) continue;
		size_t n = fread(buf, 1, sizeof(buf) - 1, f);
		fclose(f);
		buf[n] = 0;
		char *l = strchr(buf, '('), *r = strrchr(buf, ')');
		if (!l || !r) continue;
		char name[64];
		snprintf(name, sizeof(name), "%.*s", (int)(r - l - 1), l + 1);
		unsigned long ut = 0, st = 0;
		long rss = 0;
		/* after ")": state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime stime ... rss is the 22nd */
		if (sscanf(r + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu %*d %*d %*d %*d %*d %*d %*u %*u %ld", &ut, &st, &rss) != 3) continue;
		if (!rss) continue;                               /* kernel threads */
		cJSON *x = cJSON_CreateArray();
		cJSON_AddItemToArray(x, cJSON_CreateNumber(atoi(e->d_name)));
		cJSON_AddItemToArray(x, cJSON_CreateString(name));
		cJSON_AddItemToArray(x, cJSON_CreateNumber((double)(ut + st)));
		cJSON_AddItemToArray(x, cJSON_CreateNumber((double)rss * pg));
		cJSON_AddItemToArray(arr, x);
	}
	if (d) closedir(d);
	cJSON_AddNumberToObject(o, "hz", (double)sysconf(_SC_CLK_TCK));
}

static void kernel_log(cJSON *o, int lines)
{
	int n = klogctl(10, NULL, 0);                         /* SYSLOG_ACTION_SIZE_BUFFER */
	if (n <= 0 || n > (1 << 22)) n = 1 << 17;
	char *b = malloc((size_t)n + 1);
	if (!b) return;
	int got = klogctl(3, b, n);                           /* SYSLOG_ACTION_READ_ALL: doesn't clear */
	if (got < 0) { free(b); return; }
	b[got] = 0;
	/* the last lines, without the "<6>" level marks */
	char *start = b + got;
	for (int k = 0; start > b && k <= lines; ) { start--; if (*start == '\n') k++; }
	if (*start == '\n') start++;
	char *out = malloc(strlen(start) + 1), *w = out;
	for (char *p = start; *p; ) {
		if (*p == '<' && (p == start || p[-1] == '\n')) { char *q = strchr(p, '>'); if (q && q - p < 5) { p = q + 1; continue; } }
		*w++ = *p++;
	}
	*w = 0;
	cJSON_AddStringToObject(o, "kernel", out);
	free(out);
	free(b);
}

cJSON *dash_json(int with_kernel)
{
	cJSON *o = cJSON_CreateObject();
	temps(o);
	cpu(o);
	mem(o);
	disks(o);
	net(o);
	procs(o);
	char core[64];
	read_line("/tmp/CORENAME", core, sizeof(core));
	cJSON_AddStringToObject(o, "core", core);
	if (with_kernel) kernel_log(o, 80);
	return o;
}

/* ---------- the drives' dirty marks and the NVMe drive's health ----------
 * {"cmd":"drives"}: for each exFAT drive in use (the SD card, the dock's NVMe drive, USB drives), whether it's
 * marked dirty (not shut down safely) or as failing, and the NVMe drive's health log. Nothing is read while a
 * game is running ("playing"). As in Phosphor's sd_integrity: with Linux's own exFAT driver a drive is marked
 * dirty the whole time it's mounted for writing, so what counts is how it was when it was mounted ("Volume was
 * not properly unmounted" in the kernel log); the SuperStation's driver keeps the mark as it is on the drive,
 * read straight from the drive (not through the cache), twice 2 s apart so a write going on can't look dirty. */
static int exfat_flags(const char *dev)
{
	void *b = NULL;
	if (posix_memalign(&b, 4096, 4096)) return -1;
	ssize_t n = -1;
	for (int direct = 1; direct >= 0 && n < 512; direct--) {   /* straight from the drive; through the cache if it won't */
		int fd = open(dev, O_RDONLY | O_CLOEXEC | (direct ? O_DIRECT : 0));
		if (fd < 0) continue;
		if (!direct) posix_fadvise(fd, 0, 4096, POSIX_FADV_DONTNEED);
		n = pread(fd, b, 4096, 0);
		close(fd);
	}
	int r = -1;
	if (n >= 512 && !memcmp((char *)b + 3, "EXFAT   ", 8)) r = ((unsigned char *)b)[106];
	free(b);
	return r;
}

static char *klog_all(void)
{
	int n = klogctl(10, NULL, 0);
	if (n <= 0 || n > (1 << 22)) n = 1 << 17;
	char *b = malloc((size_t)n + 1);
	if (!b) return NULL;
	int got = klogctl(3, b, n);
	if (got < 0) { free(b); return NULL; }
	b[got] = 0;
	return b;
}

/* the NVMe drive's health log, the parts that say something's wrong */
static void nvme_health(cJSON *x, const char *blk)
{
	char tool[300], cmd[500], line[200];
	snprintf(tool, sizeof(tool), "%s/cmnvme", cmod_home());
	if (access(tool, X_OK)) return;
	snprintf(cmd, sizeof(cmd), "'%s' -nvmesmart /dev/%s 2>/dev/null", tool, blk);
	FILE *f = popen(cmd, "r");
	if (!f) return;
	cJSON *h = cJSON_CreateObject();
	int any = 0;
	static const char *const keep[] = { "critical_warning", "available_spare", "spare_threshold", "percentage_used",
	                                    "media_errors", "unsafe_shutdowns", "temperature_c", NULL };
	while (fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '=');
		if (!eq) continue;
		*eq = 0;
		for (int i = 0; keep[i]; i++) if (!strcmp(line, keep[i])) { cJSON_AddNumberToObject(h, line, atof(eq + 1)); any = 1; }
	}
	pclose(f);
	if (any) cJSON_AddItemToObject(x, "health", h); else cJSON_Delete(h);
}

cJSON *drives_json(void)
{
	cJSON *o = cJSON_CreateObject();
	cJSON_AddBoolToObject(o, "ok", 1);
	char core[64] = "";
	read_line("/tmp/CORENAME", core, sizeof(core));
	if (core[0] && strcmp(core, "MENU")) { cJSON_AddBoolToObject(o, "playing", 1); return o; }
	char *kl = klog_all();
	/* Linux's own exFAT driver ("exFAT-fs (dev): ..."), and whether the boot is still in the kernel log */
	int intree = kl && strstr(kl, "exFAT-fs (");
	int boot_seen = kl && (strstr(kl, "] Booting Linux") || strstr(kl, "Linux version"));
	cJSON *arr = cJSON_AddArrayToObject(o, "drives");
	FILE *f = fopen("/proc/mounts", "r");
	char line[1024];
	while (f && fgets(line, sizeof(line), f)) {
		char dev[256], mnt[256], type[32], opts[512];
		if (sscanf(line, "%255s %255s %31s %511s", dev, mnt, type, opts) != 4) continue;
		if (!strcmp(dev, "/dev/root")) {   /* the boot drive (MiSTer's SD card): its name from root= */
			char cl[1024] = "", *r;
			read_line("/proc/cmdline", cl, sizeof(cl));
			if ((r = strstr(cl, "root=/dev/"))) { r += 5; r[strcspn(r, " \n")] = 0; snprintf(dev, sizeof(dev), "%s", r); }
		}
		if (strcmp(type, "exfat") || strncmp(dev, "/dev/", 5) || !strncmp(opts, "ro", 2)) continue;   /* in use, and written to */
		const char *kind = "usb", *name = "USB drive";
		char blk[64] = "";
		snprintf(blk, sizeof(blk), "%s", dev + 5);
		if (!strncmp(blk, "mmcblk", 6)) { kind = "sd"; name = "SD card"; char *pp = strrchr(blk, 'p'); if (pp) *pp = 0; }
		else {
			for (char *c = blk + 2; *c; c++) if (isdigit((unsigned char)*c)) { *c = 0; break; }
			char p[300], v[64] = "";
			snprintf(p, sizeof(p), "/sys/block/%s/device/vendor", blk);
			read_line(p, v, sizeof(v));
			if (strcasestr(v, "jmicron")) { kind = "nvme"; name = "NVMe drive"; }
		}
		int fl = exfat_flags(dev);
		if (fl < 0) continue;
		const char *state = fl & 4 ? "media-failure" : "clean";
		if (!(fl & 4)) {
			if (intree) {
				char msg[300];
				snprintf(msg, sizeof(msg), "exFAT-fs (%s): Volume was not properly unmounted", dev + 5);
				state = strstr(kl, msg) ? "dirty" : boot_seen ? "clean" : "unknown";
			} else if (fl & 2) {
				sleep(2);   /* a write going on: once more */
				int f2 = exfat_flags(dev);
				state = f2 >= 0 && (f2 & 2) ? "dirty" : "clean";
			}
		}
		cJSON *x = cJSON_CreateObject();
		cJSON_AddStringToObject(x, "kind", kind);
		cJSON_AddStringToObject(x, "name", name);
		cJSON_AddStringToObject(x, "mount", mnt);
		cJSON_AddStringToObject(x, "dev", dev + 5);
		cJSON_AddStringToObject(x, "state", state);
		if (!strcmp(kind, "nvme")) nvme_health(x, blk);
		cJSON_AddItemToArray(arr, x);
	}
	if (f) fclose(f);
	free(kl);
	return o;
}
