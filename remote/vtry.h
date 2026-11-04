/* cmremote: MiSTer's own video settings (MiSTer.ini [MiSTer]), tried with Keep or an automatic put-back.
 * Copyright (C) 2026 f3bandit. PolyForm Noncommercial 1.0.0 (see ../LICENSE.md); Retro Remake: see ../LICENSE-RETRO-REMAKE.md.
 *
 *   {cmd:"vset" [, file]}                      files (profiles), active, file, settings, pending, secs
 *                                              (file: the ini to show and change; default the one in use)
 *   {cmd:"vset", try:{key:value|null}, secs}   try (null/"" = MiSTer's default); restarts Main
 *   {cmd:"vset", profile:N}                    try switching to profile N (0 MiSTer.ini, 1-3)
 *   {cmd:"vset", keep:true}                    keep the change that's being tried
 *   {cmd:"vset", revert:true}                  put it back now
 *   {cmd:"vset", undo:true}                    try the copy from before the last change */
#ifndef CMREMOTE_VTRY_H
#define CMREMOTE_VTRY_H
#include "vendor/cJSON.h"

#define VT_SECS 35   /* how long a try waits for Keep */

cJSON *vt_cmd(cJSON *req, const char **err);   /* the reply (no "ok"), or NULL with *err */
void vt_boot(void);                            /* at start: a try that was never kept is put back */
cJSON *vt_read(const char *text);
char *vt_set(const char *text, const char *key, const char *value);
int vt_fpga_yc(void);                          /* Main's video settings now: vga_mode svideo or cvbs */
int vt_altcfg(int set);                        /* the profile Main uses (-1 reads; -1 back: unknown) */
#endif
