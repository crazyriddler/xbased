/*
 * xbased native core — CCI disc images (cci_image.c): an XISO compressed a
 * sector at a time with LZ4 (Repackinator, XboxToolkit, Cerbios' loader),
 * read as the plain image it holds. A split image (Name.1.cci, Name.2.cci,
 * ... each part a CCI of its own, made for FATX's 4 GiB file limit) is read
 * from its first part as the one image they make in turn.
 */
#ifndef NATIVE_CORE_CCI_IMAGE_H
#define NATIVE_CORE_CCI_IMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* An open file descriptor's image as a CCI, if it is one: registered by the
 * descriptor, so the disc's reads (nc_cci_read on it) go through it; a
 * split image's first part's later parts opened beside it. False: not a
 * CCI (or one whose header can't be read, or a split one's part missing). */
bool nc_cci_attach(int fd);
void nc_cci_detach(int fd);
/* fd registered as a CCI */
bool nc_cci_is(int fd);
/* len bytes of the image it holds at off; zeros past its end. False: a
 * block that can't be read or decoded. Any thread, several at once. */
bool nc_cci_read(int fd, void *buf, size_t len, uint64_t off);
/* The bytes the image's files take on disk, all its parts' (0: not a CCI) */
uint64_t nc_cci_file_size(int fd);

/* A file a split CCI's later part (Name.2.cci, Name.3.cci, ... beside a
 * Name.1.cci that is a split image's first part): not a game of its own,
 * the first part is. path: the file's, its folder's with it. */
bool nc_cci_later_part(const char *path);
/* A split image's part path (Name.N.cci): its part k's in out. False: not
 * such a name, or no room. */
bool nc_cci_part_path(const char *path, unsigned k, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
