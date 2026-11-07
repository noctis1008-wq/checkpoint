/*
 * c_github_downloader.c — download files from GitHub releases (Checkpoint script)
 *
 * The "c_" prefix is only there for the script list, which Checkpoint sorts by
 * name: it puts this script right after "browser", second in the list.
 *
 * Fetches a release asset straight onto the SD card, without a PC:
 *   - a few presets (the Dropbox sync script, GodMode9_RefFix, this script's
 *     own update), each with its usual destination,
 *   - any public repository: type "owner/repo", pick a file of its latest
 *     release, pick where it goes (the destination is suggested from the file
 *     type: .firm -> /luma/payloads, .c -> Checkpoint's scripts, .3dsx -> /3ds,
 *     .cia -> /cia).
 *
 * A download is written next to its destination as "<name>.part" and only
 * renamed into place once it is complete, so a dropped connection never leaves a
 * half-written .firm behind. Downloads pass through memory: very large files
 * (hundreds of MB) can fail with "too big" on a 3DS.
 *
 * Only public repositories, and only the latest release (GitHub's
 * /releases/latest: drafts and pre-releases are skipped). HTTPS certificates
 * are not verified by Checkpoint's HTTP layer.
 */

#include <checkpoint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

#define GH_API "https://api.github.com/repos/"
#define GH_WEB "https://github.com/"

#define PATHN   512
#define NAMEN   160
#define URLN    768
#define REPON   64   /* gui_keyboard on 3DS: 63 characters + NUL */
#define MAXASSET 64

char g_root[PATHN];  /* app_root(): "/3ds/Checkpoint" or "sdmc:/switch/Checkpoint" */
char g_sd[PATHN];    /* prefix for SD-absolute paths: "" on 3DS, "sdmc:" on Switch */
int g_is3ds;
int g_failed; /* a download was attempted and failed */

void init_paths(void)
{
    char* root = app_root();
    strncpy(g_root, root, PATHN - 1);
    g_root[PATHN - 1] = '\0';
    free(root);
    if (strstr(g_root, "/switch") != NULL) {
        strcpy(g_sd, "sdmc:");
        g_is3ds = 0;
    }
    else {
        g_sd[0] = '\0';
        g_is3ds = 1;
    }
    script_log("github downloader");
}

/* ---- helpers ------------------------------------------------------------ */

void jget(struct JSON* o, char* key, char* dest, int size)
{
    dest[0] = '\0';
    if (!json_is_object(o) || !json_object_contains(o, key)) {
        return;
    }
    struct JSON* e = json_object_element(o, key);
    if (!json_is_string(e)) {
        return;
    }
    char* s = json_get_string(e);
    strncpy(dest, s, size - 1);
    dest[size - 1] = '\0';
    free(s);
}

int jint(struct JSON* o, char* key)
{
    if (!json_is_object(o) || !json_object_contains(o, key)) {
        return 0;
    }
    struct JSON* e = json_object_element(o, key);
    return json_is_int(e) ? json_get_int(e) : 0;
}

/* Does `name` end in `ext` (case-insensitive, ext includes the dot)? */
int ends_with(char* name, char* ext)
{
    int n = strlen(name);
    int e = strlen(ext);
    if (n <= e) {
        return 0;
    }
    int i = 0;
    while (i < e) {
        if (tolower(name[n - e + i]) != tolower(ext[i])) {
            return 0;
        }
        i = i + 1;
    }
    return 1;
}

/* The folder part of `path` (everything before the last '/') into dst. */
void parent_of(char* path, char* dst, int size)
{
    int last = -1;
    int i    = 0;
    while (path[i] != '\0') {
        if (path[i] == '/') {
            last = i;
        }
        i = i + 1;
    }
    int n = last > 0 ? last : 0;
    if (n > size - 1) {
        n = size - 1;
    }
    strncpy(dst, path, n);
    dst[n] = '\0';
}

/* "owner/repo": two non-empty parts of [A-Za-z0-9._-]. */
int valid_repo(char* s)
{
    int slash = 0;
    int i     = 0;
    int part  = 0;
    while (s[i] != '\0') {
        char c = s[i];
        if (c == '/') {
            if (part == 0 || slash) {
                return 0;
            }
            slash = 1;
            part  = 0;
        }
        else if (isalnum(c) || c == '-' || c == '_' || c == '.') {
            part = part + 1;
        }
        else {
            return 0;
        }
        i = i + 1;
    }
    return slash && part > 0;
}

void size_text(int bytes, char* dst)
{
    if (bytes >= 1048576) {
        sprintf(dst, "%d.%d MB", bytes / 1048576, (bytes % 1048576) * 10 / 1048576);
    }
    else if (bytes >= 1024) {
        sprintf(dst, "%d KB", bytes / 1024);
    }
    else {
        sprintf(dst, "%d B", bytes);
    }
}

/* ---- download ------------------------------------------------------------ */

void explain(int st, char* dst)
{
    if (st == 404) {
        strcpy(dst, "Not found (404). Check the repository\nname, and that it has a release.");
    }
    else if (st == 403) {
        strcpy(dst, "GitHub refused the request (403).\nIf you made many requests, wait an\nhour (API rate limit) and try again.");
    }
    else if (st == -2) {
        strcpy(dst, "The file is too big to download on\nthis console.");
    }
    else if (st < 0) {
        sprintf(dst, "GitHub could not be reached\n(transfer error %d). Check Wi-Fi.", st);
    }
    else {
        sprintf(dst, "GitHub answered HTTP %d.", st);
    }
}

/* Download `url` to the SD file `dest` via "<dest>.part". 1 ok. */
int download_to(char* url, char* dest)
{
    char line[URLN + 32];
    sprintf(line, "GET %s", url);
    script_log(line);

    char dir[PATHN];
    parent_of(dest, dir, PATHN);
    if (dir[0] != '\0') {
        sd_mkdirs(dir);
    }

    gui_status("Downloading...");
    progress_note("downloading");
    char* out = NULL;
    int n     = 0;
    int st    = web_get(&out, &n, url);
    if (st != 200 || out == NULL || n <= 0) {
        if (out != NULL) {
            free(out);
        }
        char msg[200];
        explain(st, msg);
        sprintf(line, "  download failed (%d)", st);
        script_log(line);
        gui_message(msg);
        return 0;
    }

    char part[PATHN + 8];
    sprintf(part, "%s.part", dest);
    FILE* f = fopen(part, "wb");
    if (f == NULL) {
        free(out);
        gui_message("Could not write to the SD card.");
        return 0;
    }
    int wrote = fwrite(out, 1, n, f);
    fclose(f);
    free(out);
    if (wrote != n) {
        remove(part);
        gui_message("The file could not be saved.\nThe SD card may be full.");
        return 0;
    }
    /* FAT cannot rename over an existing file */
    if (sd_exists(dest)) {
        remove(dest);
    }
    if (rename(part, dest) != 0) {
        remove(part);
        gui_message("The downloaded file could not be\nmoved into place.");
        return 0;
    }
    char sz[32];
    size_text(n, sz);
    sprintf(line, "  saved %s (%s)", dest, sz);
    script_log(line);
    return 1;
}

/* ---- presets ------------------------------------------------------------- */

#define NPRESET 3

/* Preset `i`: repo, asset name, destination (relative: "@" = Checkpoint root,
 * "/" = SD root), menu label, 3DS-only. Returned from a switch rather than kept
 * in global arrays of char*: picoc's cleanup of such globals is not safe. */
char* p_repo(int i)
{
    if (i == 0) {
        return "noctis1008-wq/godmode9-with-cartridge-fixer";
    }
    return "noctis1008-wq/checkpoint";
}

char* p_asset(int i)
{
    if (i == 0) {
        return "GodMode9_RefFix.firm";
    }
    if (i == 1) {
        return "dropbox.c";
    }
    return "c_github_downloader.c";
}

char* p_dest(int i)
{
    if (i == 0) {
        return "/luma/payloads/GodMode9_RefFix.firm";
    }
    if (i == 1) {
        return "@/scripts/universal/dropbox.c";
    }
    return "@/scripts/universal/c_github_downloader.c";
}

char* p_label(int i)
{
    if (i == 0) {
        return "GodMode9_RefFix (cartridge fixer)";
    }
    if (i == 1) {
        return "Dropbox sync script (dropbox.c)";
    }
    return "Update this downloader";
}

int p_3dsonly(int i)
{
    return i == 0;
}

/* Earlier versions were called github_downloader.c: once this one is in
 * place under its new name, the old copy would only be a duplicate entry. */
void drop_old_name(void)
{
    char old_path[PATHN];
    char new_path[PATHN];
    sprintf(old_path, "%s/scripts/universal/github_downloader.c", g_root);
    sprintf(new_path, "%s/scripts/universal/c_github_downloader.c", g_root);
    FILE* f = fopen(new_path, "rb");
    if (f == NULL) {
        return;
    }
    fclose(f);
    remove(old_path);
}

/* Expand "@/x" to <root>/x and "/x" to <sd prefix>/x. */
void expand_dest(char* rel, char* dst)
{
    if (rel[0] == '@') {
        char tail[PATHN];
        int i = 1;
        while (rel[i] != '\0' && i < PATHN) {
            tail[i - 1] = rel[i];
            i           = i + 1;
        }
        tail[i - 1] = '\0';
        sprintf(dst, "%s%s", g_root, tail);
    }
    else {
        sprintf(dst, "%s%s", g_sd, rel);
    }
}

int run_preset(int i)
{
    char url[URLN];
    sprintf(url, "%s%s/releases/latest/download/%s", GH_WEB, p_repo(i), p_asset(i));
    char dest[PATHN];
    expand_dest(p_dest(i), dest);
    char msg[PATHN + 200];
    sprintf(msg, "Download the latest\n%s\nfrom github.com/%s\n\nto %s?", p_asset(i), p_repo(i), dest);
    if (!gui_confirm(msg)) {
        return 0;
    }
    if (!download_to(url, dest)) {
        g_failed = 1;
        return 0;
    }
    if (i == NPRESET - 1) {
        drop_old_name(); /* self-update: retire the old file name */
    }
    if (i == 0) {
        gui_message("Done. Hold START while powering on to\nopen it from the Luma3DS chainloader.");
    }
    else {
        gui_message("Done. New or updated scripts appear in\nthe Scripts list the next time\nCheckpoint starts.");
    }
    return 1;
}

/* ---- any repository ------------------------------------------------------ */

/* Suggested destination folder for a file name, SD-absolute. */
void suggest_dir(char* name, char* dst)
{
    if (ends_with(name, ".firm")) {
        sprintf(dst, "%s/luma/payloads", g_sd);
    }
    else if (ends_with(name, ".c")) {
        sprintf(dst, "%s/scripts/universal", g_root);
    }
    else if (ends_with(name, ".3dsx")) {
        sprintf(dst, "%s/3ds", g_sd);
    }
    else if (ends_with(name, ".cia")) {
        sprintf(dst, "%s/cia", g_sd);
    }
    else if (ends_with(name, ".nro")) {
        sprintf(dst, "%s/switch", g_sd);
    }
    else {
        sprintf(dst, "%s/", g_sd);
    }
}

int run_custom(void)
{
    char repo[REPON];
    gui_keyboard(repo, "GitHub repository (owner/repo)", REPON);
    if (repo[0] == '\0') {
        return 0;
    }
    if (!valid_repo(repo)) {
        gui_message("Type it as owner/repo, for example\nBernardoGiordano/Checkpoint");
        return 0;
    }

    char url[URLN];
    sprintf(url, "%s%s/releases/latest", GH_API, repo);
    gui_status("Reading the latest release...");
    char* out = NULL;
    int n     = 0;
    int st    = web_request("GET", url, "Accept: application/vnd.github+json", "", 0, &out, &n, NULL);
    if (st != 200 || out == NULL) {
        if (out != NULL) {
            free(out);
        }
        char msg[200];
        explain(st, msg);
        gui_message(msg);
        g_failed = 1;
        return 0;
    }
    struct JSON* j = json_new();
    json_parse(j, out);
    free(out);
    if (!json_is_valid(j) || !json_object_contains(j, "assets")) {
        json_delete(j);
        gui_message("GitHub's answer could not be read.");
        return 0;
    }
    char tag[NAMEN];
    jget(j, "tag_name", tag, NAMEN);

    struct JSON* assets = json_object_element(j, "assets");
    int count           = json_array_size(assets);
    if (count > MAXASSET) {
        count = MAXASSET;
    }
    if (count == 0) {
        json_delete(j);
        gui_message("The latest release has no files.");
        return 0;
    }
    char* labels[MAXASSET];
    char* names[MAXASSET];
    char* urls[MAXASSET];
    int i;
    for (i = 0; i < count; i++) {
        struct JSON* a = json_array_element(assets, i);
        char nm[NAMEN];
        char u[URLN];
        char sz[32];
        jget(a, "name", nm, NAMEN);
        jget(a, "browser_download_url", u, URLN);
        size_text(jint(a, "size"), sz);
        char lab[NAMEN + 40];
        sprintf(lab, "%s (%s)", nm, sz);
        labels[i] = strdup(lab);
        names[i]  = strdup(nm);
        urls[i]   = strdup(u);
    }
    json_delete(j);

    char prompt[NAMEN + 80];
    sprintf(prompt, "%s %s", repo, tag);
    int pick = gui_pick_one(prompt, labels, count);
    int ok   = 0;
    if (pick >= 0) {
        /* where to: the suggestion first, then the usual places */
        char sug[PATHN];
        suggest_dir(names[pick], sug);
        char d1[PATHN];
        char d2[PATHN];
        char d3[PATHN];
        char d4[PATHN];
        char d5[PATHN];
        sprintf(d1, "%s/scripts/universal", g_root);
        sprintf(d2, "%s/luma/payloads", g_sd);
        sprintf(d3, "%s/3ds", g_sd);
        sprintf(d4, "%s/cia", g_sd);
        sprintf(d5, "%s/", g_sd);
        char* dirs[6];
        int nd   = 0;
        dirs[nd] = sug;
        nd       = nd + 1;
        if (strcmp(sug, d1) != 0) {
            dirs[nd] = d1;
            nd       = nd + 1;
        }
        if (g_is3ds && strcmp(sug, d2) != 0) {
            dirs[nd] = d2;
            nd       = nd + 1;
        }
        if (g_is3ds && strcmp(sug, d3) != 0) {
            dirs[nd] = d3;
            nd       = nd + 1;
        }
        if (g_is3ds && strcmp(sug, d4) != 0) {
            dirs[nd] = d4;
            nd       = nd + 1;
        }
        if (strcmp(sug, d5) != 0) {
            dirs[nd] = d5;
            nd       = nd + 1;
        }
        int dp = gui_pick_one("Save to", dirs, nd);
        if (dp >= 0) {
            char dest[PATHN];
            int dl = strlen(dirs[dp]);
            if (dl > 0 && dirs[dp][dl - 1] == '/') {
                sprintf(dest, "%s%s", dirs[dp], names[pick]);
            }
            else {
                sprintf(dest, "%s/%s", dirs[dp], names[pick]);
            }
            int go = 1;
            if (sd_exists(dest)) {
                char msg[PATHN + 64];
                sprintf(msg, "%s\nalready exists. Replace it?", dest);
                go = gui_confirm(msg);
            }
            if (go) {
                if (download_to(urls[pick], dest)) {
                    char msg[PATHN + 64];
                    sprintf(msg, "Saved to\n%s", dest);
                    gui_message(msg);
                    ok = 1;
                }
                else {
                    g_failed = 1;
                }
            }
        }
    }
    for (i = 0; i < count; i++) {
        free(labels[i]);
        free(names[i]);
        free(urls[i]);
    }
    return ok;
}

/* ---- main ------------------------------------------------------------- */

int main(int argc, char** argv)
{
    init_paths();
    g_failed = 0;
    drop_old_name();

    char* opts[NPRESET + 1];
    int map[NPRESET + 1];
    int n = 0;
    int i;
    for (i = 0; i < NPRESET; i++) {
        if (p_3dsonly(i) && !g_is3ds) {
            continue;
        }
        opts[n] = p_label(i);
        map[n]  = i;
        n       = n + 1;
    }
    opts[n] = "Other repository...";
    map[n]  = -1;
    n       = n + 1;

    int c = gui_pick_one("Download from GitHub", opts, n);
    if (c < 0) {
        return 0;
    }
    if (map[c] >= 0) {
        run_preset(map[c]);
    }
    else {
        run_custom();
    }
    return g_failed ? 1 : 0;
}
