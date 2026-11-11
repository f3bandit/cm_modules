/* cmnvme: the NVMe drive behind the SuperStation dock's JMicron USB bridge (JMS583 and alike), which
 * otherwise only says "JMicron Generic". Read-only NVMe admin commands through JMicron's pass-through,
 * the three steps smartmontools uses for "-d sntjmicron": the command, the data, the response.
 *
 *   cmnvme -nvmeid /dev/sdX      refused: the dock's bridge resets on Identify (see identify())
 *   cmnvme -nvmesmart /dev/sdX   key=value lines from the health log (log page 02h): temperature,
 *                                   wear, spare, written / read, power-on hours, errors...
 *   cmnvme -temp /dev/sdX        just the temperature in °C
 *
 * Copyright (C) 2026 f3bandit. All rights reserved; licensed to Retro Remake (see LICENSE.md). */
#include <errno.h>
#include <fcntl.h>
#include <scsi/sg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define JM_SIG 0x454d564eu        /* "NVME", little endian */
#define JM_CMD 512

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static int sg(int fd, int proto, int dir, uint8_t *buf, unsigned n)
{
	uint8_t cdb[12] = { 0xA1, (uint8_t)(0x80 | proto), 0, (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n };
	uint8_t sense[32];
	sg_io_hdr_t h;
	memset(&h, 0, sizeof(h));
	h.interface_id = 'S';
	h.dxfer_direction = dir;
	h.cmd_len = sizeof(cdb);
	h.mx_sb_len = sizeof(sense);
	h.dxfer_len = n;
	h.dxferp = buf;
	h.cmdp = cdb;
	h.sbp = sense;
	h.timeout = 5000;
	if (ioctl(fd, SG_IO, &h) < 0) return -1;
	if (getenv("CMNVME_DEBUG"))
		fprintf(stderr, "sg proto %x len %u: status %#x host %#x driver %#x sense %02x %02x %02x %02x resid %d\n", proto, n,
		        h.status, h.host_status, h.driver_status, sense[0], sense[2] & 0x0f, sense[12], sense[13], h.resid);
	if (h.status || h.host_status || (h.driver_status & 0x0f)) { errno = EIO; return -1; }
	return 0;
}

/* one read-only admin command; the data in out (size bytes) */
static int jm_admin(const char *dev, uint32_t opcode, uint32_t nsid, uint32_t cdw10, uint8_t *out, unsigned size)
{
	int fd = open(dev, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) return -1;
	uint8_t cmd[JM_CMD] = { 0 }, reply[JM_CMD] = { 0 };
	put32(cmd, JM_SIG);
	put32(cmd + 8, opcode);
	put32(cmd + 12, nsid);
	put32(cmd + 48, cdw10);
	int r = -1;
	if (sg(fd, 0x0, SG_DXFER_TO_DEV, cmd, JM_CMD) == 0 && sg(fd, 0x2, SG_DXFER_FROM_DEV, out, size) == 0 &&
	    sg(fd, 0xF, SG_DXFER_FROM_DEV, reply, JM_CMD) == 0) {
		if (le32(reply) != JM_SIG) errno = ENODEV;           /* not a JMicron NVMe bridge */
		else if (le32(reply + 20) >> 17) { errno = EIO; if (getenv("CMNVME_DEBUG")) fprintf(stderr, "nvme status %#x\n", le32(reply + 20) >> 17); }
		else r = 0;
	}
	close(fd);
	return r;
}

/* NVMe's 128-bit little-endian counters, as a decimal string (they fit in 64 bits in practice) */
static void u128(const uint8_t *p, char *out, size_t n)
{
	unsigned long long v = 0;
	for (int i = 7; i >= 0; i--) v = v << 8 | p[i];
	int big = 0;
	for (int i = 8; i < 16; i++) if (p[i]) big = 1;
	snprintf(out, n, big ? "%llu+" : "%llu", v);
}

/* Identify Controller: not sent. The SuperStation dock's JMicron bridge (152d:0583) times out on it, at 4 KB
   and at 512 bytes, and USB then resets the dock with its drive mounted. The health log (below) works.
   Kept as a refusal, so callers (sd_integrity.sh's drive_gen) fall back to "model not known". */
static int identify(const char *dev)
{
	(void)dev;
	fprintf(stderr, "cmnvme -nvmeid: not asked (the dock's bridge resets on Identify)\n");
	return 1;
}

static int health(const char *dev, int temp_only)
{
	uint8_t d[512];
	if (jm_admin(dev, 0x02, 0xFFFFFFFFu, 0x02 | ((512 / 4 - 1) << 16), d, sizeof(d))) {
		fprintf(stderr, "cmnvme -nvmesmart: %s\n", strerror(errno));
		return 1;
	}
	int t = (int)le16(d + 1) - 273;
	if (temp_only) { printf("%d\n", t); return 0; }
	char b[48];
	printf("critical_warning=%d\ntemperature_c=%d\navailable_spare=%d\nspare_threshold=%d\npercentage_used=%d\n", d[0], t, d[3], d[4], d[5]);
	static const struct { const char *k; int at; } C[] = {
		{ "data_read_units", 32 }, { "data_written_units", 48 }, { "power_cycles", 112 }, { "power_on_hours", 128 },
		{ "unsafe_shutdowns", 144 }, { "media_errors", 160 }, { "error_log_entries", 176 },
	};
	for (size_t i = 0; i < sizeof(C) / sizeof(C[0]); i++) { u128(d + C[i].at, b, sizeof(b)); printf("%s=%s\n", C[i].k, b); }
	int s1 = le16(d + 200);
	if (s1 > 0) printf("temp_sensor1_c=%d\n", s1 - 273);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 3 && !strcmp(argv[1], "-nvmeid")) return identify(argv[2]);
	if (argc == 3 && !strcmp(argv[1], "-nvmesmart")) return health(argv[2], 0);
	if (argc == 3 && !strcmp(argv[1], "-temp")) return health(argv[2], 1);
	fprintf(stderr, "cmnvme -nvmeid|-nvmesmart|-temp /dev/sdX   (an NVMe drive behind a JMicron USB bridge)\n");
	return 2;
}
