/*
 * Host test of the Autofix decisions (native/app/ui/autofix.c): the fix catalog, symptom classes, the order
 * of tries (learned results, the publisher's first), the trial value, the poll loop's passes, failures and
 * give-up, and the settings files. No window, no emulator: nc_option and nc_win_prop_set are stubbed.
 *
 *   clang -std=gnu11 -I. -Inative/compat/linux/include -I<SDL3 include dir> tests/autofix/autofix_test.c \
 *         native/app/ui/autofix.c native/app/ui/config.c <libSDL3.a> -lm -lpthread -ldl -lX11 -o autofix_test
 *   ./autofix_test      (prints "all ok")
 */
#include "native/app/ui/autofix.h"
#include "native/core/options.h"
#include "native/kernel/kernel.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_prop[64][2][64]; static int g_nprop;
void nc_win_prop_set(const char *n, const char *v) { snprintf(g_prop[g_nprop][0], 64, "%s", n); snprintf(g_prop[g_nprop][1], 64, "%s", v); g_nprop++; }
static char g_opt[8][64]; static int g_nopt;
bool nc_option(NcOptions *o, const char *a) { (void)o; snprintf(g_opt[g_nopt++], 64, "%s", a); return true; }

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
int main(void)
{
    int fails = 0;
    CHECK(af_count() == 9);
    for (unsigned i = 0; i < af_count(); i++) CHECK(af_find(af_at(i)->id) == af_at(i));
    CHECK(!af_find("nope"));

    /* apply */
    unsetenv("NC_NO_FAST");
    CHECK(af_apply("kernel-locks", NULL) && getenv("NC_NO_FAST"));
    af_clear_env(); CHECK(!getenv("NC_NO_FAST"));
    CHECK(af_apply("drive-timing", (NcOptions *)1) && !strcmp(g_opt[0], "devtime=2"));
    CHECK(af_apply("no-audio-patch", NULL) && !strcmp(g_prop[0][0], "debug.hakux.apuspin") && !strcmp(g_prop[0][1], "0"));
    CHECK(!af_apply("zzz", NULL));

    /* classify */
    CHECK(af_classify(0, false, true) == AF_SYM_NOFRAME);
    CHECK(af_classify(3, true, false) == AF_SYM_GPU);
    CHECK(af_classify(2, false, false) == AF_SYM_SPIN);
    CHECK(af_classify(0, false, false) == AF_SYM_DEADLOCK);

    /* candidates: deadlock, no patches applied */
    Config *game = calloc(1, sizeof(Config)), *learned = calloc(1, sizeof(Config));
    snprintf(game->path, sizeof(game->path), "/tmp/afhost/game.ini");
    snprintf(learned->path, sizeof(learned->path), "/tmp/afhost/learned.ini");
    remove(game->path); remove(learned->path);
    cfg_load(game, game->path); cfg_load(learned, learned->path);
    const AfFix *c[16];
    int n = af_candidates(AF_SYM_DEADLOCK, 0x4B4D, game, learned, 0, NULL, 0, c, 16);
    CHECK(n >= 3 && !strcmp(c[0]->id, "kernel-locks"));
    for (int i = 0; i < n; i++) CHECK(c[i]->needs_patch == 0);                    /* no patches applied: none that needs one */
    n = af_candidates(AF_SYM_DEADLOCK, 0x4B4D, game, learned, NC_FIXUP_APU | NC_FIXUP_XMV, NULL, 0, c, 16);
    int has_audio = 0; for (int i = 0; i < n; i++) has_audio |= !strcmp(c[i]->id, "no-audio-patch");
    CHECK(has_audio);
    /* GPU symptom: only the GPU-capable ones */
    n = af_candidates(AF_SYM_GPU, 0, game, learned, NC_FIXUP_D3D_TIMER, NULL, 0, c, 16);
    for (int i = 0; i < n; i++) CHECK(c[i]->syms & (1u << AF_SYM_GPU));
    CHECK(n >= 2);

    /* learning reorders: the publisher's success first */
    af_record_pass(game, learned, AF_SYM_DEADLOCK, 0x4B4D, "short-slices");
    n = af_candidates(AF_SYM_DEADLOCK, 0x4B4D, game, learned, 0, NULL, 0, c, 16);
    CHECK(n >= 1 && strcmp(c[0]->id, "short-slices") != 0);   /* it is in the game's fix= now: skipped */
    Config *g2 = calloc(1, sizeof(Config));
    snprintf(g2->path, sizeof(g2->path), "/tmp/afhost/game2.ini"); remove(g2->path); cfg_load(g2, g2->path);
    n = af_candidates(AF_SYM_DEADLOCK, 0x4B4D, g2, learned, 0, NULL, 0, c, 16);
    CHECK(n >= 1 && !strcmp(c[0]->id, "short-slices"));        /* same publisher: first */
    n = af_candidates(AF_SYM_DEADLOCK, 0x1234, g2, learned, 0, NULL, 0, c, 16);
    CHECK(n >= 1 && !strcmp(c[0]->id, "kernel-locks") || !strcmp(c[0]->id, "short-slices")); /* other publisher: the ok count helps only a little */
    printf("other publisher first: %s\n", c[0]->id);

    /* persistence */
    CHECK(af_saved_count(game) == 1);
    CHECK(cfg_save(game) && cfg_save(learned));
    Config *r = calloc(1, sizeof(Config)); cfg_load(r, game->path);
    CHECK(af_saved_count(r) == 1);
    af_record_fail(r, learned, AF_SYM_SPIN, 0x4B4D, "kernel-locks");
    n = af_candidates(AF_SYM_SPIN, 0, r, learned, 0, NULL, 0, c, 16);
    for (int i = 0; i < n; i++) CHECK(strcmp(c[i]->id, "kernel-locks") && strcmp(c[i]->id, "short-slices"));
    af_record_fail(r, learned, AF_SYM_SPIN, 0, "short-slices");       /* already kept: not a failure */
    { const char *v[8]; CHECK(cfg_all(r, "fixfail", v, 8) == 1); }
    CHECK(af_noted_count(r) == 2);                                    /* the kept one and the failed one */
    af_reset_game(r);
    CHECK(af_noted_count(r) == 0);
    CHECK(af_saved_count(r) == 0);
    { const char *v[8]; CHECK(cfg_all(r, "fixfail", v, 8) == 0); }

    /* trial value round trip */
    AfRun run; af_run_init(&run, true, 0);
    char v[200];
    af_run_trial_value(&run, "kernel-locks", 120, AF_SYM_SPIN, v, sizeof(v));
    printf("value %s\n", v);
    AfRun r2; af_run_init(&r2, true, 0);
    CHECK(af_run_set_trial(&r2, v) && !strcmp(r2.trial_id, "kernel-locks") && r2.trial_frame == 120 && r2.trials == 1 && r2.trial_sym == AF_SYM_SPIN);
    af_run_trial_value(&r2, "drive-timing", 40, AF_SYM_DEADLOCK, v, sizeof(v));
    printf("value %s\n", v);
    AfRun r3; af_run_init(&r3, true, 0);
    CHECK(af_run_set_trial(&r3, v) && !strcmp(r3.trial_id, "drive-timing") && r3.trial_frame == 120 && r3.tried_n == 1 && !strcmp(r3.tried[0], "kernel-locks"));
    CHECK(!af_run_set_trial(&r3, "bogus") && !af_run_set_trial(&r3, "kernel-locks@1#0#0"));

    /* the loop: a stall with no trial -> TRY the best; paused -> none */
    AfRun a; af_run_init(&a, true, 100.0);
    AfPoll p = af_poll(&a, 130, 0, 500, 0, false, false, 0x4B4D, g2, learned, 0);
    CHECK(p.act == AF_NONE);
    p = af_poll(&a, 130, 30, 500, 0, false, true, 0x4B4D, g2, learned, 0);
    CHECK(p.act == AF_NONE);                                          /* paused */
    CHECK(af_due(&a, 130, 30, 500) && !af_due(&a, 130, 3, 500));
    p = af_poll(&a, 130, 30, 500, 0, false, false, 0x4B4D, g2, learned, 0);
    CHECK(p.act == AF_TRY && p.sym == AF_SYM_DEADLOCK && p.stall_frame == 500 && !p.failed);
    printf("first try: %s\n", p.fix->id);
    CHECK(af_poll(&a, 131, 31, 500, 0, false, false, 0x4B4D, g2, learned, 0).act == AF_NONE);   /* once a run */
    CHECK(!af_due(&a, 131, 31, 500));

    /* a trial that stalls again: failed noted, next fix; a trial that passes */
    AfRun t; af_run_init(&t, true, 0);
    af_run_set_trial(&t, "kernel-locks@500#1#0");
    p = af_poll(&t, 200, 0, 1500, 1, false, false, 0, g2, learned, 0);
    CHECK(p.act == AF_PASS && p.fix && !strcmp(p.fix->id, "kernel-locks") && p.sym == AF_SYM_DEADLOCK);
    AfRun t2; af_run_init(&t2, true, 0);
    af_run_set_trial(&t2, "kernel-locks@500#1#0");
    p = af_poll(&t2, 60, 0, 1500, 1, false, false, 0, g2, learned, 0);
    CHECK(p.act == AF_NONE);                                          /* under 90 s: not yet */
    p = af_poll(&t2, 60, 30, 20, 1, false, false, 0, g2, learned, 0);
    CHECK(p.act == AF_TRY && p.failed && !strcmp(p.failed->id, "kernel-locks") && strcmp(p.fix->id, "kernel-locks"));
    /* a pass ends the chain: the next stall gets a whole new one (the kept fixes stay out of it) */
    AfRun pc; af_run_init(&pc, true, 0);
    af_run_set_trial(&pc, "kernel-locks@500#3#0:drive-timing,reads-in-place");
    p = af_poll(&pc, 200, 0, 1500, 1, false, false, 0, g2, learned, 0);
    CHECK(p.act == AF_PASS && pc.trials == 0 && pc.tried_n == 0 && !pc.trial_id[0]);
    p = af_poll(&pc, 400, 30, 9000, 1, false, false, 0, g2, learned, 0);
    CHECK(p.act == AF_TRY && !p.failed);
    char pv[200]; af_run_trial_value(&pc, p.fix->id, p.stall_frame, p.sym, pv, sizeof(pv));
    CHECK(strstr(pv, "#1#") != NULL);                                 /* its first try */
    /* a chain gives up after AF_MAX_TRIALS, and when nothing fits */
    AfRun t3; af_run_init(&t3, true, 0);
    af_run_set_trial(&t3, "kernel-locks@500#4#0:drive-timing,reads-in-place,short-slices");
    p = af_poll(&t3, 60, 30, 20, 1, false, false, 0, g2, learned, 0);
    CHECK(p.act == AF_GIVEUP && p.failed);
    AfRun t4; af_run_init(&t4, false, 0);
    CHECK(af_poll(&t4, 999, 99, 0, 0, false, false, 0, g2, learned, 0).act == AF_NONE);           /* disabled */
    /* no frame at all for 90 s */
    AfRun t5; af_run_init(&t5, true, 0);
    p = af_poll(&t5, 95, 0, 0, 0, false, false, 0, g2, learned, 0);
    CHECK(p.act == AF_TRY && p.sym == AF_SYM_NOFRAME);

    /* every fix is tried at most once in an exhaustive chain of one symptom */
    AfRun ch; af_run_init(&ch, true, 0);
    char seen[16][40]; int ns = 0;
    for (int step = 0; step < 12; step++) {
        p = af_poll(&ch, 1000, 30, 10, 0, false, false, 0, g2, NULL, 0);
        if (p.act != AF_TRY) break;
        for (int i = 0; i < ns; i++) CHECK(strcmp(seen[i], p.fix->id));
        snprintf(seen[ns++], 40, "%s", p.fix->id);
        char val[200]; af_run_trial_value(&ch, p.fix->id, p.stall_frame, p.sym, val, sizeof(val));
        af_run_init(&ch, true, 0); CHECK(af_run_set_trial(&ch, val));
    }
    printf("chain length %d (max %d)\n", ns, AF_MAX_TRIALS);
    CHECK(ns == AF_MAX_TRIALS - 0 || ns == AF_MAX_TRIALS - 1);

    printf(fails ? "%d FAILED\n" : "all ok\n", fails);
    return fails != 0;
}
