/* cmremote: SHA-256 (FIPS 180-4), for the app link's device keys.
 * Copyright (C) 2026 f3bandit. GPL-3.0-or-later (see ../LICENSE); Retro Remake: see ../LICENSE-RETRO-REMAKE.md. */
#ifndef PHOSD_SHA256_H
#define PHOSD_SHA256_H

#include <stddef.h>
#include <stdint.h>

void sha256(const void *data, size_t len, uint8_t out[32]);
void sha256_hex(const void *data, size_t len, char out[65]);
int sha256_file(const char *path, char out[65]);   /* a whole file, a piece at a time */

#endif
