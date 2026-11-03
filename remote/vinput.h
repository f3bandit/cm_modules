/* The apps' remote control: a virtual keyboard and controller on the SuperStation (uinput).
 * Copyright (C) 2026 f3bandit. GPL-3.0-or-later (see ../LICENSE); Retro Remake: see ../LICENSE-RETRO-REMAKE.md. */
#pragma once
#include "vendor/cJSON.h"

/* {cmd:"vinput", ...}: see vinput.c; NULL and *err for a bad request */
cJSON *vinput_command(cJSON *req, const char **err);
