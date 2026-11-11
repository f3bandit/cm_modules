/* cmdisc: the game disc in the CD/DVD drive, as an ordinary .cue and .bin files that are read off the
 * disc live: a folder (FUSE) whose files are the disc's tracks, laid out the way Redump's copies are
 * (one .bin per track, raw 2352-byte sectors, each track's pregap at the start of its file). Console
 * Mode's Main already plays CD games from .cue/.bin, so a game in the drive starts the same way a copy
 * on a drive does: nothing in Main changes, and nothing is copied.
 *
 * Read with the drive's own commands (MMC READ CD through SG_IO, the table of contents with Linux's
 * CDROMREADTOC*), with a read-ahead buffer so the game rarely waits for the drive to seek. Only while a
 * disc game is mounted; nothing runs otherwise.
 *
 *   cmdisc status                    key=value: state (none/empty/open/notready/ready), the disc's system
 *                                    (psx, saturn, megacd, pcecd, neogeocd, 3do, cdi, audio, unknown), its
 *                                    product code (serial), the core that plays it, the tracks
 *   cmdisc play                      the disc's core started with it (an .mgl, as Console Mode starts games)
 *   cmdisc watch                     stays running: a disc put in while the menu is up starts its game;
 *                                    taken out, its files go
 *   cmdisc stop | eject              the files taken away (eject: and the tray opened)
 *   cmdisc mount [-b] [dir]          only the files: <dir>/<name>.cue and its .bin (default /tmp/cmdisc)
 * A PlayStation disc's files are named by its product code, so its saves follow the game; LibCrypt
 * discs need sbi.zip in games/PSX, as for copies (Main reads it): status says whether the pack is there
 * (sbi_pack) and has this game (sbi; a game that isn't in it doesn't need it).
 *   options: --name N (the files' name, default "Disc"), --dev /dev/srN (default /dev/sr0),
 *            --image file.cue (a copy on a drive instead of the drive: for testing)
 *
 * Copyright (C) 2026 f3bandit. All rights reserved; licensed to Retro Remake (see ../LICENSE.md). */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <sys/uio.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/cdrom.h>
#include <linux/fuse.h>
#include <pthread.h>
#include <scsi/sg.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SECTOR 2352
#define MAX_TRACKS 99
#define CHUNK 32                 /* sectors read from the drive at once */
#define NCHUNKS 192              /* the buffer: 192 x 32 x 2352 = 14 MB */
#define AHEAD 4                  /* chunks read ahead of the one asked for */

/* ---------- the disc: the drive, or an image (testing) ---------- */

typedef struct {
	int first, last, leadout;
	int lba[MAX_TRACKS + 2], data[MAX_TRACKS + 2], mode[MAX_TRACKS + 2];
	int start[MAX_TRACKS + 2];   /* where each track's file starts (its pregap); start[last+1] = leadout */
} toc_t;

static int dfd = -1;             /* the drive */
static const char *devpath = "/dev/sr0";
/* an image: its files, one after another in one sector space */
static struct { char path[1024]; int fd; long start, count; } img[MAX_TRACKS + 1];
static int nimg;
static toc_t T;
static pthread_mutex_t io_lk = PTHREAD_MUTEX_INITIALIZER;

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...)
{
	if (!getenv("CMDISC_DEBUG")) return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

/* MMC READ CD: n raw sectors (sync, header, data, EDC/ECC: 2352 bytes each), or their subchannel Q (16) */
static int read_cd(int lba, int n, uint8_t *buf, int sub)
{
	uint8_t cdb[12] = { 0xBE, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba,
	                    (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n, sub ? 0x00 : 0xF8, sub ? 0x02 : 0x00, 0 };
	uint8_t sense[32];
	sg_io_hdr_t io = { 0 };
	io.interface_id = 'S';
	io.dxfer_direction = SG_DXFER_FROM_DEV;
	io.cmd_len = sizeof(cdb);
	io.cmdp = cdb;
	io.dxferp = buf;
	io.dxfer_len = (unsigned)n * (sub ? 16 : SECTOR);
	io.sbp = sense;
	io.mx_sb_len = sizeof(sense);
	io.timeout = 20000;
	if (ioctl(dfd, SG_IO, &io) < 0) return -1;
	if (io.host_status || (io.driver_status & ~0x08)) return -1;
	if (io.status) {
		int key = -1;
		if (io.sb_len_wr >= 3 && (sense[0] & 0x7e) == 0x70) key = sense[2] & 0x0f;
		else if (io.sb_len_wr >= 2 && (sense[0] & 0x7e) == 0x72) key = sense[1] & 0x0f;
		if (key == 2 || key == 6) return -2;      /* not ready, or the disc changed */
		if (key != 1) return -1;                  /* (1: recovered: the data is good) */
	}
	return io.resid ? -1 : 0;
}

/* n sectors from lba, from the drive or the image; -2: the disc is gone */
static int read_sectors(int lba, int n, uint8_t *buf)
{
	if (!nimg) {
		int r = read_cd(lba, n, buf, 0);
		if (r == 0 || r == -2) return r;
		for (int i = 0; i < n; i++)                /* one at a time: a bad sector doesn't spoil the rest */
			if ((r = read_cd(lba + i, 1, buf + (size_t)i * SECTOR, 0)) == -2) return r;
			else if (r) memset(buf + (size_t)i * SECTOR, 0, SECTOR), logf_("cmdisc: sector %d unreadable", lba + i);
		return 0;
	}
	for (int i = 0; i < n; i++) {
		long s = lba + i;
		uint8_t *o = buf + (size_t)i * SECTOR;
		memset(o, 0, SECTOR);
		for (int k = 0; k < nimg; k++)
			if (s >= img[k].start && s < img[k].start + img[k].count) {
				if (pread(img[k].fd, o, SECTOR, (off_t)(s - img[k].start) * SECTOR) != SECTOR) return -1;
				break;
			}
	}
	return 0;
}

static int bcd(uint8_t b) { return (b >> 4) * 10 + (b & 15); }

/* where track i's pregap starts: the subchannel Q just before its index 1; else the usual gaps */
static int pregap_start(int i, int pcecd)
{
	int idx1 = T.lba[i], look = 300;
	if (idx1 - look < T.lba[i - 1]) look = idx1 - T.lba[i - 1];
	int start = -1;
	uint8_t *q = look > 0 ? malloc((size_t)look * 16) : NULL;
	if (q && read_cd(idx1 - look, look, q, 1) == 0) {
		int seen = 0;
		start = idx1;
		for (int k = look - 1; k >= 0; k--) {
			const uint8_t *s = q + (size_t)k * 16;
			if ((s[0] & 0x0f) != 1) continue;     /* not position data */
			seen = 1;
			if (bcd(s[1]) == i && bcd(s[2]) == 0) start = idx1 - look + k;
			else break;
		}
		if (!seen) start = -1;
	}
	free(q);
	if (start >= 0) for (int g = 150; g <= 225; g += 75) if (abs((idx1 - start) - g) <= 4 && idx1 - g >= T.lba[i - 1]) start = idx1 - g;
	if (start >= 0) return start;
	if (T.data[i - 1] && !T.data[i]) return idx1 - 150;
	if (!T.data[i - 1] && T.data[i] && pcecd) return idx1 - 225;
	return idx1;
}

static int drive_state(void)
{
	int fd = open(devpath, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) return -1;
	int s = ioctl(fd, CDROM_DRIVE_STATUS, CDSL_CURRENT);
	close(fd);
	return s;
}

static long msf_parse(const char *s)
{
	int m = 0, sec = 0, f = 0;
	return sscanf(s, "%d:%d:%d", &m, &sec, &f) == 3 ? (long)m * 4500 + sec * 75 + f : -1;
}

/* an image's .cue: its files one after another; each track's index 1 (and 0) */
static int open_image(const char *cue)
{
	FILE *f = fopen(cue, "r");
	if (!f) return -1;
	char dir[1024], line[1200];
	snprintf(dir, sizeof(dir), "%s", cue);
	char *sl = strrchr(dir, '/');
	if (sl) *sl = 0; else strcpy(dir, ".");
	long base = 0;
	int tr = 0, idx0[MAX_TRACKS + 2] = { 0 }, have0[MAX_TRACKS + 2] = { 0 };
	memset(&T, 0, sizeof(T));
	T.first = 0;
	while (fgets(line, sizeof(line), f)) {
		char *p = line;
		while (isspace((unsigned char)*p)) p++;
		if (!strncasecmp(p, "FILE ", 5)) {
			char *a = strchr(p, '"'), *b = a ? strchr(a + 1, '"') : NULL;
			if (!a || !b || nimg >= MAX_TRACKS) { fclose(f); return -1; }
			*b = 0;
			if (nimg) base = img[nimg - 1].start + img[nimg - 1].count;
			snprintf(img[nimg].path, sizeof(img[nimg].path), "%s/%s", dir, a + 1);
			img[nimg].fd = open(img[nimg].path, O_RDONLY | O_CLOEXEC);
			struct stat st;
			if (img[nimg].fd < 0 || fstat(img[nimg].fd, &st)) { fclose(f); return -1; }
			img[nimg].start = base;
			img[nimg].count = st.st_size / SECTOR;
			nimg++;
		} else if (!strncasecmp(p, "TRACK ", 6)) {
			tr = atoi(p + 6);
			if (tr < 1 || tr > MAX_TRACKS || !nimg) { fclose(f); return -1; }
			if (!T.first) T.first = tr;
			T.last = tr;
			if (strstr(p, "/2048") || strstr(p, "/2336")) { fclose(f); errno = EINVAL; return -2; }   /* only raw (2352-byte) images */
			T.data[tr] = !strcasestr(p, "AUDIO");
			T.mode[tr] = strcasestr(p, "MODE1") ? 1 : strcasestr(p, "MODE2") ? 2 : 0;
		} else if (!strncasecmp(p, "INDEX ", 6) && tr) {
			int n = atoi(p + 6);
			char *t = p + 6;
			while (*t && !isspace((unsigned char)*t)) t++;
			long at = msf_parse(t);
			if (at < 0) continue;
			if (n == 1) T.lba[tr] = (int)(img[nimg - 1].start + at);
			if (n == 0) idx0[tr] = (int)(img[nimg - 1].start + at), have0[tr] = 1;
		}
	}
	fclose(f);
	if (!T.first || !nimg) return -1;
	T.leadout = (int)(img[nimg - 1].start + img[nimg - 1].count);
	for (int i = T.first; i <= T.last; i++) T.start[i] = i == T.first ? 0 : have0[i] ? idx0[i] : T.lba[i];
	T.start[T.last + 1] = T.leadout;
	return 0;
}

static int open_drive(void)
{
	dfd = open(devpath, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (dfd < 0) return -1;
	struct cdrom_tochdr h;
	if (ioctl(dfd, CDROMREADTOCHDR, &h) || h.cdth_trk0 < 1 || h.cdth_trk1 < h.cdth_trk0 || h.cdth_trk1 > MAX_TRACKS) return -1;
	memset(&T, 0, sizeof(T));
	T.first = h.cdth_trk0;
	T.last = h.cdth_trk1;
	for (int i = T.first; i <= T.last + 1; i++) {
		struct cdrom_tocentry e = { 0 };
		e.cdte_track = i > T.last ? CDROM_LEADOUT : i;
		e.cdte_format = CDROM_LBA;
		if (ioctl(dfd, CDROMREADTOCENTRY, &e)) return -1;
		if (i > T.last) T.leadout = e.cdte_addr.lba;
		else T.lba[i] = e.cdte_addr.lba, T.data[i] = (e.cdte_ctrl & CDROM_DATA_TRACK) != 0;
	}
	return 0;
}

static int iso_root(int track, uint32_t *rlba, uint32_t *rsize);
static int iso_find(int track, uint32_t dlba, uint32_t dsize, const char *want, uint32_t *flba, uint32_t *fsize);

/* the disc's system, from its first data sectors (and each data track's mode) */
static const char *disc_type(void)
{
	static char type[16];
	uint8_t s[SECTOR];
	int d = -1;
	for (int i = T.first; i <= T.last; i++) {
		if (!T.data[i]) continue;
		if (d < 0) d = i;
		if (!T.mode[i] && !read_sectors(T.lba[i], 1, s)) T.mode[i] = s[15] == 1 ? 1 : 2;
	}
	if (d < 0) return "audio";
	strcpy(type, "unknown");
	if (read_sectors(T.lba[d], 1, s)) return type;
	const uint8_t *u = s + (T.mode[d] == 2 ? 24 : 16);   /* the user data */
	if (!memcmp(u, "SEGADISCSYSTEM", 14)) return strcpy(type, "megacd");
	if (!memcmp(u, "SEGA SEGASATURN", 15)) return strcpy(type, "saturn");
	if (u[0] == 1 && !memcmp(u + 1, "\x5a\x5a\x5a\x5a\x5a\x01", 6)) return strcpy(type, "3do");
	if (!read_sectors(T.lba[d] + 1, 1, s) && memmem(s, SECTOR, "PC Engine CD-ROM SYSTEM", 23)) return strcpy(type, "pcecd");
	if (!read_sectors(T.lba[d] + 16, 1, s)) {            /* the ISO 9660 volume descriptor */
		u = s + (T.mode[d] == 2 ? 24 : 16);
		if (!memcmp(u + 1, "CD001", 5) && !memcmp(u + 8, "PLAYSTATION", 11)) return strcpy(type, "psx");
		if (!memcmp(u + 1, "CD-I ", 5)) return strcpy(type, "cdi");
	}
	return type;
}

/* a Neo Geo CD: an ISO disc whose root has IPL.TXT (its load list) */
static int neogeo_cd(void)
{
	int d = -1;
	for (int i = T.first; i <= T.last && d < 0; i++) if (T.data[i]) d = i;
	uint32_t rl, rs, fl, fs;
	return d >= 0 && !iso_root(d, &rl, &rs) && !iso_find(d, rl - (uint32_t)T.lba[d], rs, "IPL.TXT", &fl, &fs);
}

/* ---------- the game's product code: ISO 9660 (PlayStation's SYSTEM.CNF), the Saturn and Mega CD headers ---------- */

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* a file in an ISO 9660 directory: its first sector (from the track's start) and size; 0 when found */
static int iso_find(int track, uint32_t dlba, uint32_t dsize, const char *want, uint32_t *flba, uint32_t *fsize)
{
	int ofs = T.mode[track] == 2 ? 24 : 16;
	uint8_t s[SECTOR];
	for (uint32_t k = 0; k < (dsize + 2047) / 2048 && k < 16; k++) {
		if (read_sectors(T.lba[track] + (int)(dlba + k) - 0, 1, s)) return -1;
		const uint8_t *d = s + ofs;
		for (int o = 0; o < 2048;) {
			int len = d[o];
			if (!len || o + len > 2048) break;
			int nl = d[o + 32];
			if (o + 33 + nl <= 2048 && nl >= (int)strlen(want) && !strncasecmp((const char *)d + o + 33, want, strlen(want)) &&
			    (nl == (int)strlen(want) || d[o + 33 + strlen(want)] == ';')) {
				*flba = le32(d + o + 2);
				*fsize = le32(d + o + 10);
				return 0;
			}
			o += len;
		}
	}
	return -1;
}

/* the ISO 9660 root directory of a data track (lba relative to the disc's LBA 0 for PlayStation discs) */
static int iso_root(int track, uint32_t *rlba, uint32_t *rsize)
{
	uint8_t s[SECTOR];
	if (read_sectors(T.lba[track] + 16, 1, s)) return -1;
	const uint8_t *u = s + (T.mode[track] == 2 ? 24 : 16);
	if (u[0] != 1 || memcmp(u + 1, "CD001", 5)) return -1;
	*rlba = le32(u + 156 + 2);
	*rsize = le32(u + 156 + 10);
	return 0;
}

static void trim_copy(char *dst, size_t n, const uint8_t *src, size_t len)
{
	size_t j = 0;
	for (size_t i = 0; i < len && j + 1 < n; i++) if (src[i] > ' ' && src[i] < 127) dst[j++] = (char)src[i];
	dst[j] = 0;
}

static char serial[40];

static void read_serial(const char *type)
{
	serial[0] = 0;
	int d = -1;
	for (int i = T.first; i <= T.last && d < 0; i++) if (T.data[i]) d = i;
	if (d < 0) return;
	uint8_t s[SECTOR];
	const uint8_t *u = s + (T.mode[d] == 2 ? 24 : 16);
	if (!strcmp(type, "saturn") || !strcmp(type, "megacd")) {
		if (read_sectors(T.lba[d], 1, s)) return;
		trim_copy(serial, sizeof(serial), u + (!strcmp(type, "saturn") ? 0x20 : 0x180), !strcmp(type, "saturn") ? 10 : 14);
		return;
	}
	if (strcmp(type, "psx")) return;
	uint32_t rl, rs, fl, fs;
	/* PlayStation discs' ISO sector numbers count from the disc's start: track 1 starts at LBA 0 */
	if (iso_root(d, &rl, &rs) || iso_find(d, rl - (uint32_t)T.lba[d], rs, "SYSTEM.CNF", &fl, &fs)) return;
	if (read_sectors((int)fl, 1, s)) return;
	char cnf[2049];
	memcpy(cnf, u, 2048);
	cnf[2048] = 0;
	char *b = strcasestr(cnf, "BOOT");
	if (!b) return;
	char *e = b + strcspn(b, "\r\n");
	*e = 0;
	char *p = strrchr(b, '\\');
	if (!p) p = strrchr(b, ':');
	if (!p) return;
	p++;
	char *semi = strchr(p, ';');
	if (semi) *semi = 0;
	size_t j = 0;
	for (; *p && j + 1 < sizeof(serial); p++) {
		if (*p == '_') serial[j++] = '-';
		else if (*p != '.' && !isspace((unsigned char)*p)) serial[j++] = (char)toupper((unsigned char)*p);
	}
	serial[j] = 0;
}

static void layout(const char *type)
{
	if (!nimg) {
		int pce = !strcmp(type, "pcecd");
		T.start[T.first] = T.lba[T.first];
		for (int i = T.first + 1; i <= T.last; i++) T.start[i] = pregap_start(i, pce);
		T.start[T.last + 1] = T.leadout;
	}
}

/* ---------- the buffer: chunks of the disc, read ahead ---------- */

static struct { int chunk; uint64_t used; uint8_t *data; } C[NCHUNKS];
static uint64_t tick;
static pthread_mutex_t c_lk = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t c_cv = PTHREAD_COND_INITIALIZER;
static int want[AHEAD], nwant, gone;

static int cache_find(int chunk)
{
	for (int i = 0; i < NCHUNKS; i++) if (C[i].chunk == chunk) return i;
	return -1;
}

static int cache_slot(void)
{
	int lru = 0;
	for (int i = 0; i < NCHUNKS; i++) {
		if (C[i].chunk < 0) return i;
		if (C[i].used < C[lru].used) lru = i;
	}
	return lru;
}

/* the chunk's sectors in the buffer (read now if they aren't); -1 / -2: couldn't */
static int cache_get(int chunk)
{
	pthread_mutex_lock(&c_lk);
	int i = cache_find(chunk);
	if (i >= 0) { C[i].used = ++tick; pthread_mutex_unlock(&c_lk); return i; }
	pthread_mutex_unlock(&c_lk);
	uint8_t *buf = calloc(CHUNK, SECTOR);
	if (!buf) return -1;
	int lba = chunk * CHUNK, n = CHUNK;
	if (lba + n > T.leadout) n = T.leadout - lba;
	if (n <= 0) { free(buf); return -1; }
	pthread_mutex_lock(&io_lk);
	int r = read_sectors(lba, n, buf);
	pthread_mutex_unlock(&io_lk);
	if (r) { free(buf); if (r == -2) gone = 1; return r; }
	pthread_mutex_lock(&c_lk);
	if ((i = cache_find(chunk)) < 0) {          /* (the read-ahead may have got there first) */
		i = cache_slot();
		free(C[i].data);
		C[i].data = buf;
		C[i].chunk = chunk;
	} else free(buf);
	C[i].used = ++tick;
	pthread_mutex_unlock(&c_lk);
	return i;
}

static void *ahead_thread(void *arg)
{
	(void)arg;
	for (;;) {
		pthread_mutex_lock(&c_lk);
		while (!nwant) pthread_cond_wait(&c_cv, &c_lk);
		int c = want[0];
		memmove(want, want + 1, sizeof(int) * (size_t)(--nwant));
		int have = cache_find(c) >= 0;
		pthread_mutex_unlock(&c_lk);
		if (!have && !gone) cache_get(c);
	}
	return NULL;
}

static void ahead(int chunk)
{
	pthread_mutex_lock(&c_lk);
	nwant = 0;
	for (int k = 1; k <= AHEAD; k++)
		if ((chunk + k) * CHUNK < T.leadout && cache_find(chunk + k) < 0) want[nwant++] = chunk + k;
	if (nwant) pthread_cond_signal(&c_cv);
	pthread_mutex_unlock(&c_lk);
}

/* len bytes of the disc from sector lba, byte off */
static int disc_read(long lba, long off, uint8_t *out, size_t len)
{
	size_t done = 0;
	long pos = lba * SECTOR + off;
	int last = -1;
	while (done < len) {
		long s = pos / SECTOR;
		int chunk = (int)(s / CHUNK);
		int i = cache_get(chunk);
		if (i < 0) return i;
		pthread_mutex_lock(&c_lk);
		if (C[i].chunk != chunk && (i = cache_find(chunk)) < 0) {   /* the read-ahead reused it meanwhile: again */
			pthread_mutex_unlock(&c_lk);
			continue;
		}
		C[i].used = ++tick;
		long in = pos - (long)chunk * CHUNK * SECTOR;
		size_t n = (size_t)CHUNK * SECTOR - (size_t)in;
		if (n > len - done) n = len - done;
		memcpy(out + done, C[i].data + in, n);
		pthread_mutex_unlock(&c_lk);
		done += n;
		pos += (long)n;
		last = chunk;
	}
	if (last >= 0) ahead(last);
	return 0;
}

/* ---------- the files ---------- */

static char name[200] = "Disc", cue[8192];
static int ntracks;

static void track_file(char *out, size_t n, int i)
{
	if (ntracks == 1) snprintf(out, n, "%s.bin", name);
	else snprintf(out, n, ntracks >= 10 ? "%s (Track %02d).bin" : "%s (Track %d).bin", name, i);
}

static void make_cue(void)
{
	size_t o = 0;
	for (int i = T.first; i <= T.last; i++) {
		char f[300];
		track_file(f, sizeof(f), i);
		int off = T.lba[i] - T.start[i];
		o += (size_t)snprintf(cue + o, sizeof(cue) - o, "FILE \"%s\" BINARY\r\n  TRACK %02d %s\r\n", f, i,
		                      !T.data[i] ? "AUDIO" : T.mode[i] == 1 ? "MODE1/2352" : "MODE2/2352");
		if (off > 0)
			o += (size_t)snprintf(cue + o, sizeof(cue) - o, "    INDEX 00 00:00:00\r\n    INDEX 01 %02d:%02d:%02d\r\n", off / 4500, off / 75 % 60, off % 75);
		else o += (size_t)snprintf(cue + o, sizeof(cue) - o, "    INDEX 01 00:00:00\r\n");
		if (o >= sizeof(cue)) break;
	}
}

/* inodes: 1 the folder, 2 the .cue, 2 + track: a track's .bin */
static uint64_t file_size(uint64_t ino)
{
	if (ino == 2) return strlen(cue);
	int t = (int)ino - 2;
	return (uint64_t)(T.start[t + 1] - T.start[t]) * SECTOR;
}

static int file_name(uint64_t ino, char *out, size_t n)
{
	if (ino == 2) { snprintf(out, n, "%s.cue", name); return 0; }
	int t = (int)ino - 2;
	if (t < T.first || t > T.last) return -1;
	track_file(out, n, t);
	return 0;
}

/* ---------- FUSE ---------- */

static int ffd = -1;
static time_t started;

static void reply(uint64_t unique, int err, const void *data, size_t len)
{
	struct fuse_out_header h = { (uint32_t)(sizeof(h) + (err ? 0 : len)), err, unique };
	struct iovec v[2] = { { &h, sizeof(h) }, { (void *)data, err ? 0 : len } };
	if (writev(ffd, v, err || !len ? 1 : 2) < 0 && errno != ENOENT) logf_("cmdisc: reply: %s", strerror(errno));
}

static void attr_of(uint64_t ino, struct fuse_attr *a)
{
	memset(a, 0, sizeof(*a));
	a->ino = ino;
	a->mode = ino == 1 ? (S_IFDIR | 0555) : (S_IFREG | 0444);
	a->nlink = ino == 1 ? 2 : 1;
	a->size = ino == 1 ? 0 : file_size(ino);
	a->blocks = (a->size + 511) / 512;
	a->atime = a->mtime = a->ctime = (uint64_t)started;
	a->blksize = SECTOR * 16;
}

static void serve(void)
{
	size_t bsz = 1 << 20;
	uint8_t *buf = malloc(bsz), *out = malloc(1 << 20);
	unsigned minor = 26;
	for (;;) {
		ssize_t n = read(ffd, buf, bsz);
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN || errno == ENOENT) continue;
			if (errno == ENODEV) return;             /* unmounted */
			logf_("cmdisc: read: %s", strerror(errno));
			return;
		}
		if ((size_t)n < sizeof(struct fuse_in_header)) continue;
		struct fuse_in_header *in = (struct fuse_in_header *)buf;
		void *arg = buf + sizeof(*in);
		switch (in->opcode) {
		case FUSE_INIT: {
			struct fuse_init_in *ii = arg;
			struct fuse_init_out o = { 0 };
			o.major = FUSE_KERNEL_VERSION;
			o.minor = minor = ii->minor < 26 ? ii->minor : 26;
			o.max_readahead = ii->max_readahead;
			o.flags = 0;
			o.max_background = 4;
			o.congestion_threshold = 3;
			o.max_write = 4096;
			o.time_gran = 1;
			reply(in->unique, 0, &o, minor < 23 ? FUSE_COMPAT_22_INIT_OUT_SIZE : sizeof(o));
			break;
		}
		case FUSE_LOOKUP: {
			const char *nm = arg;
			if (in->nodeid != 1) { reply(in->unique, -ENOENT, NULL, 0); break; }
			uint64_t ino = 0;
			char f[300];
			for (uint64_t k = 2; k <= (uint64_t)T.last + 2; k++)
				if (!file_name(k, f, sizeof(f)) && !strcmp(f, nm)) { ino = k; break; }
			if (!ino) { reply(in->unique, -ENOENT, NULL, 0); break; }
			struct fuse_entry_out e = { 0 };
			e.nodeid = ino;
			e.entry_valid = e.attr_valid = 3600;
			attr_of(ino, &e.attr);
			reply(in->unique, 0, &e, minor < 9 ? FUSE_COMPAT_ENTRY_OUT_SIZE : sizeof(e));
			break;
		}
		case FUSE_GETATTR: {
			struct fuse_attr_out a = { 0 };
			a.attr_valid = 3600;
			attr_of(in->nodeid, &a.attr);
			reply(in->unique, 0, &a, minor < 9 ? FUSE_COMPAT_ATTR_OUT_SIZE : sizeof(a));
			break;
		}
		case FUSE_OPEN:
		case FUSE_OPENDIR: {
			struct fuse_open_out o = { 0 };
			o.open_flags = in->opcode == FUSE_OPEN ? FOPEN_KEEP_CACHE : 0;
			reply(in->unique, 0, &o, sizeof(o));
			break;
		}
		case FUSE_READ: {
			struct fuse_read_in *r = arg;
			uint64_t size = file_size(in->nodeid), off = r->offset, len = r->size;
			if (off >= size) { reply(in->unique, 0, out, 0); break; }
			if (len > size - off) len = size - off;
			if (len > (1 << 20)) len = 1 << 20;
			if (in->nodeid == 2) { reply(in->unique, 0, cue + off, len); break; }
			int t = (int)in->nodeid - 2;
			struct timespec t0, t1;
			clock_gettime(CLOCK_MONOTONIC, &t0);
			int rc = disc_read(T.start[t], (long)off, out, len);
			clock_gettime(CLOCK_MONOTONIC, &t1);
			logf_("read track %d at %llu, %llu bytes: %ld ms%s", t, (unsigned long long)off, (unsigned long long)len,
			      (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000), rc ? " (failed)" : "");
			if (rc) reply(in->unique, rc == -2 ? -ENOMEDIUM : -EIO, NULL, 0);
			else reply(in->unique, 0, out, len);
			break;
		}
		case FUSE_READDIR: {
			struct fuse_read_in *r = arg;
			size_t o = 0;
			for (uint64_t k = r->offset; k < (uint64_t)(T.last - T.first + 1) + 4; k++) {
				char f[300];
				uint64_t ino;
				int type = DT_REG;
				if (k == 0) strcpy(f, "."), ino = 1, type = DT_DIR;
				else if (k == 1) strcpy(f, ".."), ino = 1, type = DT_DIR;
				else if (k == 2) ino = 2, file_name(2, f, sizeof(f));
				else { ino = 2 + (uint64_t)T.first + (k - 3); if (file_name(ino, f, sizeof(f))) break; }
				size_t nl = strlen(f), ent = FUSE_DIRENT_ALIGN(FUSE_NAME_OFFSET + nl);
				if (o + ent > r->size) break;
				struct fuse_dirent *d = (struct fuse_dirent *)(out + o);
				memset(d, 0, ent);
				d->ino = ino;
				d->off = k + 1;
				d->namelen = (uint32_t)nl;
				d->type = (uint32_t)type;
				memcpy(d->name, f, nl);
				o += ent;
			}
			reply(in->unique, 0, out, o);
			break;
		}
		case FUSE_STATFS: {
			struct fuse_statfs_out s = { 0 };
			s.st.bsize = s.st.frsize = SECTOR;
			s.st.blocks = (uint64_t)T.leadout;
			s.st.namelen = 255;
			reply(in->unique, 0, &s, sizeof(s));
			break;
		}
		case FUSE_RELEASE:
		case FUSE_RELEASEDIR:
		case FUSE_FLUSH:
		case FUSE_ACCESS:
			reply(in->unique, 0, NULL, 0);
			break;
		case FUSE_FORGET:
		case FUSE_BATCH_FORGET:
		case FUSE_INTERRUPT:
			break;
		case FUSE_DESTROY:
			reply(in->unique, 0, NULL, 0);
			return;
		default:
			reply(in->unique, -ENOSYS, NULL, 0);
		}
	}
}

/* ---------- the commands ---------- */

static const char *state_name(int s)
{
	switch (s) {
	case CDS_NO_DISC: return "empty";
	case CDS_TRAY_OPEN: return "open";
	case CDS_DRIVE_NOT_READY: return "notready";
	case CDS_DISC_OK: return "ready";
	default: return "none";
	}
}

static int load(const char *image, const char **type)
{
	if (image) {
		int r = open_image(image);
		if (r == -2) { printf("error=%s has 2048-byte sectors: only raw (2352-byte) images can stand in for a disc\n", image); return 1; }
		if (r) { printf("error=couldn't read %s\n", image); return 1; }
	}
	else {
		int s = drive_state();
		if (s != CDS_DISC_OK) { printf("state=%s\n", s < 0 ? "none" : state_name(s)); return 1; }
		if (open_drive()) { printf("state=ready\nerror=couldn't read the disc's table of contents\n"); return 1; }
	}
	*type = disc_type();
	if (!strcmp(*type, "unknown") && neogeo_cd()) *type = "neogeocd";
	layout(*type);
	read_serial(*type);
	ntracks = T.last - T.first + 1;
	return 0;
}

/* the core for each kind of disc, and how MiSTer's .mgl loads a disc into it (type s, the core's
   disc slot): the same slots Main's own file browser uses (menu.cpp) */
static const struct { const char *type, *rbf, *name; int index; } CORE[] = {
	{ "psx", "_Console/PSX", "PlayStation", 1 }, { "audio", "_Console/PSX", "PlayStation (its CD player)", 1 },
	{ "saturn", "_Console/Saturn", "Saturn", 0 }, { "megacd", "_Console/MegaCD", "Mega CD", 0 },
	{ "pcecd", "_Console/TurboGrafx16", "PC Engine CD", 0 }, { "neogeocd", "_Console/NeoGeo", "Neo Geo CD", 1 },
	{ "3do", "_Console/3DO", "3DO", 0 }, { "cdi", "_Console/CDi", "CD-i", 0 }, { NULL, NULL, NULL, 0 },
};

static int core_of(const char *type)
{
	for (int i = 0; CORE[i].type; i++) if (!strcmp(CORE[i].type, type)) return i;
	return -1;
}

/* the core's newest .rbf is there (MiSTer picks the newest itself; this only says whether there's one) */
static int core_installed(const char *rbf)
{
	char dir[300], base[120];
	snprintf(dir, sizeof(dir), "/media/fat/%s", rbf);
	char *sl = strrchr(dir, '/');
	if (!sl) return 0;
	snprintf(base, sizeof(base), "%s_", sl + 1);
	*sl = 0;
	DIR *d = opendir(dir);
	if (!d) return 0;
	struct dirent *e;
	int found = 0;
	while ((e = readdir(d)) && !found) found = !strncmp(e->d_name, base, strlen(base)) && strstr(e->d_name, ".rbf");
	closedir(d);
	return found;
}

static int core_running(void)
{
	char c[64] = "";
	FILE *f = fopen("/tmp/CORENAME", "r");
	if (f) { if (!fgets(c, sizeof(c), f)) c[0] = 0; fclose(f); }
	c[strcspn(c, "\r\n")] = 0;
	return c[0] && strcmp(c, "MENU");
}

static int mister_cmd(const char *line)
{
	int fd = open("/dev/MiSTer_cmd", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) return -1;
	ssize_t w = write(fd, line, strlen(line));
	close(fd);
	return w == (ssize_t)strlen(line) ? 0 : -1;
}

static int mounted(const char *dir)
{
	FILE *f = fopen("/proc/mounts", "r");
	if (!f) return 0;
	char l[1500];
	int yes = 0;
	size_t n = strlen(dir);
	while (!yes && fgets(l, sizeof(l), f)) yes = !strncmp(l, "cmdisc ", 7) && !strncmp(l + 7, dir, n) && l[7 + n] == ' ';
	fclose(f);
	return yes;
}

static void unmount(const char *dir)
{
	for (int i = 0; i < 3 && mounted(dir); i++) umount2(dir, MNT_DETACH);
	rmdir(dir);
}

/* mount the disc in dir and serve it from a background process; the parent returns once it's up */
static int mount_bg(const char *dir, const char *type, int quiet)
{
	make_cue();
	started = time(NULL);
	unmount(dir);
	mkdir(dir, 0755);
	ffd = open("/dev/fuse", O_RDWR | O_CLOEXEC);
	if (ffd < 0) { printf("error=no /dev/fuse: %s\n", strerror(errno)); return 1; }
	char opts[200];
	snprintf(opts, sizeof(opts), "fd=%d,rootmode=40000,user_id=0,group_id=0,allow_other,max_read=131072", ffd);
	if (mount("cmdisc", dir, "fuse.cmdisc", MS_NOSUID | MS_NODEV | MS_RDONLY, opts)) {
		printf("error=couldn't mount %s: %s\n", dir, strerror(errno));
		close(ffd);
		return 1;
	}
	pid_t p = fork();
	if (p < 0) { unmount(dir); return 1; }
	if (p > 0) {
		close(ffd);
		if (!quiet) printf("type=%s\nserial=%s\ntracks=%d\ncue=%s/%s.cue\n", type, serial, ntracks, dir, name);
		return 0;
	}
	setsid();
	int nul = open("/dev/null", O_RDWR);
	if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); if (!getenv("CMDISC_DEBUG")) dup2(nul, 2); }
	signal(SIGPIPE, SIG_IGN);
	pthread_t th;
	pthread_create(&th, NULL, ahead_thread, NULL);
	serve();
	unmount(dir);
	_exit(0);
}

/* play: the disc's core started with the disc (the files in dir) */
static int play(const char *dir, const char *type)
{
	int c = core_of(type);
	if (c < 0) { printf("error=this disc isn't one Console Mode's cores play (%s)\n", type); return 1; }
	if (!core_installed(CORE[c].rbf)) { printf("error=the %s core isn't installed (%s)\n", CORE[c].name, CORE[c].rbf); return 1; }
	if (serial[0] && !getenv("CMDISC_KEEP_NAME")) snprintf(name, sizeof(name), "%s", serial);   /* saves follow the game */
	else if (!strcmp(type, "audio")) snprintf(name, sizeof(name), "Audio CD");
	if (mount_bg(dir, type, 1)) return 1;
	char mgl[] = "/tmp/cmdisc.mgl", line[300];
	FILE *f = fopen(mgl, "w");
	if (!f) { printf("error=couldn't write %s\n", mgl); return 1; }
	fprintf(f, "<mistergamedescription>\n\t<rbf>%s</rbf>\n\t<file delay=\"1\" type=\"s\" index=\"%d\" path=\"%s/%s.cue\"/>\n</mistergamedescription>\n",
	        CORE[c].rbf, CORE[c].index, dir, name);
	fclose(f);
	snprintf(line, sizeof(line), "load_core %s\n", mgl);
	if (mister_cmd(line)) { printf("error=MiSTer didn't take the command (/dev/MiSTer_cmd)\n"); unmount(dir); return 1; }
	printf("type=%s\nserial=%s\ncore=%s\ncue=%s/%s.cue\nplaying=1\n", type, serial, CORE[c].name, dir, name);
	return 0;
}

static int eject(void)
{
	int fd = open(devpath, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) { printf("error=no drive (%s)\n", devpath); return 1; }
	ioctl(fd, CDROM_LOCKDOOR, 0);
	int r = ioctl(fd, CDROMEJECT, 0);
	close(fd);
	printf(r ? "error=the drive didn't open\n" : "ejected=1\n");
	return r ? 1 : 0;
}

/* watch: a disc put in starts its game (only while Console Mode's menu is up); taken out, its files go */
static int watch(const char *dir)
{
	int had = 0;
	for (;;) {
		int s = drive_state();
		if (s == CDS_DISC_OK && !had) {
			had = 1;
			if (!core_running() && !mounted(dir)) {
				pid_t p = fork();
				if (p == 0) {
					const char *type;
					if (!load(NULL, &type)) { fflush(stdout); play(dir, type); }
					fflush(stdout);
					_exit(0);
				}
				if (p > 0) waitpid(p, NULL, 0);
				if (dfd >= 0) { close(dfd); dfd = -1; }
			}
		} else if (s != CDS_DISC_OK && s != CDS_DRIVE_NOT_READY) {
			if (had && mounted(dir)) unmount(dir);
			had = 0;
		}
		sleep(2);
	}
	return 0;
}

/* LibCrypt: Main reads a PlayStation disc's .sbi from sbi.zip in games/PSX (the community pack,
   named by product code); 1 the game has one there, 0 it hasn't, -1 no pack */
static int sbi_state(const char *code)
{
	static const char *roots[] = { "/media/usb0", "/media/usb1", "/media/usb2", "/media/usb3", "/media/fat", NULL };
	int pack = 0;
	char want[64];
	snprintf(want, sizeof(want), "%s.sbi", code);
	for (int r = 0; roots[r]; r++) {
		char dir[200];
		snprintf(dir, sizeof(dir), "%s/games/PSX", roots[r]);
		DIR *d = opendir(dir);
		if (!d) continue;
		struct dirent *e;
		while ((e = readdir(d))) {
			if (strcasecmp(e->d_name, "sbi.zip")) continue;
			char p[500];
			snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
			FILE *f = fopen(p, "rb");
			if (!f) continue;
			pack = 1;
			static char buf[1 << 20];
			size_t n = fread(buf, 1, sizeof(buf), f);
			fclose(f);
			if (*code && memmem(buf, n, want, strlen(want))) { closedir(d); return 1; }
		}
		closedir(d);
	}
	return pack ? 0 : -1;
}

static void default_dir(char *d, size_t n, const char *arg) { snprintf(d, n, "%s", arg ? arg : "/tmp/cmdisc"); }

int main(int argc, char **argv)
{
	const char *image = NULL, *dirarg = NULL, *cmd = argc > 1 ? argv[1] : "";
	int bg = 0;
	for (int a = 2; a < argc; a++) {
		if (!strcmp(argv[a], "-b")) bg = 1;
		else if (!strcmp(argv[a], "--name") && a + 1 < argc) snprintf(name, sizeof(name), "%s", argv[++a]);
		else if (!strcmp(argv[a], "--dev") && a + 1 < argc) devpath = argv[++a];
		else if (!strcmp(argv[a], "--image") && a + 1 < argc) image = argv[++a];
		else if (argv[a][0] != '-') dirarg = argv[a];
	}
	for (char *p = name; *p; p++) if (*p == '/' || *p == '"' || (unsigned char)*p < 32) *p = '_';
	for (int i = 0; i < NCHUNKS; i++) C[i].chunk = -1;
	char dir[1024];
	default_dir(dir, sizeof(dir), dirarg);
	const char *type;

	if (!strcmp(cmd, "status")) {
		if (load(image, &type)) return 0;
		int c = core_of(type);
		printf("state=ready\ntype=%s\nserial=%s\ncore=%s\ncore_installed=%d\ntracks=%d\nsectors=%d\nplaying=%d\n", type, serial,
		       c >= 0 ? CORE[c].name : "", c >= 0 && core_installed(CORE[c].rbf), ntracks, T.leadout, mounted(dir));
		if (!strcmp(type, "psx")) {
			int sb = sbi_state(serial);
			printf("sbi_pack=%s\nsbi=%d\n", sb < 0 ? "missing" : "found", sb > 0);
		}
		for (int i = T.first; i <= T.last; i++)
			printf("track%d=%s %d %d\n", i, !T.data[i] ? "audio" : T.mode[i] == 1 ? "mode1" : "mode2", T.lba[i], T.start[i + 1] - T.start[i]);
		return 0;
	}
	if (!strcmp(cmd, "umount") || !strcmp(cmd, "stop")) {
		unmount(dir);
		printf("stopped=1\n");
		return 0;
	}
	if (!strcmp(cmd, "eject")) {
		unmount(dir);
		return eject();
	}
	if (!strcmp(cmd, "watch")) return watch(dir);
	if (!strcmp(cmd, "play")) return load(image, &type) ? 1 : play(dir, type);
	if (!strcmp(cmd, "mount")) {
		if (load(image, &type)) return 1;
		if (bg) return mount_bg(dir, type, 0);
		make_cue();
		started = time(NULL);
		unmount(dir);
		mkdir(dir, 0755);
		ffd = open("/dev/fuse", O_RDWR | O_CLOEXEC);
		if (ffd < 0) { printf("error=no /dev/fuse: %s\n", strerror(errno)); return 1; }
		char opts[200];
		snprintf(opts, sizeof(opts), "fd=%d,rootmode=40000,user_id=0,group_id=0,allow_other,max_read=131072", ffd);
		if (mount("cmdisc", dir, "fuse.cmdisc", MS_NOSUID | MS_NODEV | MS_RDONLY, opts)) { printf("error=couldn't mount %s: %s\n", dir, strerror(errno)); return 1; }
		printf("type=%s\nserial=%s\ntracks=%d\ncue=%s/%s.cue\n", type, serial, ntracks, dir, name);
		fflush(stdout);
		signal(SIGPIPE, SIG_IGN);
		pthread_t th;
		pthread_create(&th, NULL, ahead_thread, NULL);
		serve();
		unmount(dir);
		return 0;
	}
	fprintf(stderr, "usage: cmdisc status | play | mount [-b] [dir] | stop | eject | watch   [--name N] [--dev /dev/srN] [--image file.cue]\n");
	return 2;
}
