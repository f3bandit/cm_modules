/* cmremote: MiSTer & Linux backups for the apps (the system files and the settings files, read and
 * written back in pieces). See sysbk.c for the commands.
 * Copyright (C) 2026 f3bandit. PolyForm Noncommercial 1.0.0 (see ../LICENSE.md). */
#ifndef CMREMOTE_SYSBK_H
#define CMREMOTE_SYSBK_H
#include "vendor/cJSON.h"
cJSON *sysbk_cmd(cJSON *req, const char **err);   /* the reply (no "ok"), or NULL with *err */
#endif
