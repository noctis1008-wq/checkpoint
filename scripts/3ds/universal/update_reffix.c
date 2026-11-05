/*
 * update_reffix.c — one-touch update of GodMode9_RefFix (Checkpoint script, 3DS)
 *
 * Downloads the latest GodMode9_RefFix.firm from GitHub and puts it in
 * /luma/payloads/, with no questions asked. If a copy is already there under a
 * hotkey name (x_GodMode9_RefFix.firm, y_..., etc.), that file is replaced, so
 * the button you use at boot keeps working. The new file is written as
 * "<name>.part" first and only replaces the old one once it is complete.
 */

#include <checkpoint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

#define URL      "https://github.com/noctis1008-wq/godmode9-with-cartridge-fixer/releases/latest/download/GodMode9_RefFix.firm"
#define PAYLOADS "/luma/payloads"
#define BASENAME "GodMode9_RefFix.firm"
#define PATHN    512
#define NAMEN    160

/* Does `name` end in BASENAME, case-insensitively? (x_GodMode9_RefFix.firm too) */
int is_reffix(char* name)
{
    char* base = BASENAME;
    int n      = strlen(name);
    int b      = strlen(base);
    if (n < b) {
        return 0;
    }
    int i = 0;
    while (i < b) {
        if (tolower(name[n - b + i]) != tolower(base[i])) {
            return 0;
        }
        i = i + 1;
    }
    /* exactly the name, or a hotkey prefix ending in '_' */
    return n == b || name[n - b - 1] == '_';
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
    int j = 0;
    while (full[last + 1 + j] != '\0' && j < size - 1) {
        dst[j] = full[last + 1 + j];
        j      = j + 1;
    }
    dst[j] = '\0';
}

int main(int argc, char** argv)
{
    char dir[PATHN];
    sprintf(dir, "%s", PAYLOADS);
    sd_mkdirs(dir);

    /* keep the name already in use (hotkey prefix included) */
    char target[PATHN];
    sprintf(target, "%s/%s", dir, BASENAME);
    struct directory* d = read_directory(dir);
    int i;
    for (i = 0; d != NULL && i < d->count; i++) {
        char nm[NAMEN];
        basename_of(d->files[i], nm, NAMEN);
        if (is_reffix(nm)) {
            sprintf(target, "%s/%s", dir, nm);
            break;
        }
    }
    if (d != NULL) {
        delete_directory(d);
    }

    gui_status("Downloading GodMode9_RefFix...");
    progress_note("downloading");
    script_log("GET latest GodMode9_RefFix.firm");
    char* out = NULL;
    int n     = 0;
    int st    = web_get(&out, &n, URL);
    /* a FIRM starts with "FIRM": refuse anything else (an error page, say) */
    if (st != 200 || out == NULL || n < 512 || out[0] != 'F' || out[1] != 'I' || out[2] != 'R' || out[3] != 'M') {
        if (out != NULL) {
            free(out);
        }
        char msg[160];
        if (st < 0) {
            sprintf(msg, "Download failed (transfer error %d).\nCheck Wi-Fi and try again.", st);
        }
        else {
            sprintf(msg, "Download failed (HTTP %d).\nNothing was changed.", st);
        }
        script_log(msg);
        gui_message(msg);
        return 1;
    }

    char part[PATHN + 8];
    sprintf(part, "%s.part", target);
    FILE* f = fopen(part, "wb");
    int wrote = 0;
    if (f != NULL) {
        wrote = fwrite(out, 1, n, f);
        fclose(f);
    }
    free(out);
    if (f == NULL || wrote != n) {
        remove(part);
        gui_message("Could not write to the SD card.\nNothing was changed.");
        return 1;
    }
    if (sd_exists(target)) {
        remove(target);
    }
    if (rename(part, target) != 0) {
        gui_message("The new file could not be moved into\nplace. It is saved with \".part\" at the\nend of its name.");
        return 1;
    }

    char msg[PATHN + 96];
    sprintf(msg, "Updated:\n%s\n(%d KB)\n\nIt is used from the next boot.", target, n / 1024);
    script_log(msg);
    gui_message(msg);
    return 0;
}
