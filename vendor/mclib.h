/* ps1_memcard_lib: the memory card library (one folder a card, one .mcd a version).
 * Copyright (C) 2026 f3bandit. PolyForm Noncommercial 1.0.0 (see LICENSE.md). */
#ifndef PS1MC_MCLIB_H
#define PS1MC_MCLIB_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void mclib_set_dir(const char *dir);                           /* the library's folder (default /media/fat/memcards) */
const char *mclib_dir(void);
int mclib_safe_name(const char *in, char *out, size_t n);       /* a usable card name; -1 if nothing's left */
/* the library card this image most likely is (0..1; 0.5 or more: the same card), its name in card */
double mclib_recognise(const unsigned char *img, char *card, size_t n);
int mclib_exists(const char *card);
/* a new version of card (made if needed); rel = "card/version.mcd"; *same: it matched the latest one */
int mclib_store(const char *card, const unsigned char *img, char *rel, size_t rn, int *same);
int mclib_valid(const char *rel);                               /* card/version.mcd or an old loose .mcd */
char *mclib_list(void);                                         /* rel\tbytes\tmtime lines, newest first */
void mclib_suggest(char *out, size_t n);                        /* "Memory card N", not used yet */

int mclib_load(const char *card, const char *ver, unsigned char *buf, char *got, size_t gn);   /* ver "" newest */
char **mclib_versions(const char *card, int *n);               /* oldest first; caller frees each and the list */
int mclib_real(const char *card);                              /* read from a real card (not made here) */
void mclib_mark_made(const char *card);                        /* a card made here (blank, or copies) */
/* deleting (after the front end has asked "Delete ...?"): confirm is the name again, so a stray call can't delete.
 * A card keeps at least one version: its last one goes with the card. 0 deleted, -1 not (*err says why) */
int mclib_delete_card(const char *card, const char *confirm, const char **err);
int mclib_delete_version(const char *card, const char *ver, const char *confirm, const char **err);

#ifdef __cplusplus
}
#endif

#endif
