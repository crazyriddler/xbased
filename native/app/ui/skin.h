/*
 * xbased front end — the skins (skin.c): the dashboard styles of the main
 * screen and their backdrops, drawn in the current theme (ui.h, ui_theme).
 * Orb is xbased's own; the others are the xemu Modern GUI's styles drawn
 * again on this interface.
 */
#ifndef NATIVE_APP_UI_SKIN_H
#define NATIVE_APP_UI_SKIN_H

#include <stdbool.h>

enum SkinStyle {
    SK_ORB, SK_TABS, SK_CLASSIC, SK_BLADES, SK_METRO, SK_ORIGINAL, SK_OBLADES, SK_GLOSSY, SK_MODERN, SK_HOLO,
    SK_AURORA, SK_COUNT
};

typedef struct {
    int id;                 /* what it opens (the caller's) */
    const char *label;
    const char *desc;
} SkinItem;

/* The style in use (the config's "ui.style", set each frame by the caller). */
void skin_set(const char *name);
int skin_id(void);
const char *skin_name(int id);       /* the config's value: "orb", "tabs", ... */
const char *skin_title(int id);      /* shown in the settings */
int skin_from_name(const char *name);

/* The screens' backdrop in the style (all screens, not only the main one);
 * k 0: still, 1: moving. */
void skin_backdrop(float k);

/* The main screen: the items (n at most 8) in the style, sel the selected
 * one. Returns the index chosen (A, Enter, a click), or -1. */
int skin_main(const SkinItem *items, int n, int *sel);

#endif
