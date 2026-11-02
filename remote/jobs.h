/* ss1remote: the long jobs (update-guard: Update All runs, rollbacks, restores) and shared helpers.
 * Copyright (C) 2026 f3bandit. GPL-3.0-or-later (see ../LICENSE); Retro Remake: see ../LICENSE-RETRO-REMAKE.md. */
#ifndef SS1_JOBS_H
#define SS1_JOBS_H

#include "vendor/cJSON.h"
#include "cmlib.h"

void plog(const char *fmt, ...);
const char *ss1_home(void);       /* /media/fat/ConsoleMode/ss1 */
const char *ss1_tmp(void);        /* /tmp/ss1remote */
int file_exists(const char *p);

/* one job at a time: update-guard.sh <args>. name: ua_run, rollback, restore, keep, delete. NULL started, or why not */
const char *job_start(const char *name, const char *arg0, const char *const *args, int nargs);
/* the commands these answer ("ua", "guard", "job"); NULL when it's another one */
cJSON *jobs_api(cJSON *req);
/* at start: update-guard's boot step (copies of the settings inside Linux, the Linux version) */
void jobs_boot(void);

#endif
