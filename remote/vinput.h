/* The apps' remote control: a virtual keyboard and controller on the SuperStation (uinput).
 * Copyright (C) 2026 f3bandit. All rights reserved; licensed to Retro Remake (see ../LICENSE.md). */
#pragma once
#include "vendor/cJSON.h"

/* {cmd:"vinput", ...}: see vinput.c; NULL and *err for a bad request */
cJSON *vinput_command(cJSON *req, const char **err);
