/* cmremote: the few paths and helpers the ported tools (vtry.c, extras.c) ask for.
 * Copyright (C) 2026 f3bandit. PolyForm Noncommercial 1.0.0 (see ../LICENSE.md); Retro Remake: see ../LICENSE-RETRO-REMAKE.md. */
#ifndef CMREMOTE_COMPAT_H
#define CMREMOTE_COMPAT_H
#include "jobs.h"
#include "cmlib.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

static inline const char *p_fat(void) { return cm_fat; }
static inline const char *p_tmp(void) { return cmod_tmp(); }
static inline const char *p_data(void) { return cmod_home(); }
static inline const char *p_corename(void) { const char *v = getenv("CMOD_CORENAME"); return v && *v ? v : "/tmp/CORENAME"; }
static inline const char *p_mister_cmd(void) { const char *v = getenv("CMOD_MISTER_CMD"); return v && *v ? v : "/dev/MiSTer_cmd"; }
static inline int mkdirs(const char *dir)
{
	char d[1024];
	snprintf(d, sizeof(d), "%s", dir);
	for (char *p = d + 1; *p; p++) {
		if (*p == '/') {
			*p = 0;
			if (mkdir(d, 0755) != 0 && errno != EEXIST) return -1;
			*p = '/';
		}
	}
	return mkdir(d, 0755) != 0 && errno != EEXIST ? -1 : 0;
}
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
/* one command to MiSTer's Main (its FIFO); 0 sent */
static inline int mister_cmd(const char *cmd, char *err, size_t errn)
{
	const char *dev = p_mister_cmd();
	int fd = open(dev, O_WRONLY | O_NONBLOCK | O_APPEND | (strncmp(dev, "/dev/", 5) ? O_CREAT : 0), 0644);
	if (fd < 0) { snprintf(err, errn, "MiSTer isn't listening on %s (%s)", dev, strerror(errno)); return -1; }
	ssize_t w = write(fd, cmd, strlen(cmd));
	close(fd);
	if (w < 0) { snprintf(err, errn, "couldn't send the command to MiSTer (%s)", strerror(errno)); return -1; }
	plog("sent: %.*s", (int)strcspn(cmd, "\n"), cmd);
	return 0;
}
#endif
