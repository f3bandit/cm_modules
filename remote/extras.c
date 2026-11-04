/* cmremote: the Windows app's view-only Devices and Network sections, MiSTer.ini, and Scripts.
 * Copyright (C) 2026 f3bandit. PolyForm Noncommercial 1.0.0 (see ../LICENSE.md).
 * network and usb only read. MiSTer.ini: every save keeps the old file in backups/ini first,
 * and any of those can be put back (which keeps the current one first too). Scripts: listed, and run here
 * (their output shown in the app) for the ones that don't need the TV. */
#include "extras.h"
#include "compat.h"
#include "../include/cmmod.h"
#include <dirent.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char *sysfs(void) { const char *s = getenv("CMOD_SYS"); return s && *s ? s : "/sys"; }

static void read_line(const char *path, char *out, size_t n)
{
	*out = 0;
	FILE *f = fopen(path, "r");
	if (!f) return;
	if (fgets(out, (int)n, f)) out[strcspn(out, "\r\n")] = 0;
	fclose(f);
	while (*out && out[strlen(out) - 1] == ' ') out[strlen(out) - 1] = 0;
}

cJSON *ex_network(void)
{
	cJSON *o = cJSON_CreateObject(), *arr = cJSON_AddArrayToObject(o, "interfaces");
	char p[300], v[128];
	gethostname(v, sizeof(v));
	v[sizeof(v) - 1] = 0;
	cJSON_AddStringToObject(o, "hostname", v);
	DIR *d;
	snprintf(p, sizeof(p), "%s/class/net", sysfs());
	struct ifaddrs *ifa = NULL;
	getifaddrs(&ifa);
	if ((d = opendir(p))) {
		struct dirent *e;
		while ((e = readdir(d))) {
			if (e->d_name[0] == '.' || !strcmp(e->d_name, "lo") || !strncmp(e->d_name, "ifb", 3) || !strncmp(e->d_name, "sit", 3)) continue;
			cJSON *x = cJSON_CreateObject();
			cJSON_AddStringToObject(x, "name", e->d_name);
			snprintf(p, sizeof(p), "%s/class/net/%s/wireless", sysfs(), e->d_name);
			struct stat sb;
			int wl = stat(p, &sb) == 0 || !strncmp(e->d_name, "wlan", 4);
			cJSON_AddStringToObject(x, "kind", wl ? "wifi" : "wired");
			snprintf(p, sizeof(p), "%s/class/net/%s/address", sysfs(), e->d_name); read_line(p, v, sizeof(v));
			cJSON_AddStringToObject(x, "mac", v);
			snprintf(p, sizeof(p), "%s/class/net/%s/operstate", sysfs(), e->d_name); read_line(p, v, sizeof(v));
			cJSON_AddBoolToObject(x, "up", !strcmp(v, "up"));
			snprintf(p, sizeof(p), "%s/class/net/%s/speed", sysfs(), e->d_name); read_line(p, v, sizeof(v));
			if (!wl && atoi(v) > 0) cJSON_AddNumberToObject(x, "mbps", atoi(v));
			cJSON *ips = cJSON_AddArrayToObject(x, "ips");
			for (struct ifaddrs *a = ifa; a; a = a->ifa_next) {
				if (!a->ifa_addr || strcmp(a->ifa_name, e->d_name)) continue;
				char ip[64] = "";
				if (a->ifa_addr->sa_family == AF_INET) inet_ntop(AF_INET, &((struct sockaddr_in *)a->ifa_addr)->sin_addr, ip, sizeof(ip));
				else if (a->ifa_addr->sa_family == AF_INET6) {
					struct in6_addr *a6 = &((struct sockaddr_in6 *)a->ifa_addr)->sin6_addr;
					if (IN6_IS_ADDR_LINKLOCAL(a6)) continue;
					inet_ntop(AF_INET6, a6, ip, sizeof(ip));
				}
				if (*ip) cJSON_AddItemToArray(ips, cJSON_CreateString(ip));
			}
			cJSON_AddItemToArray(arr, x);
		}
		closedir(d);
	}
	if (ifa) freeifaddrs(ifa);
	/* WiFi: which network and how strong (no passwords, no saved list) */
	cmod_wifi_status_t ws;
	cmod_wifi_status(&ws);
	cJSON *w = cJSON_AddObjectToObject(o, "wifi");
	cJSON_AddBoolToObject(w, "present", ws.present);
	cJSON_AddBoolToObject(w, "connected", ws.connected);
	cJSON_AddStringToObject(w, "iface", ws.iface);
	cJSON_AddStringToObject(w, "ssid", ws.ssid);
	cJSON_AddNumberToObject(w, "signal", ws.signal);
	cJSON_AddNumberToObject(w, "freq", ws.freq);
	/* the default route */
	FILE *f = fopen("/proc/net/route", "r");
	char line[256];
	while (f && fgets(line, sizeof(line), f)) {
		char ifn[32];
		unsigned dst, gw;
		if (sscanf(line, "%31s %x %x", ifn, &dst, &gw) == 3 && dst == 0 && gw) {
			struct in_addr a = { .s_addr = gw };
			cJSON_AddStringToObject(o, "gateway", inet_ntoa(a));
			cJSON_AddStringToObject(o, "gateway_if", ifn);
			break;
		}
	}
	if (f) fclose(f);
	cJSON *dns = cJSON_AddArrayToObject(o, "dns");
	f = fopen("/etc/resolv.conf", "r");
	while (f && fgets(line, sizeof(line), f)) {
		char ns[64];
		if (sscanf(line, "nameserver %63s", ns) == 1) cJSON_AddItemToArray(dns, cJSON_CreateString(ns));
	}
	if (f) fclose(f);
	return o;
}

/* USB devices (not hubs): name, maker, ids, speed */
cJSON *ex_usb(void)
{
	cJSON *o = cJSON_CreateObject(), *arr = cJSON_AddArrayToObject(o, "devices");
	char p[700], v[128];
	snprintf(p, sizeof(p), "%s/bus/usb/devices", sysfs());
	DIR *d = opendir(p);
	struct dirent *e;
	while (d && (e = readdir(d))) {
		if (e->d_name[0] == '.' || strchr(e->d_name, ':') || !strncmp(e->d_name, "usb", 3)) continue;
		char base[300];
		snprintf(base, sizeof(base), "%s/bus/usb/devices/%s", sysfs(), e->d_name);
		snprintf(p, sizeof(p), "%s/bDeviceClass", base); read_line(p, v, sizeof(v));
		if (!strcmp(v, "09")) continue;                         /* a hub */
		cJSON *x = cJSON_CreateObject();
		cJSON_AddStringToObject(x, "port", e->d_name);
		snprintf(p, sizeof(p), "%s/product", base); read_line(p, v, sizeof(v)); cJSON_AddStringToObject(x, "product", v);
		snprintf(p, sizeof(p), "%s/manufacturer", base); read_line(p, v, sizeof(v)); cJSON_AddStringToObject(x, "maker", v);
		char vid[16], pid[16];
		snprintf(p, sizeof(p), "%s/idVendor", base); read_line(p, vid, sizeof(vid));
		snprintf(p, sizeof(p), "%s/idProduct", base); read_line(p, pid, sizeof(pid));
		snprintf(v, sizeof(v), "%s:%s", vid, pid);
		cJSON_AddStringToObject(x, "id", v);
		snprintf(p, sizeof(p), "%s/speed", base); read_line(p, v, sizeof(v)); cJSON_AddNumberToObject(x, "mbps", atof(v));
		/* what it is, from its first interface's class */
		snprintf(p, sizeof(p), "%s/%s:1.0/bInterfaceClass", base, e->d_name); read_line(p, v, sizeof(v));
		const char *k = !strcmp(v, "03") ? "input" : !strcmp(v, "08") ? "storage" : !strcmp(v, "e0") ? "bluetooth" : !strcmp(v, "ff") ? "vendor" :
		                !strcmp(v, "01") ? "audio" : !strcmp(v, "02") || !strcmp(v, "0a") ? "network" : "other";
		cJSON_AddStringToObject(x, "kind", k);
		cJSON_AddItemToArray(arr, x);
	}
	if (d) closedir(d);
	return o;
}

/* ---------- MiSTer.ini ---------- */
#define INI_MAX (256 * 1024)

static char *slurp(const char *path, size_t *n)
{
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	char *b = malloc(INI_MAX + 1);
	size_t r = b ? fread(b, 1, INI_MAX + 1, f) : 0;
	fclose(f);
	if (!b) return NULL;
	if (r > INI_MAX) { free(b); return NULL; }
	b[r] = 0;
	if (n) *n = r;
	return b;
}

static int write_atomic(const char *path, const char *data, size_t n)
{
	char tmp[700];
	snprintf(tmp, sizeof(tmp), "%s.cm-new", path);
	FILE *f = fopen(tmp, "wb");
	if (!f) return -1;
	int bad = fwrite(data, 1, n, f) != n;
	bad |= fflush(f) != 0;
	bad |= fsync(fileno(f)) != 0;
	bad |= fclose(f) != 0;
	if (bad || rename(tmp, path)) { unlink(tmp); return -1; }
	return 0;
}

static int ini_name_ok(const char *n)
{
	/* MiSTer.ini, MiSTer_<alt>.ini */
	if (!n || strlen(n) > 60 || strchr(n, '/') || strstr(n, "..")) return 0;
	if (!strcmp(n, "MiSTer.ini") || !strcmp(n, "ConsoleMode.ini")) return 1;
	size_t l = strlen(n);
	return !strncmp(n, "MiSTer_", 7) && l > 11 && !strcmp(n + l - 4, ".ini");
}

/* where a file is: "ConsoleMode.ini" is Console Mode's own settings, ConsoleMode/config.ini */
static void ini_path(char *out, size_t n, const char *file)
{
	if (!strcmp(file, "ConsoleMode.ini")) snprintf(out, n, "%s/ConsoleMode/config.ini", p_fat());
	else snprintf(out, n, "%s/%s", p_fat(), file);
}

static void bk_dir(char *out, size_t n) { snprintf(out, n, "%s/backups/ini", p_data()); }

/* keep the current file in backups/ini/<file>.<date> */
static int ini_keep(const char *file, char *kept, size_t kn)
{
	char src[600], dir[600];
	ini_path(src, sizeof(src), file);
	size_t n = 0;
	char *cur = slurp(src, &n);
	if (!cur) { if (kept) *kept = 0; return 0; }               /* nothing there yet */
	bk_dir(dir, sizeof(dir));
	mkdirs(dir);
	char stamp[32], dst[700];
	time_t t = time(NULL);
	strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", localtime(&t));
	snprintf(dst, sizeof(dst), "%s/%s.%s", dir, file, stamp);
	struct stat sb;
	for (int k = 2; stat(dst, &sb) == 0 && k < 100; k++) {        /* two in the same second: never over one */
		snprintf(stamp + 15, sizeof(stamp) - 15, "-%d", k);
		snprintf(dst, sizeof(dst), "%s/%s.%s", dir, file, stamp);
	}
	int r = write_atomic(dst, cur, n);
	free(cur);
	if (kept) snprintf(kept, kn, "%s.%s", file, stamp);
	return r;
}

int ex_ini_keep(const char *file, char *kept, size_t kn) { return ini_keep(file, kept, kn); }

cJSON *ex_ini(cJSON *req, const char **err)
{
	const char *d = cJSON_GetStringValue(cJSON_GetObjectItem(req, "do"));
	const char *file = cJSON_GetStringValue(cJSON_GetObjectItem(req, "file"));
	if (!file) file = "MiSTer.ini";
	if (!ini_name_ok(file)) { *err = "file: MiSTer.ini, MiSTer_<name>.ini or ConsoleMode.ini (Console Mode's config.ini)"; return NULL; }
	char path[600], kept[200] = "";
	ini_path(path, sizeof(path), file);
	if (d && !strcmp(d, "save")) {
		const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(req, "text"));
		if (!text) { *err = "text: the new file"; return NULL; }
		size_t n = strlen(text);
		if (n > INI_MAX) { *err = "That's too big for an ini file"; return NULL; }
		if (ini_keep(file, kept, sizeof(kept))) { *err = "Couldn't keep a copy of the old file, so nothing was changed"; return NULL; }
		if (write_atomic(path, text, n)) { *err = "Couldn't write it (the old file is untouched)"; return NULL; }
		plog("ini: %s saved from the app (old one kept as %s)", file, kept[0] ? kept : "-");
	} else if (d && !strcmp(d, "restore")) {
		const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(req, "name"));
		size_t fl = strlen(file);
		if (!name || strchr(name, '/') || strstr(name, "..") || strncmp(name, file, fl) || name[fl] != '.') { *err = "name: one of the kept copies"; return NULL; }
		char dir[600], src[800];
		bk_dir(dir, sizeof(dir));
		snprintf(src, sizeof(src), "%s/%s", dir, name);
		size_t n = 0;
		char *old = slurp(src, &n);
		if (!old) { *err = "That copy isn't there"; return NULL; }
		if (ini_keep(file, kept, sizeof(kept)) || write_atomic(path, old, n)) { free(old); *err = "Couldn't put it back (the current file is untouched)"; return NULL; }
		free(old);
		plog("ini: %s put back from %s (the one before kept as %s)", file, name, kept[0] ? kept : "-");
	} else if (d) { *err = "do: save (text) or restore (name)"; return NULL; }
	cJSON *o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "file", file);
	if (kept[0]) cJSON_AddStringToObject(o, "kept", kept);
	size_t n = 0;
	char *t = slurp(path, &n);
	cJSON_AddBoolToObject(o, "exists", t != NULL);
	cJSON_AddStringToObject(o, "text", t ? t : "");
	free(t);
	/* the other ini files, and the kept copies of this one (newest first) */
	cJSON *files = cJSON_AddArrayToObject(o, "files"), *ks = cJSON_AddArrayToObject(o, "kept_copies");
	DIR *dd = opendir(p_fat());
	struct dirent *e;
	while (dd && (e = readdir(dd))) if (ini_name_ok(e->d_name) && strcmp(e->d_name, "ConsoleMode.ini")) cJSON_AddItemToArray(files, cJSON_CreateString(e->d_name));
	if (dd) closedir(dd);
	char cmp[600];
	ini_path(cmp, sizeof(cmp), "ConsoleMode.ini");
	if (access(cmp, R_OK) == 0) cJSON_AddItemToArray(files, cJSON_CreateString("ConsoleMode.ini"));
	char dir[600];
	bk_dir(dir, sizeof(dir));
	struct dirent **nl = NULL;
	int m = scandir(dir, &nl, NULL, alphasort);
	size_t fl = strlen(file);
	for (int i = m - 1; i >= 0; i--) {
		if (!strncmp(nl[i]->d_name, file, fl) && nl[i]->d_name[fl] == '.' && strcmp(nl[i]->d_name + fl, ".cm-new")) cJSON_AddItemToArray(ks, cJSON_CreateString(nl[i]->d_name));
		free(nl[i]);
	}
	free(nl);
	return o;
}

/* ---------- Scripts: listed here, run on the TV ---------- */
static int script_name_ok(const char *n)
{
	size_t l = n ? strlen(n) : 0;
	return l > 3 && l < 120 && n[0] != '.' && !strchr(n, '/') && !strstr(n, "..") && !strcmp(n + l - 3, ".sh");
}

cJSON *ex_scripts(cJSON *req, const char **err)
{
	const char *d = cJSON_GetStringValue(cJSON_GetObjectItem(req, "do"));
	char dir[600];
	snprintf(dir, sizeof(dir), "%s/Scripts", p_fat());
	if (d && !strcmp(d, "run")) {
		const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(req, "name"));
		char path[800], rq[600];
		struct stat sb;
		if (!script_name_ok(name)) { *err = "name: a script in Scripts"; return NULL; }
		snprintf(path, sizeof(path), "%s/%s", dir, name);
		if (stat(path, &sb) || !S_ISREG(sb.st_mode)) { *err = "That script isn't there"; return NULL; }
		/* run here, as a job: its output shows in the app (scripts that draw on the TV, like menus, don't fit) */
		const char *argv[] = { "bash", path, NULL };
		(void)rq;
		const char *why = job_run(name, argv);
		if (why) { *err = why; return NULL; }
		plog("scripts: %s run from the app", name);
	} else if (d) { *err = "do: run (name)"; return NULL; }
	cJSON *o = cJSON_CreateObject(), *arr = cJSON_AddArrayToObject(o, "scripts");
	struct dirent **nl = NULL;
	int m = scandir(dir, &nl, NULL, alphasort);
	for (int i = 0; i < m; i++) {
		if (script_name_ok(nl[i]->d_name)) {
			char path[800];
			struct stat sb;
			snprintf(path, sizeof(path), "%s/%s", dir, nl[i]->d_name);
			if (!stat(path, &sb) && S_ISREG(sb.st_mode)) {
				cJSON *x = cJSON_CreateObject();
				cJSON_AddStringToObject(x, "name", nl[i]->d_name);
				cJSON_AddNumberToObject(x, "bytes", (double)sb.st_size);
				cJSON_AddNumberToObject(x, "at", (double)sb.st_mtime);
				cJSON_AddItemToArray(arr, x);
			}
		}
		free(nl[i]);
	}
	free(nl);
	return o;
}
