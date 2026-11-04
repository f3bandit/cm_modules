/* cmremote: the whole-system checks and the support report ({"cmd":"diag"[,"do":"run"|"bundle"]}).
 * Copyright (C) 2026 f3bandit. PolyForm Noncommercial 1.0.0 (see ../LICENSE.md). */
#ifndef CMREMOTE_DIAG_H
#define CMREMOTE_DIAG_H
#include "vendor/cJSON.h"
cJSON *diag_api(cJSON *req);
#endif
