/* cmremote: the Diagnostics dashboard (dash.c). Copyright (C) 2026 f3bandit. PolyForm Noncommercial 1.0.0. */
#ifndef CMOD_DASH_H
#define CMOD_DASH_H
#include "vendor/cJSON.h"
cJSON *dash_json(int with_kernel);
cJSON *drives_json(void);   /* {"cmd":"drives"}: the drives' dirty marks */
#endif
