/* cmremote: the long jobs (update-guard: Update All runs, rollbacks, restores) and shared helpers.
 * Copyright (C) 2026 f3bandit. All rights reserved; licensed to Retro Remake (see ../LICENSE.md). */
#ifndef CMOD_JOBS_H
#define CMOD_JOBS_H

#include "vendor/cJSON.h"
#include "cmlib.h"

void plog(const char *fmt, ...);
const char *cmod_home(void);       /* /media/fat/ConsoleMode/tools */
int cmod_backup_kind_ok(const char *kind);   /* a backups/ kind the apps keep a copy of on the PC */
const char *cmod_tmp(void);        /* /tmp/cmremote */
int file_exists(const char *p);

/* one job at a time: update-guard.sh <args>. name: ua_run, rollback, restore, keep, delete. NULL started, or why not */
const char *job_start(const char *name, const char *arg0, const char *const *args, int nargs);
/* any program as the one job ({"cmd":"job"} shows its output); NULL started, else why not */
const char *job_run(const char *name, const char *const *argv);
/* runs argv, its output (malloc'd, NULL if it didn't run); killed after limit seconds */
char *run_capture(const char *const *argv, int limit);
/* the commands these answer ("ua", "guard", "job"); NULL when it's another one */
cJSON *jobs_api(cJSON *req);
/* at start: update-guard's boot step (copies of the settings inside Linux, the Linux version) */
void jobs_boot(void);

#endif
