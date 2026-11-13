/*
 * xbased — automatic texture upscaling: after a game, the textures it wrote out (hle/textures/<title id>/dump/,
 * hw/xbox/hle/pb/texpack.h) and that have no picture yet in the pack or in auto/ are handed to Real-ESRGAN
 * (realesrgan-ncnn-vulkan, the portable program, not included), which writes them four times the size into
 * auto/. It runs on its own after xbased has gone on; the game uses what is there the next time it starts.
 *
 * The program is looked for as $XBASED_REALESRGAN, then realesrgan/realesrgan-ncnn-vulkan[.exe] beside xbased,
 * then in the data folder. Each run gets a folder of its own for its input (dump/queue-<time>), so two runs
 * never take each other's files; those of runs a day old are removed.
 */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "native/app/app.h"

#ifdef _WIN32
#define TOOL "realesrgan-ncnn-vulkan.exe"
#else
#define TOOL "realesrgan-ncnn-vulkan"
#endif

static bool there(const char *path)
{
    SDL_PathInfo pi;
    return SDL_GetPathInfo(path, &pi) && pi.type != SDL_PATHTYPE_NONE;
}

/* The program's path into tool (and its folder's length into *dirlen); false without one. */
static bool find_tool(const char *exe_dir, const char *data_dir, char *tool, size_t n, size_t *dirlen)
{
    const char *env = SDL_getenv("XBASED_REALESRGAN");
    if (env && *env && there(env)) {
        SDL_snprintf(tool, n, "%s", env);
    } else {
        SDL_snprintf(tool, n, "%srealesrgan/" TOOL, exe_dir);
        if (!there(tool)) {
            SDL_snprintf(tool, n, "%srealesrgan/" TOOL, data_dir);
            if (!there(tool)) {
                return false;
            }
        }
    }
    size_t l = SDL_strlen(tool);
    while (l && tool[l - 1] != '/' && tool[l - 1] != '\\') {
        l--;
    }
    *dirlen = l;
    return true;
}

/* The upscaler started last, while it runs: one at a time (it takes the whole GPU), and a run is asked for
 * every few seconds during a game (win_main.c) as well as at its end. */
static SDL_Process *g_job;
static SDL_AtomicInt g_in;

static int run_one(const char *dir, const char *exe_dir, const char *data_dir, char *note, size_t note_n);

int nc_texauto_run(const char *dir, const char *exe_dir, const char *data_dir, char *note, size_t note_n)
{
    note[0] = 0;
    if (!SDL_CompareAndSwapAtomicInt(&g_in, 0, 1)) {
        return 0;               /* another thread is at it */
    }
    int n = 0;
    if (g_job) {
        int code = 0;
        if (SDL_WaitProcess(g_job, false, &code)) {
            SDL_DestroyProcess(g_job);
            g_job = NULL;
        }
    }
    if (!g_job) {
        n = run_one(dir, exe_dir, data_dir, note, note_n);
    }
    SDL_SetAtomicInt(&g_in, 0);
    return n;
}

static int run_one(const char *dir, const char *exe_dir, const char *data_dir, char *note, size_t note_n)
{
    char tool[1200], dump[1200], aut[1200], queue[1300], a[1400], b[1400], models[1300];
    size_t tdir = 0;
    note[0] = 0;
    if (!dir || !*dir) {
        return 0;
    }
    if (!find_tool(exe_dir, data_dir, tool, sizeof(tool), &tdir)) {
        SDL_snprintf(note, note_n, "no %s (looked in %srealesrgan/ and %srealesrgan/)", TOOL, exe_dir, data_dir);
        return -1;
    }
    SDL_snprintf(dump, sizeof(dump), "%sdump", dir);
    SDL_snprintf(aut, sizeof(aut), "%sauto", dir);
    SDL_CreateDirectory(aut);
    /* earlier runs' input folders, a day old: gone */
    int nq = 0;
    char **old = SDL_GlobDirectory(dump, "queue-*", 0, &nq);
    const SDL_Time now = (SDL_Time)time(NULL) * 1000000000ll;
    for (int i = 0; old && i < nq; i++) {
        SDL_PathInfo pi;
        SDL_snprintf(a, sizeof(a), "%s/%s", dump, old[i]);
        if (SDL_GetPathInfo(a, &pi) && pi.type == SDL_PATHTYPE_DIRECTORY && now - pi.modify_time > 86400ll * 1000000000ll) {
            int nf = 0;
            char **f = SDL_GlobDirectory(a, "*", 0, &nf);
            for (int k = 0; f && k < nf; k++) {
                SDL_snprintf(b, sizeof(b), "%s/%s", a, f[k]);
                SDL_RemovePath(b);
            }
            SDL_free(f);
            SDL_RemovePath(a);
        }
    }
    SDL_free(old);
    /* this run's input: the dumped textures with no picture of the same name in the pack or in auto/ */
    int nf = 0, todo = 0;
    char **f = SDL_GlobDirectory(dump, "*.png", 0, &nf);
    SDL_snprintf(queue, sizeof(queue), "%s/queue-%lld", dump, (long long)time(NULL));
    for (int i = 0; f && i < nf; i++) {
        SDL_snprintf(a, sizeof(a), "%s/%s", aut, f[i]);
        SDL_snprintf(b, sizeof(b), "%s%s", dir, f[i]);
        if (there(a) || there(b)) {
            continue;
        }
        /* refused twice (auto/<name>.bad, hle_texpack_reject): not again */
        const size_t stem = SDL_strlen(f[i]) > 4 ? SDL_strlen(f[i]) - 4 : 0;
        SDL_snprintf(a, sizeof(a), "%s/%.*s.bad", aut, (int)stem, f[i]);
        if (there(a)) {
            continue;
        }
        if (!todo && !SDL_CreateDirectory(queue)) {
            break;
        }
        SDL_snprintf(a, sizeof(a), "%s/%s", dump, f[i]);
        SDL_snprintf(b, sizeof(b), "%s/%s", queue, f[i]);
        if (SDL_CopyFile(a, b)) {
            todo++;
        }
    }
    SDL_free(f);
    if (!todo) {
        return 0;
    }
    SDL_snprintf(models, sizeof(models), "%.*smodels", (int)tdir, tool);
    const char *args[] = { tool, "-i", queue, "-o", aut, "-n", "realesrgan-x4plus", "-s", "4", "-f", "png",
                           "-m", models, NULL };
    SDL_PropertiesID p = SDL_CreateProperties();
    SDL_SetPointerProperty(p, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)args);
    /* nothing of its output wanted (it writes its progress); not a "background" process, whose end can't be
     * asked for: this one's is (g_job), and it runs on all the same when xbased has gone */
#ifdef _WIN32
    /* Windows: a console program started from a windowed one gets a console window of its own unless it is
     * started as a "background" process (SDL: CREATE_NO_WINDOW), which there can still be asked for its end.
     * (1.0.76.30 started it without: a black window came up over the game at every batch.) */
    SDL_SetBooleanProperty(p, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
#endif
    SDL_SetNumberProperty(p, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(p, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(p, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_Process *pr = SDL_CreateProcessWithProperties(p);
    SDL_DestroyProperties(p);
    if (!pr) {
        SDL_snprintf(note, note_n, "%s did not start: %s", tool, SDL_GetError());
        return -1;
    }
    g_job = pr;                         /* it runs on; its end is asked for at the next call */
    SDL_snprintf(note, note_n, "%d textures to %s (into %s)", todo, tool, aut);
    return todo;
}
