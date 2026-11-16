/*
 * xbased front end — a folder laid out as an Xbox disc image (xiso_build.c).
 *
 * Homebrew lives on the hard disk as folders (Partition1/Apps/<name>/default.xbe);
 * the emulator boots disc images, so the folder is written out as an XDVDFS image
 * (the format of Xbox game discs) and that is started from the emulated drive.
 */
#ifndef NATIVE_APP_UI_XISO_BUILD_H
#define NATIVE_APP_UI_XISO_BUILD_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct XisoTree XisoTree;

/* The folder's tree read (names, sizes, times: no file data). NULL with err when it
 * can't be stored (a name over 255 bytes, a file over 4 GB, two names that differ
 * only in case, nothing in it). */
XisoTree *xiso_tree_open(const char *dir, char *err, size_t errn);
/* A number that changes when anything in the tree does (its image is stale then). */
uint64_t xiso_tree_signature(const XisoTree *t);
uint64_t xiso_tree_data_bytes(const XisoTree *t);
/* The image written to path (the file data copied in; *pct 0-100 as it goes, may be
 * NULL). The path is removed again on failure. */
bool xiso_tree_write(XisoTree *t, const char *path, SDL_AtomicInt *pct, char *err, size_t errn);
void xiso_tree_free(XisoTree *t);

#endif
