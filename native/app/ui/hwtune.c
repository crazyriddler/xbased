/*
 * xbased — picture settings chosen for the PC it runs on. The first time the dashboard starts (and whenever
 * "hw.auto" is set, which is then cleared) the GPU and the display are looked at and the settings that depend on
 * them are written to xbased.ini: the first time only those not there yet, with hw.auto all of them.
 *
 * Automatic texture upscaling is turned on for a discrete GPU of 6 GB or more.
 *
 *   GPU                               render scale up to   anisotropic   anti-aliasing   pack textures up to
 *   discrete, 10 GB or more                  4x               16x           SMAA             2048
 *   discrete, 6 GB or more                   4x               16x           SMAA             1024
 *   discrete, 3 GB or more                   3x               16x           SMAA              512
 *   discrete under 3 GB, or integrated       2x                8x           FXAA              256
 *   none found (a software renderer)         1x               off           off               256
 *
 * The render scale is the smallest that reaches the display's height (480 lines a step: 3x for 1080p and
 * 1440p, 4x for 4K), up to the GPU's limit; where it doesn't reach, FSR scales the rest. A guess from memory
 * sizes, not a measurement: a game that runs slowly wants a lower scale.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <stdio.h>
#include <string.h>
#include "native/app/ui/config.h"
#include "native/app/ui/hwtune.h"

typedef struct Gpu {
    char name[256];
    int kind;                   /* 2 discrete, 1 integrated or virtual, 0 a CPU's or none */
    uint64_t vram;              /* its largest device-local heap */
} Gpu;

static bool probe(Gpu *out)
{
    memset(out, 0, sizeof(*out));
    if (!SDL_Vulkan_LoadLibrary(NULL)) {
        return false;
    }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    PFN_vkCreateInstance create = gipa ? (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance") : NULL;
    if (!create) {
        return false;
    }
    const VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "xbased",
                                   .apiVersion = VK_API_VERSION_1_0 };
    const VkInstanceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
    VkInstance inst = VK_NULL_HANDLE;
    if (create(&ci, NULL, &inst) != VK_SUCCESS || !inst) {
        return false;
    }
    PFN_vkEnumeratePhysicalDevices en = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties gp =
        (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceMemoryProperties gm =
        (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(inst, "vkGetPhysicalDeviceMemoryProperties");
    PFN_vkDestroyInstance destroy = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");
    VkPhysicalDevice dev[16];
    uint32_t n = 16;
    bool any = false;
    if (en && gp && gm && (en(inst, &n, dev) == VK_SUCCESS || n)) {
        for (uint32_t i = 0; i < n && i < 16; i++) {
            VkPhysicalDeviceProperties p;
            VkPhysicalDeviceMemoryProperties m;
            gp(dev[i], &p);
            gm(dev[i], &m);
            Gpu g = { .kind = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 :
                              p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ||
                              p.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU ? 1 : 0 };
            snprintf(g.name, sizeof(g.name), "%s", p.deviceName);
            for (uint32_t h = 0; h < m.memoryHeapCount; h++) {
                if ((m.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && m.memoryHeaps[h].size > g.vram) {
                    g.vram = m.memoryHeaps[h].size;
                }
            }
            /* the best of them: a discrete one first, then the most memory */
            if (!any || g.kind > out->kind || (g.kind == out->kind && g.vram > out->vram)) {
                *out = g;
                any = true;
            }
        }
    }
    if (destroy) {
        destroy(inst, NULL);
    }
    return any;
}

void hwtune_pick(int kind, uint64_t vram, int display_h, HwTune *t)
{
    const uint64_t gb = 1024ull * 1024 * 1024;
    const int tier = kind == 2 ? (vram >= 6 * gb ? 3 : vram >= 3 * gb ? 2 : 1) : kind == 1 ? 1 : 0;
    const int most = tier == 3 ? 4 : tier == 2 ? 3 : tier == 1 ? 2 : 1;
    int want = display_h > 0 ? (display_h + 479 - 48) / 480 : 2;     /* the first to reach it (48 lines short will do) */
    want = want < 1 ? 1 : want;
    t->scale = want > most ? most : want;
    t->aniso = tier >= 2 ? 16 : tier == 1 ? 8 : 0;
    t->smaa = tier >= 2;
    t->fxaa = tier == 1;
    t->fsr = tier >= 1 && display_h > 0 && t->scale * 480 < display_h - 48;
    t->texauto = tier == 3;
    t->texcap = kind == 2 && vram >= 10 * gb ? 2048 : kind == 2 && vram >= 6 * gb ? 1024 :
                kind == 2 && vram >= 3 * gb ? 512 : 256;
}

static bool put(Config *c, const char *key, const char *val, bool all)
{
    if (!all && cfg_get(c, key)) {
        return false;
    }
    cfg_set(c, key, val);
    return true;
}

bool hwtune_apply(Config *c, SDL_Window *win, char *note, size_t note_n)
{
    const bool all = cfg_bool(c, "hw.auto", false);
    note[0] = 0;
    if (!all && cfg_get(c, "hw.tuned")) {
        return false;
    }
    Gpu g;
    const bool found = probe(&g);
    const SDL_DisplayMode *m = win ? SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(win)) : NULL;
    const int dh = m ? (int)((float)m->h * (m->pixel_density > 0.0f ? m->pixel_density : 1.0f) + 0.5f) : 0;
    HwTune t;
    hwtune_pick(found ? g.kind : 0, found ? g.vram : 0, dh, &t);
    char v[16];
    int set = 0;
    snprintf(v, sizeof(v), "%d", t.scale);
    set += put(c, "scale", v, all);
    snprintf(v, sizeof(v), "%d", t.aniso);
    set += put(c, "aniso", v, all);
    /* the anti-aliasing: both keys together, and only when neither was chosen */
    if (all || (!cfg_get(c, "smaa") && !cfg_get(c, "fxaa"))) {
        cfg_set(c, "smaa", t.smaa ? "1" : "0");
        cfg_set(c, "fxaa", t.fxaa ? "1" : "0");
        set += 2;
    }
    if (t.fsr) {
        set += put(c, "upscale", "fsr", all);
    } else if (all) {
        set += put(c, "upscale", "bilinear", true);
    }
    snprintf(v, sizeof(v), "%d", t.texcap);
    set += put(c, "texcap", v, all);
    /* automatic texture upscaling: on where the GPU has the memory for it (and off elsewhere when asked again) */
    if (t.texauto) {
        set += put(c, "texauto", "1", all);
    } else if (all) {
        set += put(c, "texauto", "0", true);
    }
    cfg_set(c, "hw.tuned", "1");
    cfg_unset(c, "hw.auto");
    snprintf(note, note_n, "settings for this PC (%s, %.1f GB, display %d high): scale %dx, anisotropic %dx, %s, "
             "%s, pack textures up to %d; %d %s", found ? g.name : "no Vulkan GPU found",
             (double)(found ? g.vram : 0) / (1024.0 * 1024.0 * 1024.0), dh, t.scale, t.aniso,
             t.smaa ? "SMAA" : t.fxaa ? "FXAA" : "no anti-aliasing", t.fsr ? "FSR" : "no upscaler needed", t.texcap,
             set, all ? "set" : "set (those not chosen before)");
    return true;
}
