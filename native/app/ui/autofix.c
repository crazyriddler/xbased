/*
 * hakuX front end — stalled games (autofix.h).
 */
#include "native/app/ui/autofix.h"

#include "native/core/options.h"
#include "native/kernel/kernel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/system_properties.h>

#define SYM(s) (1u << (s))
#define ANY (SYM(AF_SYM_DEADLOCK) | SYM(AF_SYM_SPIN) | SYM(AF_SYM_GPU) | SYM(AF_SYM_NOFRAME))

/* The order is the first guess's: what the kernel does with threads and locks, then the drives, then
 * the patches this emulator makes to a title's library code (each only when the run applied it), then
 * the GPU's, then the memory. Every one is a switch that was there (a debug A/B, an option); none is
 * new behaviour. */
static const AfFix k_fixes[] = {
    { "kernel-locks", "locks in the kernel",
      "The kernel runs the title's locks itself instead of the short paths patched into its code.",
      SYM(AF_SYM_DEADLOCK) | SYM(AF_SYM_SPIN), 0, NULL, "NC_NO_FAST", "1", NULL, NULL },
    { "drive-timing", "the drives' own timing",
      "Disc and hard disk reads take as long as the console's drives took (a title may wait on a read that came too fast).",
      SYM(AF_SYM_DEADLOCK) | SYM(AF_SYM_SPIN) | SYM(AF_SYM_NOFRAME), 0, "devtime=2", NULL, NULL, NULL, NULL },
    { "reads-in-place", "reads in place",
      "A thread's file reads finish where they are asked, not on a helper thread.",
      SYM(AF_SYM_DEADLOCK) | SYM(AF_SYM_NOFRAME), 0, NULL, "NC_SYNC_IN_PLACE", "1", NULL, NULL },
    { "short-slices", "short thread slices",
      "Threads of one priority take turns every 2 ms instead of every 20 ms.",
      SYM(AF_SYM_DEADLOCK) | SYM(AF_SYM_SPIN), 0, NULL, "NC_QUANTUM_US", "2000", NULL, NULL },
    { "no-audio-patch", "no audio wait patch",
      "DirectSound's waits on the audio chip are left as the title has them.",
      SYM(AF_SYM_DEADLOCK) | SYM(AF_SYM_SPIN), NC_FIXUP_APU, NULL, NULL, NULL, "debug.hakux.apuspin", "0" },
    { "no-movie-patch", "no movie wait patch",
      "The movie player's wait for its audio streams is left as the title has it.",
      SYM(AF_SYM_DEADLOCK) | SYM(AF_SYM_SPIN) | SYM(AF_SYM_NOFRAME), NC_FIXUP_XMV, NULL, NULL, NULL,
      "debug.hakux.xmvwait", "0" },
    { "no-timer-patch", "no D3D timer patch",
      "Direct3D's timer callback is left as the title has it.",
      SYM(AF_SYM_SPIN) | SYM(AF_SYM_GPU) | SYM(AF_SYM_NOFRAME), NC_FIXUP_D3D_TIMER, NULL, NULL, NULL,
      "debug.hakux.d3dtimer", "0" },
    { "gpu-unbatched", "GPU commands one by one",
      "Draw commands are not merged before they go to the GPU.",
      SYM(AF_SYM_GPU) | SYM(AF_SYM_NOFRAME), 0, NULL, NULL, NULL, "debug.hakux.batch", "0" },
    { "debug-kit-memory", "128 MiB of memory",
      "The console has a debug kit's 128 MiB (a title marked for 64 gets 64 still).",
      SYM(AF_SYM_DEADLOCK) | SYM(AF_SYM_NOFRAME), 0, "ram=128", NULL, NULL, NULL, NULL },
};

#define N_FIXES (sizeof(k_fixes) / sizeof(k_fixes[0]))

const AfFix *af_find(const char *id)
{
    for (unsigned i = 0; id && i < N_FIXES; i++) {
        if (!strcmp(k_fixes[i].id, id)) {
            return &k_fixes[i];
        }
    }
    return NULL;
}

unsigned af_count(void)
{
    return (unsigned)N_FIXES;
}

const AfFix *af_at(unsigned i)
{
    return i < N_FIXES ? &k_fixes[i] : NULL;
}

const char *af_sym_name(AfSym s)
{
    static const char *const n[AF_SYM_N] = { "deadlock", "spin", "gpu", "noframe" };
    return s >= 0 && s < AF_SYM_N ? n[s] : "?";
}

bool af_apply(const char *id, NcOptions *opt)
{
    const AfFix *f = af_find(id);
    if (!f) {
        return false;
    }
    if (f->arg && opt) {
        nc_option(opt, f->arg);
    }
    if (f->env_name) {
        setenv(f->env_name, f->env_val, 1);
    }
    if (f->prop_name) {
        nc_win_prop_set(f->prop_name, f->prop_val);
    }
    return true;
}

AfSym af_classify(unsigned running_or_ready, bool render_waits, bool no_frame)
{
    if (no_frame) {
        return AF_SYM_NOFRAME;
    }
    if (render_waits) {
        return AF_SYM_GPU;
    }
    return running_or_ready ? AF_SYM_SPIN : AF_SYM_DEADLOCK;
}

/* ---- the files ---- */

static bool listed(const Config *c, const char *key, const char *id)
{
    const char *v[64];
    const int n = c ? cfg_all(c, key, v, 64) : 0;
    for (int i = 0; i < n; i++) {
        if (!strcmp(v[i], id)) {
            return true;
        }
    }
    return false;
}

/* learned: "ok.<sym>.<id>" and "fail.<sym>.<id>" (counts, over every game), and "pub.<hex>.<id>" (a fix
 * that worked for a title of this publisher) */
static int learned_int(const Config *l, const char *fmt, const char *a, const char *b)
{
    char k[48];
    snprintf(k, sizeof(k), fmt, a, b);
    return l ? cfg_int(l, k, 0) : 0;
}

static int score(const AfFix *f, AfSym sym, uint32_t publisher, const Config *learned, int index)
{
    char pub[12];
    snprintf(pub, sizeof(pub), "%04x", (unsigned)(publisher & 0xFFFF));
    const char *s = af_sym_name(sym);
    int v = index * 10;
    v -= learned_int(learned, "ok.%s.%s", s, f->id) * 25;
    v += learned_int(learned, "fail.%s.%s", s, f->id) * 15;
    if (publisher && learned_int(learned, "pub.%s.%s", pub, f->id) > 0) {
        v -= 60;                    /* worked for a title of the same publisher: first */
    }
    return v;
}

int af_candidates(AfSym sym, uint32_t publisher, const Config *game, const Config *learned, unsigned patches,
                  const char *const *tried, int tried_n, const AfFix **out, int max)
{
    int idx[N_FIXES], sc[N_FIXES], n = 0;
    for (unsigned i = 0; i < N_FIXES; i++) {
        const AfFix *f = &k_fixes[i];
        bool skip = !(f->syms & SYM(sym)) || (f->needs_patch && !(patches & f->needs_patch)) ||
                    listed(game, "fix", f->id) || listed(game, "fixfail", f->id);
        for (int t = 0; t < tried_n && !skip; t++) {
            skip = tried[t] && !strcmp(tried[t], f->id);
        }
        if (!skip) {
            idx[n] = (int)i;
            sc[n] = score(f, sym, publisher, learned, (int)i);
            n++;
        }
    }
    for (int i = 1; i < n; i++) {   /* insertion sort, stable */
        const int ki = idx[i], ks = sc[i];
        int j = i - 1;
        while (j >= 0 && sc[j] > ks) {
            idx[j + 1] = idx[j];
            sc[j + 1] = sc[j];
            j--;
        }
        idx[j + 1] = ki;
        sc[j + 1] = ks;
    }
    for (int i = 0; i < n && i < max; i++) {
        out[i] = &k_fixes[idx[i]];
    }
    return n < max ? n : max;
}

static void bump(Config *l, const char *fmt, const char *a, const char *b)
{
    if (!l) {
        return;
    }
    char k[48];
    snprintf(k, sizeof(k), fmt, a, b);
    cfg_set_int(l, k, cfg_int(l, k, 0) + 1);
}

void af_record_pass(Config *game, Config *learned, AfSym sym, uint32_t publisher, const char *id)
{
    if (game && !listed(game, "fix", id)) {
        cfg_add(game, "fix", id);
    }
    if (game) {
        cfg_remove_value(game, "fixfail", id);
    }
    char pub[12];
    snprintf(pub, sizeof(pub), "%04x", (unsigned)(publisher & 0xFFFF));
    bump(learned, "ok.%s.%s", af_sym_name(sym), id);
    if (publisher) {
        bump(learned, "pub.%s.%s", pub, id);
    }
}

void af_record_fail(Config *game, Config *learned, AfSym sym, uint32_t publisher, const char *id)
{
    (void)publisher;
    if (listed(game, "fix", id)) {
        return;                     /* kept already: a run that carried it on is not a failure of it */
    }
    if (game && !listed(game, "fixfail", id)) {
        cfg_add(game, "fixfail", id);
    }
    bump(learned, "fail.%s.%s", af_sym_name(sym), id);
}

int af_saved_count(const Config *game)
{
    const char *v[64];
    return game ? cfg_all(game, "fix", v, 64) : 0;
}

int af_noted_count(const Config *game)
{
    const char *v[64];
    return game ? cfg_all(game, "fix", v, 64) + cfg_all(game, "fixfail", v, 64) : 0;
}

void af_reset_game(Config *game)
{
    const char *v[64];
    int n = cfg_all(game, "fix", v, 64);
    char ids[64][40];
    for (int i = 0; i < n; i++) {
        snprintf(ids[i], sizeof(ids[i]), "%s", v[i]);
    }
    for (int i = 0; i < n; i++) {
        cfg_remove_value(game, "fix", ids[i]);
    }
    n = cfg_all(game, "fixfail", v, 64);
    for (int i = 0; i < n; i++) {
        snprintf(ids[i], sizeof(ids[i]), "%s", v[i]);
    }
    for (int i = 0; i < n; i++) {
        cfg_remove_value(game, "fixfail", ids[i]);
    }
}

/* ---- the loop ---- */

void af_run_init(AfRun *r, bool enabled, double now_s)
{
    memset(r, 0, sizeof(*r));
    r->enabled = enabled;
    r->started_s = now_s;
}

bool af_run_set_trial(AfRun *r, const char *value)
{
    /* id@frame#n:id,id */
    char id[40];
    unsigned long long frame = 0;
    int trials = 0, sym = 0, used = 0;
    if (sscanf(value, "%39[^@]@%llu#%d#%d%n", id, &frame, &trials, &sym, &used) < 4 || !af_find(id) || trials < 1 ||
        sym < 0 || sym >= AF_SYM_N) {
        return false;
    }
    r->trial_sym = (AfSym)sym;
    snprintf(r->trial_id, sizeof(r->trial_id), "%s", id);
    r->trial_frame = frame;
    r->trials = trials;
    r->tried_n = 0;
    const char *p = value + used;
    while (*p == ':' || *p == ',') {
        p++;
        char t[40];
        int k = 0;
        while (*p && *p != ',' && k < 39) {
            t[k++] = *p++;
        }
        t[k] = 0;
        if (k && r->tried_n < AF_MAX_TRIALS) {
            snprintf(r->tried[r->tried_n++], sizeof(r->tried[0]), "%s", t);
        }
    }
    return true;
}

void af_run_trial_value(const AfRun *r, const char *id, uint64_t stall_frame, AfSym sym, char *out, unsigned n)
{
    /* the bar to pass is the furthest any run of the chain got */
    if (r->trial_id[0] && r->trial_frame > stall_frame) {
        stall_frame = r->trial_frame;
    }
    int o = snprintf(out, n, "%s@%llu#%d#%d", id, (unsigned long long)stall_frame, r->trials + 1, (int)sym);
    for (int i = 0; i < r->tried_n && o > 0 && (unsigned)o < n; i++) {
        o += snprintf(out + o, n - (unsigned)o, "%c%s", i ? ',' : ':', r->tried[i]);
    }
    if (r->trial_id[0] && o > 0 && (unsigned)o < n) {
        snprintf(out + o, n - (unsigned)o, "%c%s", r->tried_n ? ',' : ':', r->trial_id);
    }
}

bool af_due(const AfRun *r, double now_s, double hang_s, uint64_t frames)
{
    if (!r->enabled) {
        return false;
    }
    const double run_s = now_s - r->started_s;
    if (hang_s >= AF_STALL_S || (frames == 0 && run_s >= AF_NOFRAME_S)) {
        return !r->acted;
    }
    return r->trial_id[0] && !r->judged && run_s >= AF_PASS_MIN_S && frames >= r->trial_frame + AF_PASS_FRAMES;
}

void af_clear_env(void)
{
    for (unsigned i = 0; i < N_FIXES; i++) {
        if (k_fixes[i].env_name) {
            unsetenv(k_fixes[i].env_name);
        }
    }
}

AfPoll af_poll(AfRun *r, double now_s, double hang_s, uint64_t frames, unsigned running_or_ready, bool render_waits,
               bool paused, uint32_t publisher, const Config *game, const Config *learned, unsigned patches)
{
    AfPoll p = { .act = AF_NONE };
    if (!r->enabled || paused) {
        return p;
    }
    const double run_s = now_s - r->started_s;
    const bool no_frame = frames == 0 && run_s >= AF_NOFRAME_S;
    const bool stalled = hang_s >= AF_STALL_S || no_frame;
    const bool trial = r->trial_id[0] != 0;
    if (trial && !r->judged && !stalled && run_s >= AF_PASS_MIN_S && frames >= r->trial_frame + AF_PASS_FRAMES) {
        r->judged = true;
        p.act = AF_PASS;
        p.sym = r->trial_sym;
        p.fix = af_find(r->trial_id);
        /* that stall is over: the next one is a new chain, with its own tries (the fixes kept stay) */
        r->trial_id[0] = 0;
        r->trial_frame = 0;
        r->trials = 0;
        r->tried_n = 0;
        r->judged = false;
        r->acted = false;
        return p;
    }
    if (!stalled || r->acted) {
        return p;
    }
    r->acted = true;
    p.sym = af_classify(running_or_ready, render_waits, no_frame);
    p.stall_frame = frames;
    if (trial && !r->judged) {
        r->judged = true;
        p.failed = af_find(r->trial_id);
    }
    /* what this chain has tried: its earlier trials and this run's */
    const char *tried[AF_MAX_TRIALS + 2];
    int nt = 0;
    for (int i = 0; i < r->tried_n; i++) {
        tried[nt++] = r->tried[i];
    }
    if (trial) {
        tried[nt++] = r->trial_id;
    }
    const AfFix *c[1];
    if (r->trials >= AF_MAX_TRIALS || af_candidates(p.sym, publisher, game, learned, patches, tried, nt, c, 1) < 1) {
        r->gave_up = true;
        p.act = AF_GIVEUP;
        return p;
    }
    p.act = AF_TRY;
    p.fix = c[0];
    return p;
}
