/* cmremote: what the Windows app's Devices, Network and Advanced sections read (and MiSTer.ini, scripts).
 * Copyright (C) 2026 f3bandit. All rights reserved; licensed to Retro Remake (see ../LICENSE.md). */
#ifndef CMREMOTE_EXTRAS_H
#define CMREMOTE_EXTRAS_H
#include "vendor/cJSON.h"
#include <stddef.h>
/* each returns the reply object (no "ok"), or NULL with *err set */
cJSON *ex_network(void);
cJSON *ex_usb(void);
cJSON *ex_ini(cJSON *req, const char **err);
cJSON *ex_scripts(cJSON *req, const char **err);
/* keep <fat>/<file> in phosphor/backups/ini/<file>.<date> (0 also when there's no file yet; *kept "" then) */
int ex_ini_keep(const char *file, char *kept, size_t kn);
#endif
