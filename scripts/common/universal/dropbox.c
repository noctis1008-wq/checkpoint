/*
 * dropbox.c — Dropbox save backup and sync (Checkpoint script)
 *
 * End-user setup and usage: scripts/dropbox.md, next to the scripting manual in
 * scripts/README.md.
 *
 * Backs up game saves ("extracts" them into Checkpoint's own backup folders),
 * uploads Checkpoint save backups to the user's own Dropbox, and downloads the
 * ones that are only in Dropbox back onto the console — a two-way sync of the
 * BACKUP folders. A game's live save is never written by this script: a backup
 * that came down from Dropbox shows up in Checkpoint's list, and the user
 * restores it there with the normal confirmation. That keeps a sync mistake
 * from ever costing a save.
 *
 * The menu:
 *   - Sync all            — for every title: upload the local backups Dropbox
 *                           does not have, download the Dropbox backups this
 *                           console does not have (saves and, on 3DS, extdata),
 *   - Back up all + sync  — first take a fresh backup of every title's save,
 *                           then Sync all,
 *   - This title          — the same two steps for the highlighted title only,
 *   - Settings            — account check, passphrase, sign out (revokes).
 *
 * Dropbox layout (inside the app's own folder, Apps/<your app name>/):
 *   /<3ds|switch>/saves/<TITLEID> <title name>/<backup name>.zip
 *   /<3ds|switch>/extdata/<TITLEID> <title name>/<backup name>.zip
 * Titles are matched by the 16-digit id in front, so a renamed title or a
 * console in another language still finds its own folder.
 *
 * Sign-in. Dropbox has no device flow, so this uses the authorization-code
 * flow without a redirect: the authorize page shows a code, the user types it
 * on the console. The app's key and secret identify the user's OWN Dropbox app
 * (created once at dropbox.com/developers, "App folder" access), so the
 * authorize URL is fixed and can be opened from a bookmark or a link. The
 * long-lived refresh token, the key and the secret go into config/dropbox.vault,
 * sealed with device_seal; nothing is left in the clear on the card.
 *
 * On-SD layout (paths relative to app_root()):
 *   config/dropbox.vault      app key, app secret and refresh token, sealed
 *   config/dropbox.json       optional setup file you drop in yourself:
 *                             {"app_key": "...", "app_secret": "..."}
 *                             read once, folded into the vault, then DELETED
 *   config/dropbox_login.txt  the authorize URL, written at sign-in so it can be
 *                             copied to a phone or PC; holds no secret
 *
 * What the seal is worth, and what it is not: see the comment at the top of
 * webdav.c and the "How the credentials are stored" section of googledrive.md —
 * the same device_seal vault, the same honest limits. The app has "App folder"
 * access only, so even a leaked token reaches nothing but this app's folder,
 * and Settings -> Sign out revokes it at Dropbox.
 */

#include <checkpoint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <ctype.h>

#define API_TOKEN    "https://api.dropboxapi.com/oauth2/token"
#define API_RPC      "https://api.dropboxapi.com/2/"
#define API_UPLOAD   "https://content.dropboxapi.com/2/files/upload"
#define API_DOWNLOAD "https://content.dropboxapi.com/2/files/download"
#define AUTHORIZE    "https://www.dropbox.com/oauth2/authorize"

/* buffer sizes (macros, so picoc's array bounds stay literal) */
#define KEYN     64   /* app key / app secret                                   */
#define CODEN    65   /* authorization code typed on the keyboard (63 + NUL)    */
#define REFRESHN 256  /* refresh token                                           */
#define ACCESSN  2048 /* short-lived access token ("sl." tokens are long)        */
#define AUTHN    2100 /* "Authorization: Bearer " + access token                 */
#define NAMEN    160  /* a sanitised path segment                                */
#define PATHN    512  /* an SD path                                              */
#define DBXN     640  /* a Dropbox path, UTF-8                                   */
#define ARGN     1600 /* a JSON argument with every non-ASCII char \u-escaped    */
#define HDRN     3900 /* auth header + Dropbox-API-Arg header + content type     */
#define FORMN    1024 /* a form-encoded token request body                      */
#define PASSN    65   /* passphrase buffer (gui_keyboard: 63 + NUL)              */
#define VAULTN   1024 /* the vault's JSON document                              */
#define MAX_PICK 256  /* cap on listed titles / backups                          */

char g_key[KEYN];
char g_secret[KEYN];
char g_refresh[REFRESHN];
char g_access[ACCESSN];
char g_auth[AUTHN];
char g_pass[PASSN]; /* this run's passphrase, "" when the vault has none */

char g_cfg_dir[PATHN];
char g_vault_path[PATHN];
char g_plain_path[PATHN];
char g_login_path[PATHN];
char g_tmp_zip[PATHN];
char g_platform[16]; /* "3ds" or "switch" */

/* per-run totals */
int g_up;
int g_down;
int g_same;
int g_backed;
int g_failed;

void init_paths(void)
{
    g_key[0]     = '\0';
    g_secret[0]  = '\0';
    g_refresh[0] = '\0';
    g_access[0]  = '\0';
    g_auth[0]    = '\0';
    g_pass[0]    = '\0';
    g_up         = 0;
    g_down       = 0;
    g_same       = 0;
    g_backed     = 0;
    g_failed     = 0;

    char* root = app_root();
    sprintf(g_cfg_dir, "%s/config", root);
    sprintf(g_vault_path, "%s/config/dropbox.vault", root);
    sprintf(g_plain_path, "%s/config/dropbox.json", root);
    sprintf(g_login_path, "%s/config/dropbox_login.txt", root);
    sprintf(g_tmp_zip, "%s/config/_dropbox_tmp.zip", root);
    if (strstr(root, "/switch") != NULL) {
        strcpy(g_platform, "switch");
    }
    else {
        strcpy(g_platform, "3ds");
    }
    char line[PATHN + 64];
    sprintf(line, "dropbox sync (%s)", g_platform);
    script_log(line);
    free(root);
}

/* ---- tiny helpers ------------------------------------------------------- */

char* slurp_n(char* path, int* size)
{
    size[0] = 0;
    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    int bytes = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (bytes <= 0) {
        fclose(f);
        return NULL;
    }
    char* buf = malloc(bytes + 1);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    int got = fread(buf, 1, bytes, f);
    fclose(f);
    buf[got] = '\0';
    size[0]  = got;
    return buf;
}

char* slurp(char* path)
{
    int n = 0;
    return slurp_n(path, &n);
}

int file_size(char* path)
{
    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        return -1;
    }
    fseek(f, 0, SEEK_END);
    int n = ftell(f);
    fclose(f);
    return n;
}

/* Copy string member `key` of object `o` into dest[size]; "" if absent. */
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

/* Append `raw` verbatim, then `value` as the inside of a JSON string, to `dst`.
 * Every byte outside printable ASCII is written as \uXXXX (UTF-8 decoded, with
 * surrogate pairs above U+FFFF): the result goes into the Dropbox-API-Arg HTTP
 * header, which must be pure ASCII — and title names are often Japanese.
 * 0 (and dst possibly half-written) if it would not fit `limit`. */
int jstr_append(char* dst, char* raw, char* value, int limit)
{
    int j = strlen(dst);
    int i = 0;
    while (raw[i] != '\0') {
        if (j + 1 >= limit) {
            return 0;
        }
        dst[j] = raw[i];
        j      = j + 1;
        i      = i + 1;
    }
    dst[j] = '\0';

    i = 0;
    while (value[i] != '\0') {
        int c   = value[i] & 255;
        int adv = 1;
        int cp  = -1;
        char esc[16];
        esc[0] = '\0';
        if (c == '"' || c == '\\') {
            esc[0] = '\\';
            esc[1] = c;
            esc[2] = '\0';
        }
        else if (c < 32 || c == 127) {
            sprintf(esc, "\\u%04x", c);
        }
        else if (c < 128) {
            esc[0] = c;
            esc[1] = '\0';
        }
        else {
            int c1 = value[i + 1] & 255;
            if ((c & 224) == 192 && (c1 & 192) == 128) {
                cp  = ((c & 31) << 6) | (c1 & 63);
                adv = 2;
            }
            else if ((c & 240) == 224 && (c1 & 192) == 128 && (value[i + 2] & 192) == 128) {
                cp  = ((c & 15) << 12) | ((c1 & 63) << 6) | (value[i + 2] & 63);
                adv = 3;
            }
            else if ((c & 248) == 240 && (c1 & 192) == 128 && (value[i + 2] & 192) == 128 && (value[i + 3] & 192) == 128) {
                cp  = ((c & 7) << 18) | ((c1 & 63) << 12) | ((value[i + 2] & 63) << 6) | (value[i + 3] & 63);
                adv = 4;
            }
            else {
                cp = 65533; /* not UTF-8: replacement character */
            }
            if (cp >= 65536) {
                cp = cp - 65536;
                sprintf(esc, "\\u%04x\\u%04x", 55296 + (cp >> 10), 56320 + (cp & 1023));
            }
            else {
                sprintf(esc, "\\u%04x", cp);
            }
        }
        int n = strlen(esc);
        if (j + n >= limit) {
            return 0;
        }
        int k = 0;
        while (k < n) {
            dst[j] = esc[k];
            j      = j + 1;
            k      = k + 1;
        }
        dst[j] = '\0';
        i      = i + adv;
    }
    return 1;
}

/* Append "&key=value" (no '&' when dst is empty), value percent-encoded. */
int form_append(char* dst, char* key, char* value, int limit)
{
    char* enc = url_encode(value);
    int need  = strlen(dst) + 1 + strlen(key) + 1 + strlen(enc) + 1;
    int ok    = 0;
    if (need <= limit) {
        if (dst[0] != '\0') {
            strcat(dst, "&");
        }
        strcat(dst, key);
        strcat(dst, "=");
        strcat(dst, enc);
        ok = 1;
    }
    free(enc);
    return ok;
}

void basename_of(char* full, char* dst, int size)
{
    int last = -1;
    int i    = 0;
    while (full[i] != '\0') {
        if (full[i] == '/') {
            last = i;
        }
        i = i + 1;
    }
    /* a folder from sav_list ends in '/': take the segment before it */
    if (last >= 0 && full[last + 1] == '\0') {
        int end = last;
        last    = -1;
        i       = 0;
        while (i < end) {
            if (full[i] == '/') {
                last = i;
            }
            i = i + 1;
        }
        int j = 0;
        while (last + 1 + j < end && j < size - 1) {
            dst[j] = full[last + 1 + j];
            j      = j + 1;
        }
        dst[j] = '\0';
        return;
    }
    int start = last + 1;
    int j     = 0;
    while (full[start + j] != '\0' && j < size - 1) {
        dst[j] = full[start + j];
        j      = j + 1;
    }
    dst[j] = '\0';
}

/* One safe path segment: characters Dropbox and Windows refuse become '_',
 * trailing dots and spaces go. */
void sanitize_name(char* src, char* dst, int size)
{
    int i = 0;
    int j = 0;
    while (src[i] != '\0' && j < size - 1) {
        char c = src[i];
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            dst[j] = '_';
        }
        else if ((c & 255) < 32) {
            dst[j] = '_';
        }
        else {
            dst[j] = c;
        }
        i = i + 1;
        j = j + 1;
    }
    while (j > 0 && (dst[j - 1] == '.' || dst[j - 1] == ' ')) {
        j = j - 1;
    }
    dst[j] = '\0';
    if (dst[0] == '\0') {
        strcpy(dst, "untitled");
    }
}

int name_in(char** names, int count, char* name)
{
    int i = 0;
    while (i < count) {
        if (strcasecmp(names[i], name) == 0) {
            return 1;
        }
        i = i + 1;
    }
    return 0;
}

void free_names(char** names, int count)
{
    int i = 0;
    while (i < count) {
        free(names[i]);
        i = i + 1;
    }
}

/* Does `s` end in ".zip" (any case)? */
int is_zip(char* s)
{
    int n = strlen(s);
    return n > 4 && s[n - 4] == '.' && (s[n - 3] == 'z' || s[n - 3] == 'Z') &&
           (s[n - 2] == 'i' || s[n - 2] == 'I') && (s[n - 1] == 'p' || s[n - 1] == 'P');
}

/* ---- credential vault --------------------------------------------------- */

void seal_error_text(int rc, char* dst)
{
    if (rc == -5) {
        strcpy(dst, "Wrong passphrase, or this vault was\nsealed on a different console.");
    }
    else if (rc == -4) {
        strcpy(dst, "This console would not give up a key\nand the vault has no passphrase.");
    }
    else if (rc == -2) {
        strcpy(dst, "This vault was written by a newer\nversion of Checkpoint.");
    }
    else if (rc == -3) {
        strcpy(dst, "Out of memory.");
    }
    else if (rc == -6) {
        strcpy(dst, "This console would not produce the\nrandom bytes needed to seal a vault.");
    }
    else {
        sprintf(dst, "The vault could not be read (%d).", rc);
    }
}

int vault_write(void)
{
    char json[VAULTN];
    strcpy(json, "{");
    int ok = jstr_append(json, "\"app_key\":\"", g_key, VAULTN);
    ok     = ok && jstr_append(json, "\",\"app_secret\":\"", g_secret, VAULTN);
    ok     = ok && jstr_append(json, "\",\"refresh_token\":\"", g_refresh, VAULTN);
    ok     = ok && jstr_append(json, "\"}", "", VAULTN);
    if (!ok) {
        script_log("the credentials do not fit the vault document buffer");
        gui_message("The sign-in details are too long to\nstore. This is a bug — please report it.");
        return 0;
    }

    char* blob = NULL;
    int bn     = 0;
    gui_status("Sealing the sign-in...");
    int rc = device_seal(json, strlen(json), g_pass, &blob, &bn);
    if (rc != 0) {
        char msg[160];
        seal_error_text(rc, msg);
        script_log("device_seal failed");
        gui_message(msg);
        return 0;
    }

    sd_mkdirs(g_cfg_dir);
    FILE* f = fopen(g_vault_path, "wb");
    if (f == NULL) {
        free(blob);
        script_log("could not open dropbox.vault for writing");
        gui_message("Could not write config/dropbox.vault.");
        return 0;
    }
    int wrote = fwrite(blob, 1, bn, f);
    fclose(f);
    free(blob);
    if (wrote != bn) {
        script_log("dropbox.vault was written short");
        gui_message("config/dropbox.vault was written short.\nThe SD card may be full.");
        return 0;
    }
    script_log(g_pass[0] != '\0' ? "vault sealed (console key + passphrase)" : "vault sealed (console key)");
    return 1;
}

/* 1 = loaded, 0 = no vault yet, -1 = there is one but it could not be opened */
int vault_read(void)
{
    int n     = 0;
    char* raw = slurp_n(g_vault_path, &n);
    if (raw == NULL) {
        return 0;
    }
    int needs = seal_needs_passphrase(raw, n);
    if (needs < 0) {
        free(raw);
        script_log("dropbox.vault is not a sealed file");
        gui_message("config/dropbox.vault is damaged.\nDelete it to sign in again.");
        return -1;
    }

    char* json = NULL;
    int jn     = 0;
    int rc     = 0;
    int tries  = 0;
    while (tries < 3) {
        g_pass[0] = '\0';
        if (needs == 1) {
            gui_keyboard(g_pass, "Dropbox passphrase", PASSN);
            if (g_pass[0] == '\0') {
                free(raw);
                script_log("cancelled at the passphrase prompt");
                return -1;
            }
        }
        gui_status("Opening the credential vault...");
        rc = device_unseal(raw, n, g_pass, &json, &jn);
        if (rc == 0) {
            break;
        }
        tries = tries + 1;
        if (rc != -5 || needs != 1 || tries == 3) {
            break;
        }
        gui_message("That passphrase did not open the vault.\nTry again.");
    }
    free(raw);
    if (rc != 0) {
        char msg[160];
        seal_error_text(rc, msg);
        script_log("device_unseal failed");
        gui_message(msg);
        return -1;
    }

    struct JSON* v = json_new();
    json_parse(v, json);
    free(json);
    jget(v, "app_key", g_key, KEYN);
    jget(v, "app_secret", g_secret, KEYN);
    jget(v, "refresh_token", g_refresh, REFRESHN);
    json_delete(v);
    if (g_key[0] == '\0' || g_refresh[0] == '\0') {
        script_log("the vault opened but is incomplete");
        gui_message("The vault opened but is incomplete.\nUse Settings -> Sign out, then sign in\nagain.");
        return -1;
    }
    script_log("vault opened");
    return 1;
}

int ask_new_passphrase(void)
{
    char first[PASSN];
    char again[PASSN];
    gui_keyboard(first, "New passphrase (8+ characters)", PASSN);
    if (first[0] == '\0') {
        return 0;
    }
    if (strlen(first) < 8) {
        gui_message("That passphrase is too short.\nUse at least 8 characters.");
        return 0;
    }
    gui_keyboard(again, "Type it again", PASSN);
    if (strcmp(first, again) != 0) {
        gui_message("The two passphrases did not match.\nNothing was changed.");
        return 0;
    }
    strcpy(g_pass, first);
    return 1;
}

void vault_forget(void)
{
    if (sd_exists(g_vault_path)) {
        remove(g_vault_path);
    }
    g_key[0]     = '\0';
    g_secret[0]  = '\0';
    g_refresh[0] = '\0';
    g_access[0]  = '\0';
    g_auth[0]    = '\0';
    g_pass[0]    = '\0';
}

/* ---- OAuth -------------------------------------------------------------- */

void explain_status(int st, char* dst)
{
    if (st == 401) {
        strcpy(dst, "Dropbox refused the sign-in (401).\nUse Settings -> Sign out, then sign in\nagain.");
    }
    else if (st == 400) {
        strcpy(dst, "Dropbox rejected the request (400).");
    }
    else if (st == 403) {
        strcpy(dst, "Dropbox forbade the request (403).\nCheck the app's Permissions tab:\nfiles.metadata.read, files.content.read\nand files.content.write must be ticked.");
    }
    else if (st == 409) {
        strcpy(dst, "Dropbox could not do that with this\npath (409).");
    }
    else if (st == 429) {
        strcpy(dst, "Dropbox asked to slow down (429).\nTry again in a few minutes.");
    }
    else if (st == -1) {
        strcpy(dst, "No HTTP support is available.");
    }
    else if (st == -2) {
        strcpy(dst, "The answer did not fit in memory.");
    }
    else if (st < 0) {
        sprintf(dst, "Dropbox could not be reached\n(transfer error %d). Check Wi-Fi.", st);
    }
    else {
        sprintf(dst, "Dropbox answered HTTP %d.", st);
    }
}

/* POST a form to the token endpoint. On 200 fills g_access (and g_refresh when
 * the answer carries one). Returns the HTTP status (or negative). */
int token_request(char* form)
{
    char* out = NULL;
    int n     = 0;
    int st    = web_request("POST", API_TOKEN, "Content-Type: application/x-www-form-urlencoded", form, strlen(form), &out, &n, NULL);
    if (st == 200 && out != NULL) {
        struct JSON* j = json_new();
        json_parse(j, out);
        if (json_is_valid(j)) {
            jget(j, "access_token", g_access, ACCESSN);
            char r[REFRESHN];
            jget(j, "refresh_token", r, REFRESHN);
            if (r[0] != '\0') {
                strcpy(g_refresh, r);
            }
        }
        json_delete(j);
        if (g_access[0] == '\0') {
            st = -20;
        }
    }
    else if (out != NULL) {
        /* keep Dropbox's own words for the log: "invalid_grant" etc. */
        char line[200];
        strncpy(line, out, 150);
        line[150] = '\0';
        script_log(line);
    }
    if (out != NULL) {
        free(out);
    }
    if (g_access[0] != '\0') {
        sprintf(g_auth, "Authorization: Bearer %s", g_access);
    }
    return st;
}

/* New access token from the refresh token. 1 ok. */
int refresh_access(void)
{
    char form[FORMN];
    form[0] = '\0';
    int ok  = form_append(form, "grant_type", "refresh_token", FORMN);
    ok      = ok && form_append(form, "refresh_token", g_refresh, FORMN);
    ok      = ok && form_append(form, "client_id", g_key, FORMN);
    if (g_secret[0] != '\0') {
        ok = ok && form_append(form, "client_secret", g_secret, FORMN);
    }
    if (!ok) {
        return 0;
    }
    gui_status("Signing in to Dropbox...");
    g_access[0] = '\0';
    int st      = token_request(form);
    if (st == 200) {
        script_log("access token refreshed");
        return 1;
    }
    char line[64];
    sprintf(line, "token refresh failed (%d)", st);
    script_log(line);
    char msg[200];
    if (st == 400 || st == 401) {
        strcpy(msg, "Dropbox no longer accepts the saved\nsign-in (it was revoked, or the app was\ndeleted). Use Settings -> Sign out,\nthen sign in again.");
    }
    else {
        explain_status(st, msg);
    }
    gui_message(msg);
    return 0;
}

/* Read config/dropbox.json if it is there: {"app_key":"...","app_secret":"..."} */
int import_plain_config(void)
{
    char* raw = slurp(g_plain_path);
    if (raw == NULL) {
        return 0;
    }
    struct JSON* root = json_new();
    json_parse(root, raw);
    free(raw);
    if (!json_is_valid(root) || !json_is_object(root)) {
        json_delete(root);
        gui_message("config/dropbox.json is not valid JSON.\nIt was ignored.");
        return 0;
    }
    jget(root, "app_key", g_key, KEYN);
    jget(root, "app_secret", g_secret, KEYN);
    json_delete(root);
    if (g_key[0] == '\0') {
        gui_message("config/dropbox.json has no \"app_key\".");
        return 0;
    }
    script_log("app key read from dropbox.json");
    return 1;
}

/* First run: app key + secret, the authorize page, the code, the token. */
int sign_in(void)
{
    g_key[0]     = '\0';
    g_secret[0]  = '\0';
    g_refresh[0] = '\0';
    int fromFile = import_plain_config();
    if (fromFile && g_secret[0] == '\0') {
        /* a setup file with only the key: the secret is typed here instead */
        char sec[KEYN];
        gui_keyboard(sec, "App secret", KEYN);
        if (sec[0] == '\0') {
            return 0;
        }
        strcpy(g_secret, sec);
    }
    if (!fromFile) {
        gui_message("Sign in to Dropbox.\n\nYou need the App key and App secret of\nyour own Dropbox app (Settings tab at\ndropbox.com/developers/apps).\nSee scripts/dropbox.md for the setup.");
        char buf[KEYN];
        gui_keyboard(buf, "App key", KEYN);
        if (buf[0] == '\0') {
            return 0;
        }
        strcpy(g_key, buf);
        gui_keyboard(buf, "App secret", KEYN);
        if (buf[0] == '\0') {
            return 0;
        }
        strcpy(g_secret, buf);
    }

    char url[PATHN];
    char* ek = url_encode(g_key);
    sprintf(url, "%s?client_id=%s&response_type=code&token_access_type=offline", AUTHORIZE, ek);
    free(ek);
    sd_mkdirs(g_cfg_dir);
    FILE* f = fopen(g_login_path, "wb");
    if (f != NULL) {
        fwrite(url, 1, strlen(url), f);
        fwrite("\n", 1, 1, f);
        fclose(f);
    }
    script_log("authorize URL:");
    script_log(url);

    /* the dialog does not wrap a word that long: break the URL into lines */
    char wrapped[PATHN + 64];
    int wi = 0;
    int ui = 0;
    while (url[ui] != '\0' && wi < PATHN + 60) {
        if (ui > 0 && (ui % 36) == 0) {
            wrapped[wi] = '\n';
            wi          = wi + 1;
        }
        wrapped[wi] = url[ui];
        wi          = wi + 1;
        ui          = ui + 1;
    }
    wrapped[wi] = '\0';
    char msg[900];
    sprintf(msg, "On your phone or PC open:\n\n%s\n\n(also saved in config/dropbox_login.txt)\nSign in, press Allow, then type the code\nDropbox shows on the next screen.", wrapped);
    gui_message(msg);

    char code[CODEN];
    gui_keyboard(code, "Code from Dropbox", CODEN);
    if (code[0] == '\0') {
        script_log("cancelled at the code prompt");
        return 0;
    }

    char form[FORMN];
    form[0] = '\0';
    int ok  = form_append(form, "code", code, FORMN);
    ok      = ok && form_append(form, "grant_type", "authorization_code", FORMN);
    ok      = ok && form_append(form, "client_id", g_key, FORMN);
    ok      = ok && form_append(form, "client_secret", g_secret, FORMN);
    if (!ok) {
        return 0;
    }
    gui_status("Signing in to Dropbox...");
    g_access[0] = '\0';
    int st      = token_request(form);
    if (st != 200 || g_refresh[0] == '\0') {
        char line[64];
        sprintf(line, "code exchange failed (%d)", st);
        script_log(line);
        if (st == 400) {
            gui_message("Dropbox did not accept that code.\n\nCodes work once and expire quickly;\nalso check the App key and App secret.\nRun the script again for a new try.");
        }
        else {
            char m[200];
            explain_status(st, m);
            gui_message(m);
        }
        return 0;
    }
    script_log("signed in");

    g_pass[0] = '\0';
    if (gui_confirm("Protect the saved sign-in with a\npassphrase, typed on every run?\n\nWithout one it is still encrypted and\ntied to this console, but other homebrew\nhere could unseal it.")) {
        ask_new_passphrase();
    }
    if (!vault_write()) {
        return 0;
    }
    if (fromFile && sd_exists(g_plain_path)) {
        remove(g_plain_path);
        script_log("config/dropbox.json deleted (its contents are in the vault)");
        gui_message("Signed in. The app key and secret were\nmoved into config/dropbox.vault\n(encrypted, tied to this console), and\nconfig/dropbox.json was deleted.");
    }
    else {
        gui_message("Signed in to Dropbox.");
    }
    return 1;
}

/* Vault or first sign-in, then a fresh access token. 1 when ready. */
int ensure_signed_in(void)
{
    int have = vault_read();
    if (have < 0) {
        return 0;
    }
    if (have == 0) {
        return sign_in(); /* sign_in leaves a fresh access token behind */
    }
    return refresh_access();
}

/* ---- Dropbox API -------------------------------------------------------- */

/* RPC call with a JSON body. *out gets the malloc'd body (or NULL). */
int dbx_rpc(char* endpoint, char* body, char** out)
{
    char url[128];
    sprintf(url, "%s%s", API_RPC, endpoint);
    char h[AUTHN + 64];
    sprintf(h, "%s\nContent-Type: application/json", g_auth);
    int n  = 0;
    out[0] = NULL;
    return web_request("POST", url, h, body, strlen(body), out, &n, NULL);
}

/* The Dropbox-API-Arg header for `path` plus extra JSON members (e.g.
 * ",\"mode\":\"overwrite\""), appended to the auth header in h[HDRN]. */
int arg_header(char* h, char* path, char* extra)
{
    char arg[ARGN];
    strcpy(arg, "{");
    if (!jstr_append(arg, "\"path\":\"", path, ARGN) || !jstr_append(arg, "\"", "", ARGN)) {
        return 0;
    }
    if (strlen(arg) + strlen(extra) + 2 >= ARGN) {
        return 0;
    }
    strcat(arg, extra);
    strcat(arg, "}");
    if (strlen(g_auth) + strlen(arg) + 80 >= HDRN) {
        return 0;
    }
    sprintf(h, "%s\nDropbox-API-Arg: %s", g_auth, arg);
    return 1;
}

/* Names in Dropbox folder `path` into `names` (malloc'd), of entries whose
 * ".tag" is `tag` ("folder" or "file"). Returns the count, 0 for a folder that
 * does not exist yet, -1 when the listing itself failed. */
int dbx_list(char* path, char* tag, char** names, int max)
{
    char arg[ARGN];
    strcpy(arg, "{");
    if (!jstr_append(arg, "\"path\":\"", path, ARGN) || !jstr_append(arg, "\",\"recursive\":false,\"limit\":2000}", "", ARGN)) {
        return -1;
    }
    char* out = NULL;
    int st    = dbx_rpc("files/list_folder", arg, &out);
    int count = 0;
    while (1) {
        if (st == 409) {
            /* path/not_found: nothing uploaded there yet */
            if (out != NULL) {
                free(out);
            }
            return count;
        }
        if (st != 200 || out == NULL) {
            if (out != NULL) {
                free(out);
            }
            char line[96];
            sprintf(line, "list_folder failed (%d)", st);
            script_log(line);
            return -1;
        }
        struct JSON* j = json_new();
        json_parse(j, out);
        free(out);
        out = NULL;
        if (!json_is_valid(j) || !json_object_contains(j, "entries")) {
            json_delete(j);
            return -1;
        }
        struct JSON* entries = json_object_element(j, "entries");
        int total            = json_array_size(entries);
        int i;
        for (i = 0; i < total && count < max; i++) {
            struct JSON* e = json_array_element(entries, i);
            char t[16];
            char nm[NAMEN];
            jget(e, ".tag", t, 16);
            jget(e, "name", nm, NAMEN);
            if (strcmp(t, tag) == 0 && nm[0] != '\0') {
                names[count] = strdup(nm);
                count        = count + 1;
            }
        }
        int more = 0;
        if (json_object_contains(j, "has_more") && json_is_bool(json_object_element(j, "has_more"))) {
            more = json_get_bool(json_object_element(j, "has_more"));
        }
        char* cursor = NULL;
        if (more && count < max && json_object_contains(j, "cursor")) {
            cursor = json_get_string(json_object_element(j, "cursor"));
        }
        json_delete(j);
        if (cursor == NULL) {
            return count;
        }
        char* body = malloc(strlen(cursor) + 32);
        sprintf(body, "{\"cursor\":\"%s\"}", cursor);
        free(cursor);
        st = dbx_rpc("files/list_folder/continue", body, &out);
        free(body);
    }
    return count;
}

/* Upload the SD file `local` to Dropbox `path` (overwrite). 1 ok. */
int dbx_upload(char* local, char* path)
{
    char h[HDRN];
    if (!arg_header(h, path, ",\"mode\":\"overwrite\",\"autorename\":false,\"mute\":true")) {
        script_log("  the Dropbox path is too long");
        return 0;
    }
    strcat(h, "\nContent-Type: application/octet-stream");
    char* out = NULL;
    int n     = 0;
    int st    = web_upload_file("POST", API_UPLOAD, h, local, &out, &n, NULL);
    if (st != 200 && out != NULL) {
        char line[200];
        strncpy(line, out, 150);
        line[150] = '\0';
        script_log(line);
    }
    if (out != NULL) {
        free(out);
    }
    if (st == 200) {
        return 1;
    }
    char line[96];
    sprintf(line, "  upload rejected (code %d)", st);
    script_log(line);
    return 0;
}

/* Download Dropbox `path` into the SD file `local`. 1 ok. */
int dbx_download(char* path, char* local)
{
    char h[HDRN];
    if (!arg_header(h, path, "")) {
        script_log("  the Dropbox path is too long");
        return 0;
    }
    /* content endpoints refuse curl's default form content type */
    strcat(h, "\nContent-Type: application/octet-stream\nContent-Length: 0");
    char* out = NULL;
    int n     = 0;
    progress_note("downloading");
    int st = web_request("POST", API_DOWNLOAD, h, "", 0, &out, &n, NULL);
    if (st != 200 || out == NULL || n <= 0) {
        if (out != NULL) {
            free(out);
        }
        char line[96];
        sprintf(line, "  download failed (code %d)", st);
        script_log(line);
        return 0;
    }
    FILE* f = fopen(local, "wb");
    if (f == NULL) {
        free(out);
        script_log("  could not open the temporary file for writing");
        return 0;
    }
    int wrote = fwrite(out, 1, n, f);
    fclose(f);
    free(out);
    if (wrote != n) {
        remove(local);
        script_log("  the temporary file was written short (SD full?)");
        return 0;
    }
    return 1;
}

/* users/get_current_account: proves the sign-in works and names the account. */
int dbx_test(void)
{
    gui_status("Checking the Dropbox account...");
    char* out = NULL;
    int st    = dbx_rpc("users/get_current_account", "null", &out);
    if (st == 200 && out != NULL) {
        struct JSON* j = json_new();
        json_parse(j, out);
        char email[128];
        char name[128];
        email[0] = '\0';
        name[0]  = '\0';
        if (json_is_valid(j)) {
            jget(j, "email", email, 128);
            if (json_object_contains(j, "name")) {
                jget(json_object_element(j, "name"), "display_name", name, 128);
            }
        }
        json_delete(j);
        free(out);
        char msg[400];
        sprintf(msg, "Connected to Dropbox.\n\n%s\n%s", name, email);
        gui_message(msg);
        return 1;
    }
    if (out != NULL) {
        free(out);
    }
    char msg[200];
    explain_status(st, msg);
    gui_message(msg);
    return 0;
}

/* ---- local backups ------------------------------------------------------ */

/* "/<platform>/<saves|extdata>/<TITLEID> <name>" */
void title_dbx_path(int idx, int kind, char* dst)
{
    char* id   = title_id(idx);
    char* name = title_name(idx);
    char safe[NAMEN];
    sanitize_name(name, safe, NAMEN);
    sprintf(dst, "/%s/%s/%s %s", g_platform, kind == 0 ? "saves" : "extdata", id, safe);
    free(id);
    free(name);
}

/* Local backup folder names of title `idx` (kind 0 save, 1 extdata), sanitised
 * the same way the upload names them. Returns the count; `paths` gets the full
 * SD paths in the same order. */
int local_backups(int idx, int kind, char** names, char** paths, int max)
{
    char* base = title_backup_path(idx, kind);
    if (base[0] == '\0') {
        free(base);
        return 0;
    }
    struct directory* d = read_directory(base);
    int n               = 0;
    int i;
    for (i = 0; d != NULL && i < d->count && n < max; i++) {
        char bn[NAMEN];
        char safe[NAMEN];
        basename_of(d->files[i], bn, NAMEN);
        sanitize_name(bn, safe, NAMEN - 8);
        names[n] = strdup(safe);
        paths[n] = strdup(d->files[i]);
        n        = n + 1;
    }
    if (d != NULL) {
        delete_directory(d);
    }
    free(base);
    return n;
}

/* A folder under `base` that does not exist yet: name, "name (2)", ... */
void free_backup_dir(char* base, char* name, char* dst)
{
    sprintf(dst, "%s%s", base, name);
    int n = 2;
    while (sd_exists(dst) && n < 100) {
        sprintf(dst, "%s%s (%d)", base, name, n);
        n = n + 1;
    }
}

/* Copy one archive folder (archive path `apath`, ends in '/') into SD folder
 * `sdir` (no trailing '/'). Returns the number of files that failed. */
int copy_archive_dir(int h, char* apath, char* sdir)
{
    sd_mkdirs(sdir);
    struct directory* d = sav_list(h, apath);
    if (d == NULL) {
        return 1;
    }
    int bad = 0;
    int i;
    for (i = 0; i < d->count; i++) {
        char* e = d->files[i];
        int len = strlen(e);
        char nm[NAMEN];
        basename_of(e, nm, NAMEN);
        char sp[PATHN];
        snprintf(sp, PATHN, "%s/%s", sdir, nm);
        if (len > 0 && e[len - 1] == '/') {
            bad = bad + copy_archive_dir(h, e, sp);
        }
        else {
            char* data = NULL;
            int size   = 0;
            int rc     = sav_read(h, e, &data, &size);
            if (rc != 0) {
                bad = bad + 1;
                continue;
            }
            FILE* f = fopen(sp, "wb");
            if (f == NULL) {
                free(data);
                bad = bad + 1;
                continue;
            }
            int wrote = size > 0 ? fwrite(data, 1, size, f) : 0;
            fclose(f);
            free(data);
            if (wrote != size) {
                bad = bad + 1;
            }
        }
    }
    delete_directory(d);
    return bad;
}

/* Take a fresh backup of title `idx`'s save into Checkpoint's backup folder,
 * named like Checkpoint's own (YYYYMMDD-HHMMSS). 1 ok. */
int backup_now(int idx)
{
    if (!title_has_save(idx)) {
        return 0;
    }
    char* name = title_name(idx);
    char st[300];
    sprintf(st, "Backing up %s", name);
    gui_status(st);

    int h = sav_open(idx, 0);
    if (h < 0) {
        sprintf(st, "  %s: the save could not be opened (%d)", name, h);
        script_log(st);
        free(name);
        return 0;
    }

    int now          = time(NULL);
    struct tm* local = localtime(&now);
    char stamp[32];
    strftime(stamp, 32, "%Y%m%d-%H%M%S", local);

    char* base = title_backup_path(idx, 0);
    char dest[PATHN];
    free_backup_dir(base, stamp, dest);
    free(base);

    int bad = copy_archive_dir(h, "/", dest);
    sav_close(h);
    if (bad > 0) {
        sprintf(st, "  %s: %d file(s) could not be copied; backup kept as %s", name, bad, dest);
        script_log(st);
        free(name);
        return 0;
    }
    sprintf(st, "  backed up %s -> %s", name, dest);
    script_log(st);
    free(name);
    return 1;
}

/* ---- sync --------------------------------------------------------------- */

/* Zip `localDir` and upload it as `<dbxDir>/<zipName>`. 1 ok. */
int upload_one(char* localDir, char* dbxDir, char* zipName)
{
    progress_note(zipName);
    if (zip_dir(localDir, g_tmp_zip) != 0) {
        remove(g_tmp_zip);
        script_log("  zip failed (out of space, or cancelled)");
        return 0;
    }
    char path[DBXN];
    if (strlen(dbxDir) + strlen(zipName) + 2 >= DBXN) {
        remove(g_tmp_zip);
        return 0;
    }
    sprintf(path, "%s/%s", dbxDir, zipName);
    int ok = dbx_upload(g_tmp_zip, path);
    remove(g_tmp_zip);
    return ok;
}

/* Download `<dbxDir>/<zipName>` and unpack it into a new folder under the
 * title's local backup path. 1 ok. */
int download_one(int idx, int kind, char* dbxDir, char* zipName)
{
    char path[DBXN];
    if (strlen(dbxDir) + strlen(zipName) + 2 >= DBXN) {
        return 0;
    }
    sprintf(path, "%s/%s", dbxDir, zipName);
    if (!dbx_download(path, g_tmp_zip)) {
        return 0;
    }
    char folder[NAMEN];
    strncpy(folder, zipName, NAMEN - 1);
    folder[NAMEN - 1] = '\0';
    int len           = strlen(folder);
    if (len > 4) {
        folder[len - 4] = '\0';
    }
    char* base = title_backup_path(idx, kind);
    char dest[PATHN];
    free_backup_dir(base, folder, dest);
    free(base);
    if (sd_mkdirs(dest) != 0) {
        remove(g_tmp_zip);
        script_log("  could not create the backup folder on SD");
        return 0;
    }
    progress_note(folder);
    int rc = unzip(g_tmp_zip, dest);
    remove(g_tmp_zip);
    if (rc != 0) {
        script_log("  unzip failed (damaged file, out of space, or cancelled)");
        rmdir(dest);
        return 0;
    }
    return 1;
}

/* Two-way sync of one title's backups of one kind. Nothing is overwritten on
 * either side: a backup is only sent where its name is missing. */
void sync_title(int idx, int kind)
{
    char* names[MAX_PICK];
    char* paths[MAX_PICK];
    int ln = local_backups(idx, kind, names, paths, MAX_PICK);

    char dir[DBXN];
    title_dbx_path(idx, kind, dir);
    char* remote[MAX_PICK];
    int rn = dbx_list(dir, "file", remote, MAX_PICK);
    if (rn < 0) {
        script_log("  could not list this title in Dropbox; skipped");
        g_failed = g_failed + 1;
        free_names(names, ln);
        free_names(paths, ln);
        return;
    }
    if (ln == 0 && rn == 0) {
        return;
    }

    char* tname = title_name(idx);
    char st[400];
    sprintf(st, "== %s [%s]: %d here, %d in Dropbox", tname, kind == 0 ? "save" : "extdata", ln, rn);
    script_log(st);
    gui_status(tname);

    int i;
    /* local -> Dropbox */
    for (i = 0; i < ln; i++) {
        char zip[NAMEN];
        sprintf(zip, "%s.zip", names[i]);
        if (name_in(remote, rn, zip)) {
            g_same = g_same + 1;
            continue;
        }
        sprintf(st, "> up   %s / %s", tname, names[i]);
        script_log(st);
        if (upload_one(paths[i], dir, zip)) {
            g_up = g_up + 1;
        }
        else {
            script_log("  UPLOAD FAILED");
            g_failed = g_failed + 1;
        }
    }
    /* Dropbox -> local */
    for (i = 0; i < rn; i++) {
        if (!is_zip(remote[i])) {
            continue;
        }
        char folder[NAMEN];
        strncpy(folder, remote[i], NAMEN - 1);
        folder[NAMEN - 1] = '\0';
        folder[strlen(folder) - 4] = '\0';
        if (name_in(names, ln, folder)) {
            continue;
        }
        sprintf(st, "< down %s / %s", tname, folder);
        script_log(st);
        if (download_one(idx, kind, dir, remote[i])) {
            g_down = g_down + 1;
        }
        else {
            script_log("  DOWNLOAD FAILED");
            g_failed = g_failed + 1;
        }
    }

    free(tname);
    free_names(remote, rn);
    free_names(names, ln);
    free_names(paths, ln);
}

/* Title ids (16 hex) that have a folder in Dropbox under /<platform>/<kind>. */
int remote_title_ids(int kind, char** ids, int max)
{
    char dir[64];
    sprintf(dir, "/%s/%s", g_platform, kind == 0 ? "saves" : "extdata");
    char* folders[MAX_PICK];
    int n = dbx_list(dir, "folder", folders, MAX_PICK);
    if (n < 0) {
        return -1;
    }
    int k = 0;
    int i;
    for (i = 0; i < n && k < max; i++) {
        if (strlen(folders[i]) >= 16) {
            char id[17];
            strncpy(id, folders[i], 16);
            id[16] = '\0';
            int j;
            for (j = 0; j < 16; j++) {
                id[j] = toupper(id[j]);
            }
            ids[k] = strdup(id);
            k      = k + 1;
        }
    }
    free_names(folders, n);
    return k;
}

int has_local_backups(int idx, int kind)
{
    char* base = title_backup_path(idx, kind);
    if (base[0] == '\0') {
        free(base);
        return 0;
    }
    struct directory* d = read_directory(base);
    int has             = (d != NULL && d->count > 0);
    if (d != NULL) {
        delete_directory(d);
    }
    free(base);
    return has;
}

/* Sync every title that has backups here or a folder in Dropbox. */
void sync_all(void)
{
    int kind;
    for (kind = 0; kind < 2; kind++) {
        if (kind == 1 && strcmp(g_platform, "3ds") != 0) {
            break; /* no extdata on Switch */
        }
        progress_note("listing Dropbox");
        char* ids[MAX_PICK];
        int rn = remote_title_ids(kind, ids, MAX_PICK);
        if (rn < 0) {
            gui_message("Could not list your backups in Dropbox.");
            g_failed = g_failed + 1;
            return;
        }

        /* the titles to visit, counted first so the bar has a real total */
        int todo[MAX_PICK];
        int tn    = 0;
        int total = titles_count();
        int i;
        progress_note("looking for titles with backups");
        for (i = 0; i < total && tn < MAX_PICK; i++) {
            int has = (kind == 0) ? title_has_save(i) : title_has_extdata(i);
            if (!has) {
                continue;
            }
            char* id = title_id(i);
            if (has_local_backups(i, kind) || name_in(ids, rn, id)) {
                todo[tn] = i;
                tn       = tn + 1;
            }
            free(id);
        }
        free_names(ids, rn);

        progress_begin(0, kind == 0 ? "Saves" : "Extdata", tn);
        for (i = 0; i < tn; i++) {
            sync_title(todo[i], kind);
            progress_set(0, i + 1);
        }
        progress_end(0);
    }
}

void backup_all(void)
{
    int total = titles_count();
    int todo[MAX_PICK];
    int tn = 0;
    int i;
    for (i = 0; i < total && tn < MAX_PICK; i++) {
        if (title_has_save(i)) {
            todo[tn] = i;
            tn       = tn + 1;
        }
    }
    progress_begin(0, "Backing up", tn);
    for (i = 0; i < tn; i++) {
        if (backup_now(todo[i])) {
            g_backed = g_backed + 1;
        }
        else {
            g_failed = g_failed + 1;
        }
        progress_set(0, i + 1);
    }
    progress_end(0);
}

/* ---- settings ----------------------------------------------------------- */

int settings_menu(void)
{
    char* opts[4];
    opts[0] = "Check the account";
    opts[1] = "Set or change passphrase";
    opts[2] = "Remove passphrase";
    opts[3] = "Sign out (revoke access)";
    int c   = gui_pick_one("Settings", opts, 4);
    if (c == 0) {
        dbx_test();
    }
    else if (c == 1) {
        char saved[PASSN];
        strcpy(saved, g_pass);
        if (ask_new_passphrase()) {
            if (vault_write()) {
                gui_message("Passphrase set. You will be asked for it\neach time this script runs.\n\nIt cannot be recovered: if you forget it,\nsign out and sign in again.");
            }
            else {
                strcpy(g_pass, saved);
            }
        }
    }
    else if (c == 2) {
        if (g_pass[0] == '\0') {
            gui_message("This vault has no passphrase.");
        }
        else if (gui_confirm("Remove the passphrase?")) {
            char saved[PASSN];
            strcpy(saved, g_pass);
            g_pass[0] = '\0';
            if (!vault_write()) {
                strcpy(g_pass, saved);
            }
            else {
                gui_message("Passphrase removed.");
            }
        }
    }
    else if (c == 3) {
        if (gui_confirm("Sign out of Dropbox?\n\nThe sign-in is revoked at Dropbox and\ndeleted from this console. Your backups\nin Dropbox are left alone.")) {
            char* out = NULL;
            int st    = dbx_rpc("auth/token/revoke", "null", &out);
            if (out != NULL) {
                free(out);
            }
            vault_forget();
            char line[64];
            sprintf(line, "signed out (revoke answered %d)", st);
            script_log(line);
            gui_message(st == 200 ? "Signed out. Access was revoked at\nDropbox." : "Signed out on this console. Dropbox\ncould not be told; revoke the app at\ndropbox.com/account/connected_apps.");
            return 1;
        }
    }
    return 0;
}

/* ---- main --------------------------------------------------------------- */

int main(int argc, char** argv)
{
    init_paths();
    if (!ensure_signed_in()) {
        return 1;
    }

    char* selId = (argc > 0) ? argv[0] : "";
    int selIdx  = (selId != NULL && selId[0] != '\0') ? title_find(selId) : -1;

    char* opts[5];
    int nopts = 0;
    opts[0]   = "Sync all (upload + download)";
    opts[1]   = "Back up all saves now, then sync";
    nopts     = 2;
    int thisRow = -1;
    char thisLabel[200];
    if (selIdx >= 0) {
        char* nm = title_name(selIdx);
        snprintf(thisLabel, 200, "This title: back up + sync (%s)", nm);
        free(nm);
        thisRow     = nopts;
        opts[nopts] = thisLabel;
        nopts       = nopts + 1;
    }
    int settingsRow = nopts;
    opts[nopts]     = "Settings...";
    nopts           = nopts + 1;

    int choice = gui_pick_one("Dropbox sync", opts, nopts);
    if (choice < 0) {
        script_log("cancelled at the mode picker");
        return 0;
    }
    if (choice == settingsRow) {
        settings_menu();
        return 0;
    }

    if (choice == 1) {
        backup_all();
        sync_all();
    }
    else if (choice == thisRow) {
        if (backup_now(selIdx)) {
            g_backed = g_backed + 1;
        }
        else if (title_has_save(selIdx)) {
            g_failed = g_failed + 1;
        }
        sync_title(selIdx, 0);
        if (strcmp(g_platform, "3ds") == 0 && title_has_extdata(selIdx)) {
            sync_title(selIdx, 1);
        }
    }
    else {
        sync_all();
    }
    progress_clear();

    char summary[300];
    sprintf(summary, "Done.\n\nBacked up: %d\nUploaded: %d\nDownloaded: %d\nAlready in sync: %d\nFailed: %d", g_backed, g_up, g_down, g_same, g_failed);
    if (g_down > 0) {
        strcat(summary, "\n\nDownloaded backups are in Checkpoint's\nlist; restore them there to use them.");
    }
    script_log("summary:");
    script_log(summary);
    gui_message(summary);
    return g_failed ? 1 : 0;
}
