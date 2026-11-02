/* ss1remote: Console Mode's game library, read the way Console Mode lists it, answered in the shape of
 * Phosphor's remote access commands (docs/remote-api.md in phosphor_os), so the same apps work.
 * Copyright (C) 2026 f3bandit. GPL-3.0-or-later (see ../LICENSE); Retro Remake: see ../LICENSE-RETRO-REMAKE.md. */
#ifndef SS1_CMLIB_H
#define SS1_CMLIB_H

#include "vendor/cJSON.h"

/* where things are; tests point these elsewhere */
extern const char *cm_dir;        /* /media/fat/ConsoleMode */
extern const char *cm_fat;        /* /media/fat */
extern const char *cm_drives[];   /* the drives games are looked for on, NULL-terminated */

/* one command ({"cmd":...}); a JSON reply, {"ok":false,"error":...} when it can't. Thread-safe. */
cJSON *cm_api(cJSON *req);

/* a game's box art: its file (PNG), 0; -1 when it has none */
int cm_art_path(long id, char *out, size_t n);
/* new box art for a game, from an image file (PNG, JPEG, GIF, BMP...): stored as Console Mode's PNG */
cJSON *cm_art_set(long id, const char *src);

/* Console Mode's font file: "regular" or "bold"; 0 found */
int cm_font_path(const char *which, char *out, size_t n);

/* ids: the same game keeps its id (a hash of where it is) */
long cm_game_id(const char *path);

#endif
