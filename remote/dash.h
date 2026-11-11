/* cmremote: the Diagnostics dashboard (dash.c). Copyright (C) 2026 f3bandit. All rights reserved; licensed to Retro Remake. */
#ifndef CMOD_DASH_H
#define CMOD_DASH_H
#include "vendor/cJSON.h"
cJSON *dash_json(int with_kernel);
cJSON *drives_json(void);   /* {"cmd":"drives"}: the drives' dirty marks */
#endif
