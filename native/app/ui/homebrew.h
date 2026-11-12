/*
 * xbased front end — homebrew (homebrew.c, homebrew_ui.c): the apps installed on the
 * hard disk (Partition1/Games, Apps, Emulators), started from a disc image made of
 * their folder, and XboxHomebrew.store (Team Resurgent) to browse and install from.
 */
#ifndef NATIVE_APP_UI_HOMEBREW_H
#define NATIVE_APP_UI_HOMEBREW_H

#include "native/app/ui/ui.h"

#include <stdbool.h>
#include <stdint.h>

/* ---- installed ---- */
typedef struct {
    char name[64];      /* the folder's name */
    char group[16];     /* Games, Apps, Emulators */
    char dir[1100];
    char store_id[48];  /* from the store's marker, "" for one put there by hand */
    UiTex *icon;        /* the store's cover, when it is known */
    bool icon_tried;
} HbApp;

/* The hard disk's E: folder (hdd/Partition1) */
void hb_e_dir(char *out, size_t n);
void hb_scan(void);                 /* the installed apps read again */
int hb_count(void);
HbApp *hb_get(int i);
/* The app's disc image made (when its folder changed) on a thread, then started as a
 * game file would be. */
void hb_launch(int i);
bool hb_busy(void);
int hb_progress(void);              /* 0-100 while the image is written */
const char *hb_status(void);
void hb_poll(void);                 /* each frame, from the UI's thread */
void hb_delete(int i);              /* the app's folder removed */
UiTex *hb_icon(HbApp *a);           /* its store cover, when it has one */

/* ---- the store ---- */
typedef struct {
    char name[48];
    int count;
} StCat;

typedef struct {
    char id[48], name[72], author[48], category[32], desc[640], latest[48];
    UiTex *cover;
    bool cover_tried;
} StApp;

typedef struct {
    char id[48], version[24], date[24], changelog[480], folder[64], region[16];
    uint64_t size;
    int nfiles;
} StVer;

void st_start(void);                /* the categories and the first page, once */
void st_retry(void);                /* asked again after a failure */
int st_ncat(void);
const StCat *st_cat(int i);
int st_category(void);              /* the one shown, -1 all */
void st_set_category(int i);
int st_napps(void);
StApp *st_app(int i);
bool st_loading(void);
bool st_has_more(void);
void st_load_more(void);
const char *st_error(void);         /* "" when well */
UiTex *st_cover(StApp *a);          /* the cover's texture once its file has arrived */

void st_open_app(StApp *a);         /* its versions fetched */
bool st_ver_loading(void);
int st_nver(void);
const StVer *st_ver(int i);
/* The latest version of the app installed (a thread). Install state: 0 idle, 1 working,
 * 2 done, 3 failed; the message names the step. */
void st_install(const StApp *a);
int st_install_state(void);
const char *st_install_msg(void);
/* The install's progress: pct 0-99 while the size is known, -1 when it isn't (a moving bar);
 * bytes so far, the store's total size (0 unknown), phase 1 downloading, 2 unpacking, 3 installing. */
void st_install_progress(int *pct, uint64_t *bytes, uint64_t *total, int *phase);
void st_install_ack(void);          /* done/failed seen: back to idle */

/* ---- screens ---- */
void scr_homebrew(void);
void scr_store(void);
void scr_store_app(void);

#endif
