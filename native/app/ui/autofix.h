/*
 * hakuX front end — stalled games (autofix.c).
 *
 * A catalog of fixes, each a switch the emulator already has (an option, an environment switch, a
 * debug property), the symptoms of a stalled game (every thread waiting, a thread spinning, the
 * GPU waited on, no frame at all), and the loop around them: a game that stalls for 25 s is run
 * again with the next fix that fits the symptom (one a run); a run with a fix that gets 900 frames
 * past where the last one stalled, and doesn't stall, keeps it: "fix=<id>" in the game's settings
 * file, applied from then on. What each fix did, for which symptom, is kept for every game
 * (compat_learned.ini), and orders the fixes tried for the next one (the same publisher's first).
 * The decisions are here (no window, no SDL), the effects are win_main.c's.
 */
#ifndef NATIVE_APP_UI_AUTOFIX_H
#define NATIVE_APP_UI_AUTOFIX_H

#include <stdbool.h>
#include <stdint.h>

#include "native/app/ui/config.h"

struct NcOptions;

typedef enum {
    AF_SYM_DEADLOCK,        /* every thread waits, none runs */
    AF_SYM_SPIN,            /* a thread runs (or could), and no frame comes */
    AF_SYM_GPU,             /* the render thread waits for the GPU, a flip or a trap */
    AF_SYM_NOFRAME,         /* no frame at all since the start */
    AF_SYM_N
} AfSym;

typedef struct AfFix {
    const char *id;         /* "kernel-locks": what the settings file keeps */
    const char *label;      /* for the notice and the log */
    const char *what;       /* a sentence on what it changes */
    unsigned syms;          /* the symptoms it is tried for: 1 << AfSym */
    unsigned needs_patch;   /* NC_FIXUP_* the run must have applied (a patch off that wasn't on is no fix), or 0 */
    const char *arg;        /* an option the core takes ("ram=128"), or NULL */
    const char *env_name, *env_val;
    const char *prop_name, *prop_val;
} AfFix;

const AfFix *af_find(const char *id);
unsigned af_count(void);
const AfFix *af_at(unsigned i);
const char *af_sym_name(AfSym s);

/* Applies a fix to the run being set up (before the core starts): its option goes to nc_option, its
 * switch to the environment or the debug properties. False for an unknown id. */
bool af_apply(const char *id, struct NcOptions *opt);

/* What the symptom is, from the threads (running and runnable, all), whether the render thread waits
 * on the GPU, and whether a frame has come at all. */
AfSym af_classify(unsigned running_or_ready, bool render_waits, bool no_frame);

/* The fixes to try for a symptom, the best first: those that fit it and the run (patches applied),
 * not in game's "fix=" already (applied), not in its "fixfail=" (tried, no use), not tried by this chain
 * (tried: ids of the trials so far, tried_n). learned: the file of what worked (NULL: none); publisher:
 * the title id's top 16 bits. Returns how many (at most max) of af_at's index are put in out. */
int af_candidates(AfSym sym, uint32_t publisher, const Config *game, const Config *learned, unsigned patches,
                  const char *const *tried, int tried_n, const AfFix **out, int max);

/* What a trial came to, in the game's file ("fix=" kept, "fixfail=" noted) and the learned one. */
void af_record_pass(Config *game, Config *learned, AfSym sym, uint32_t publisher, const char *id);
void af_record_fail(Config *game, Config *learned, AfSym sym, uint32_t publisher, const char *id);
/* The game's saved fixes: how many, and all of them dropped (with the failures noted). */
int af_saved_count(const Config *game);
/* ... and with the ones tried without use (what the in-game reset row counts). */
int af_noted_count(const Config *game);
void af_reset_game(Config *game);

/* ---- the loop: one per run, polled from the window's refresh ---- */

enum { AF_STALL_S = 25, AF_NOFRAME_S = 90, AF_PASS_FRAMES = 900, AF_PASS_MIN_S = 90, AF_MAX_TRIALS = 4 };

typedef enum {
    AF_NONE,
    AF_TRY,                 /* stalled: run again with .id (a failed trial's, if .failed) */
    AF_PASS,                /* the trial of .id is through: keep it */
    AF_GIVEUP               /* stalled, and nothing left to try */
} AfAct;

typedef struct AfPoll {
    AfAct act;
    AfSym sym;              /* AF_TRY, AF_GIVEUP: the symptom now; AF_PASS: the one the fix was tried for */
    const AfFix *fix;       /* AF_TRY: the next; AF_PASS: the one through */
    const AfFix *failed;    /* AF_TRY: the trial that stalled again, or NULL */
    uint64_t stall_frame;   /* AF_TRY: the frame this run stalled at */
} AfPoll;

typedef struct AfRun {
    bool enabled;
    /* the trial this run is (from fixtrial=<id>@<frame>#<n>:<tried ids>), or none */
    char trial_id[40];
    AfSym trial_sym;        /* the symptom it was tried for */
    uint64_t trial_frame;
    int trials;             /* the chain's so far, this one included */
    char tried[AF_MAX_TRIALS + 1][40];
    int tried_n;
    bool judged, gave_up, acted;
    double started_s;
} AfRun;

void af_run_init(AfRun *r, bool enabled, double now_s);
/* "fixtrial=" value: id@frame#n#sym:id,id (the earlier trials' ids) */
bool af_run_set_trial(AfRun *r, const char *value);
/* The value for the next run's fixtrial= after trying id at stall_frame. */
void af_run_trial_value(const AfRun *r, const char *id, uint64_t stall_frame, AfSym sym, char *out, unsigned n);

/* Whether af_poll could have something to say (the cheap check, for the window's loop to make before it
 * loads the settings files). */
bool af_due(const AfRun *r, double now_s, double hang_s, uint64_t frames);
/* The environment switches the fixes set, dropped (a run again must not carry a trial's on to the next). */
void af_clear_env(void);

/* hang_s: the live hang's age (0: none), frames: closed so far, threads/render_waits as af_classify's.
 * paused: the game is paused (the menu, the window away): nothing is judged. */
AfPoll af_poll(AfRun *r, double now_s, double hang_s, uint64_t frames, unsigned running_or_ready, bool render_waits,
               bool paused, uint32_t publisher, const Config *game, const Config *learned, unsigned patches);

#endif
