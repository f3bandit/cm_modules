/* ss1remote: Update All for the apps (see ua.c). Copyright (C) 2026 f3bandit. GPL-3.0-or-later (see ../LICENSE). */
#ifndef SS1_UA_H
#define SS1_UA_H

#include "vendor/cJSON.h"

int ua_installed(void);
/* the "ua" command: {"do":"list"|"act"|"allow"|"preview"|"uninstall"|"install"|"leftovers"|"clean_ao"|"run","args":[...]}; NULL + *err on failure */
cJSON *ua_cmd(cJSON *req, const char **err);
/* now and then: stops the bridge after 5 minutes unused */
void ua_tick(void);

#endif
