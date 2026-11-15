/*
 * hakuX front end — a game's desktop shortcut (shortcut.h).
 *
 * The command it runs is the one front ends and Steam's non-Steam games
 * take too: xbased "<disc image>" boots the game without the dashboard.
 */
#include "native/app/ui/shortcut.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the name as a file name: no characters Windows or the desktop refuse */
static void file_name(const char *name, char *out, size_t n)
{
    size_t o = 0;
    for (const char *s = name; *s && o + 1 < n; s++) {
        const char c = *s;
        out[o++] = strchr("<>:\"/\\|?*", c) || (unsigned char)c < 32 ? ' ' : c;
    }
    while (o && (out[o - 1] == ' ' || out[o - 1] == '.')) {
        o--;
    }
    out[o] = 0;
    if (!o) {
        snprintf(out, n, "xbased game");
    }
}

#ifdef _WIN32
#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>

/* UTF-8 to UTF-16, with Windows' own path separators */
static void wide(const char *s, wchar_t *out, int n)
{
    if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, out, n)) {
        out[0] = 0;
    }
    for (wchar_t *c = out; *c; c++) {
        *c = *c == L'/' ? L'\\' : *c;
    }
}

bool sc_make(const char *name, const char *game, const char *icon_png, const char *icon_ico, char *where,
             size_t n)
{
    (void)icon_png;
    wchar_t exe[MAX_PATH], dir[MAX_PATH], args[2100], gw[2048], icon[MAX_PATH], lnk[MAX_PATH + 300];
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) {
        return false;
    }
    wcscpy(dir, exe);
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash) {
        *slash = 0;
    }
    wide(game, gw, 2048);
    _snwprintf(args, 2100, L"\"%ls\"", gw);
    args[2099] = 0;
    PWSTR desk = NULL;
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_Desktop, 0, NULL, &desk))) {
        return false;
    }
    char fname[300];
    file_name(name, fname, sizeof(fname));
    wchar_t wname[300];
    wide(fname, wname, 300);
    _snwprintf(lnk, MAX_PATH + 300, L"%ls\\%ls.lnk", desk, wname);
    lnk[MAX_PATH + 299] = 0;
    CoTaskMemFree(desk);
    const HRESULT init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    IShellLinkW *sl = NULL;
    bool ok = SUCCEEDED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&sl));
    if (ok) {
        IShellLinkW_SetPath(sl, exe);
        IShellLinkW_SetArguments(sl, args);
        IShellLinkW_SetWorkingDirectory(sl, dir);
        wchar_t desc[300];
        wide(name, desc, 300);
        IShellLinkW_SetDescription(sl, desc);
        if (icon_ico) {
            wide(icon_ico, icon, MAX_PATH);
            IShellLinkW_SetIconLocation(sl, icon, 0);
        } else {
            IShellLinkW_SetIconLocation(sl, exe, 0);
        }
        IPersistFile *pf = NULL;
        ok = SUCCEEDED(IShellLinkW_QueryInterface(sl, &IID_IPersistFile, (void **)&pf));
        if (ok) {
            ok = SUCCEEDED(IPersistFile_Save(pf, lnk, TRUE));
            IPersistFile_Release(pf);
        }
        IShellLinkW_Release(sl);
    }
    if (SUCCEEDED(init)) {
        CoUninitialize();
    }
    if (ok && where) {
        WideCharToMultiByte(CP_UTF8, 0, lnk, -1, where, (int)n, NULL, NULL);
    }
    return ok;
}

#elif defined(__APPLE__)
#include <sys/stat.h>
#include <unistd.h>

static void sh_quote(const char *s, char *out, size_t n)
{
    size_t o = 0;
    out[o++] = '\'';
    for (; *s && o + 6 < n; s++) {
        if (*s == '\'') {
            memcpy(out + o, "'\\''", 4);
            o += 4;
        } else {
            out[o++] = *s;
        }
    }
    out[o++] = '\'';
    out[o] = 0;
}

static bool write_text(const char *path, const char *text, mode_t mode)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        return false;
    }
    const bool ok = fputs(text, f) >= 0;
    fclose(f);
    chmod(path, mode);
    return ok;
}

bool sc_make(const char *name, const char *game, const char *icon_png, const char *icon_ico, char *where,
             size_t n)
{
    (void)icon_ico;
    char exe[1024];
    if (!nc_exe_path(exe, sizeof(exe))) {
        return false;
    }
    const char *home = getenv("HOME");
    if (!home) {
        return false;
    }
    char fname[300], app[1400], dir[1500], path[1600];
    file_name(name, fname, sizeof(fname));
    snprintf(app, sizeof(app), "%s/Desktop/%s.app", home, fname);
    snprintf(dir, sizeof(dir), "%s/Contents", app);
    mkdir(app, 0755);
    mkdir(dir, 0755);
    snprintf(dir, sizeof(dir), "%s/Contents/MacOS", app);
    mkdir(dir, 0755);
    snprintf(dir, sizeof(dir), "%s/Contents/Resources", app);
    mkdir(dir, 0755);
    char qe[2200], qg[4400], script[7000];
    sh_quote(exe, qe, sizeof(qe));
    sh_quote(game, qg, sizeof(qg));
    snprintf(script, sizeof(script), "#!/bin/sh\nexec %s %s\n", qe, qg);
    snprintf(path, sizeof(path), "%s/Contents/MacOS/launch", app);
    bool ok = write_text(path, script, 0755);
    bool icon = false;
    if (icon_png && *icon_png) {
        char qi[2200], qo[2200], cmd[4600];
        snprintf(path, sizeof(path), "%s/Contents/Resources/icon.icns", app);
        sh_quote(icon_png, qi, sizeof(qi));
        sh_quote(path, qo, sizeof(qo));
        snprintf(cmd, sizeof(cmd), "sips -s format icns %s --out %s >/dev/null 2>&1", qi, qo);
        icon = system(cmd) == 0;
    }
    char plist[3000], id[200];
    size_t k = 0;
    for (const char *c = fname; *c && k + 1 < sizeof(id); c++) {
        const char ch = *c;
        id[k++] = ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) ? ch : '-';
    }
    id[k] = 0;
    char xname[700];
    size_t o = 0;
    for (const char *c = name; *c && o + 7 < sizeof(xname); c++) {
        const char *e = *c == '&' ? "&amp;" : *c == '<' ? "&lt;" : *c == '>' ? "&gt;" : NULL;
        if (e) {
            o += (size_t)snprintf(xname + o, sizeof(xname) - o, "%s", e);
        } else {
            xname[o++] = *c;
        }
    }
    xname[o] = 0;
    snprintf(plist, sizeof(plist),
             "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
             "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
             "<plist version=\"1.0\"><dict>\n"
             "<key>CFBundleExecutable</key><string>launch</string>\n"
             "<key>CFBundleIdentifier</key><string>io.github.crazyriddler.xbased.game.%s</string>\n"
             "<key>CFBundleName</key><string>%s</string>\n"
             "<key>CFBundlePackageType</key><string>APPL</string>\n"
             "%s"
             "<key>LSApplicationCategoryType</key><string>public.app-category.games</string>\n"
             "</dict></plist>\n",
             id, xname, icon ? "<key>CFBundleIconFile</key><string>icon</string>\n" : "");
    snprintf(path, sizeof(path), "%s/Contents/Info.plist", app);
    ok = ok && write_text(path, plist, 0644);
    if (ok && where) {
        snprintf(where, n, "%s", app);
    }
    return ok;
}

#else
#include <sys/stat.h>
#include <unistd.h>

/* a desktop entry's Exec argument, quoted: ", `, $ and \ escaped */
static void exec_arg(const char *s, char *out, size_t n)
{
    size_t o = 0;
    out[o++] = '"';
    for (; *s && o + 4 < n; s++) {
        if (strchr("\"`$\\", *s)) {
            out[o++] = '\\';
        }
        out[o++] = *s;
    }
    out[o++] = '"';
    out[o] = 0;
}

static bool write_entry(const char *path, const char *name, const char *exe, const char *game, const char *icon)
{
    char qe[1200], qg[2200];
    exec_arg(exe, qe, sizeof(qe));
    exec_arg(game, qg, sizeof(qg));
    FILE *f = fopen(path, "w");
    if (!f) {
        return false;
    }
    fprintf(f, "[Desktop Entry]\nType=Application\nName=%s\nComment=Play it in xbased\nExec=%s %s\nIcon=%s\n"
               "Terminal=false\nCategories=Game;Emulator;\n",
            name, qe, qg, icon && *icon ? icon : "xbased");
    fclose(f);
    chmod(path, 0755);
    return true;
}

bool sc_make(const char *name, const char *game, const char *icon_png, const char *icon_ico, char *where,
             size_t n)
{
    (void)icon_ico;
    char exe[1024];
    const char *ai = getenv("APPIMAGE");     /* the AppImage, not its mounted copy */
    if (ai && *ai) {
        snprintf(exe, sizeof(exe), "%s", ai);
    } else {
        const ssize_t k = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (k <= 0) {
            return false;
        }
        exe[k] = 0;
    }
    const char *home = getenv("HOME");
    if (!home) {
        return false;
    }
    char desk[1024] = "";
    FILE *p = popen("xdg-user-dir DESKTOP 2>/dev/null", "r");
    if (p) {
        if (fgets(desk, sizeof(desk), p)) {
            desk[strcspn(desk, "\r\n")] = 0;
        }
        pclose(p);
    }
    if (!desk[0]) {
        snprintf(desk, sizeof(desk), "%s/Desktop", home);
    }
    char fname[300], path[1400], apps[1400];
    file_name(name, fname, sizeof(fname));
    snprintf(path, sizeof(path), "%s/%s.desktop", desk, fname);
    mkdir(desk, 0755);
    const bool ok = write_entry(path, name, exe, game, icon_png);
    /* and in the applications menu */
    snprintf(apps, sizeof(apps), "%s/.local/share/applications", home);
    mkdir(apps, 0755);
    char made[1400];
    snprintf(made, sizeof(made), "%s/xbased-%s.desktop", apps, fname);
    for (char *c = made + strlen(apps) + 1; *c; c++) {
        *c = *c == ' ' ? '-' : *c;
    }
    write_entry(made, name, exe, game, icon_png);
    if (ok && where) {
        snprintf(where, n, "%s", path);
    }
    return ok;
}
#endif
