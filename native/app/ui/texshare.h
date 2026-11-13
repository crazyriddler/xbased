/* xbased — HD texture packs shared between PCs (texshare.c). */
#ifndef NATIVE_APP_UI_TEXSHARE_H
#define NATIVE_APP_UI_TEXSHARE_H

#include <stddef.h>
#include <stdint.h>

/* A game's upscaled and hand-made textures (the data folder's hle/textures/<title id>/ and its auto/) into
 * "<data>texture packs/exported/<title> [<title id>] HD textures.zip" (more parts when it passes 1.9 GB). Returns
 * how many pictures went in (0: the game has none; -1: the file couldn't be written); out: the first zip's path. */
int texshare_export(const char *data_dir, uint32_t title_id, const char *title, char *out, size_t out_n);
/* Every zip in "<data>texture packs/" installed: its pictures (entries named <title id>/<hash>.png or
 * <title id>/auto/<hash>.png, anything else left out) into hle/textures/, the zip then moved to installed/.
 * Returns how many pictures were installed; note says what was done (empty: no zips). */
int texshare_import_all(const char *data_dir, char *note, size_t note_n);

#endif
