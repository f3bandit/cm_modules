/* The apps' remote control: a virtual keyboard and controller on the SuperStation (uinput).
 * Copyright (C) 2026 f3bandit. PolyForm Noncommercial 1.0.0 (see ../LICENSE.md); Retro Remake: see ../LICENSE-RETRO-REMAKE.md. */
#pragma once
#include "vendor/cJSON.h"

/* {cmd:"vinput", ...}: see vinput.c; NULL and *err for a bad request */
cJSON *vinput_command(cJSON *req, const char **err);
