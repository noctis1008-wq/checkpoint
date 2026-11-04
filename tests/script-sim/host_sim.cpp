// Host-side simulator for Checkpoint scripts: runs a picoc script on the build
// machine with the REAL bindings (common/script/*.cpp), backed by
//   - a fake title catalog (titles.txt: "<16hex id>|<name>|<save 0/1>|<extdata 0/1>"),
//   - save archives that are plain folders (<root>/archives/<idx>_<kind>/),
//   - scripted answers to every dialog (answers.txt, one per line, in order:
//     message -> any line, confirm -> y/n, pick -> index, keyboard -> text,
//     numpad -> number).
// Usage: host_sim <root> <script.c> [selected title id]
// <root> plays the SD card's /3ds/Checkpoint.

#include "logging.hpp"
#include "paths.hpp"
#include "scripthost.hpp"
#include "scriptrunner.hpp"
#include "scriptuibridge.hpp"
#include "scriptengine.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <map>
#include <sys/stat.h>
#include <vector>

static std::string g_root;
static std::vector<std::string> g_answers;
static size_t g_answerPos = 0;

// ---- logging / paths --------------------------------------------------------
namespace Logging {
    void log(LogLevel, const std::string& m) { fprintf(stderr, "[log] %s\n", m.c_str()); }
    void trace(const std::string& m) { log(LogLevel::TRACE, m); }
    void debug(const std::string& m) { log(LogLevel::DEBUG, m); }
    void info(const std::string& m) { log(LogLevel::INFO, m); }
    void warning(const std::string& m) { fprintf(stderr, "[warn] %s\n", m.c_str()); }
    void error(const std::string& m) { fprintf(stderr, "[error] %s\n", m.c_str()); }
}
const char* Paths::checkpointRoot(void) { return g_root.c_str(); }

// ---- UI bridge: scripted answers --------------------------------------------
static std::string nextAnswer(const char* what, const std::string& prompt)
{
    if (g_answerPos >= g_answers.size()) {
        fprintf(stderr, "\n[sim] OUT OF ANSWERS at %s: %s\n", what, prompt.c_str());
        exit(3);
    }
    return g_answers[g_answerPos++];
}

UiResponse ScriptUiBridge::request(UiRequest req)
{
    UiResponse r;
    std::string a;
    switch (req.kind) {
        case UiRequest::Kind::Message:
            printf("\n[MESSAGE] %s\n", req.prompt.c_str());
            nextAnswer("message", req.prompt);
            break;
        case UiRequest::Kind::Confirm:
            printf("\n[CONFIRM] %s\n", req.prompt.c_str());
            a           = nextAnswer("confirm", req.prompt);
            r.confirmed = (a == "y");
            printf("  -> %s\n", a.c_str());
            break;
        case UiRequest::Kind::PickOne:
            printf("\n[PICK] %s\n", req.prompt.c_str());
            for (size_t i = 0; i < req.items.size(); i++)
                printf("  %zu: %s\n", i, req.items[i].c_str());
            a       = nextAnswer("pick", req.prompt);
            r.index = atoi(a.c_str());
            printf("  -> %d\n", r.index);
            break;
        case UiRequest::Kind::PickMany:
            printf("\n[PICKMANY] %s\n", req.prompt.c_str());
            r.confirmed = false;
            break;
        case UiRequest::Kind::Keyboard:
            printf("\n[KEYBOARD max %d] %s\n", req.maxChars, req.prompt.c_str());
            a = nextAnswer("keyboard", req.prompt);
            if (a == "<cancel>") a.clear();
            if ((int)a.size() > req.maxChars - 1) a.resize(req.maxChars - 1);
            r.text = a;
            printf("  -> '%s'\n", a.c_str());
            break;
        case UiRequest::Kind::Numpad:
            a       = nextAnswer("numpad", req.prompt);
            r.index = atoi(a.c_str());
            break;
    }
    return r;
}
void ScriptUiBridge::setStatus(std::string text) { printf("[status] %s\n", text.c_str()); }
std::optional<UiRequest> ScriptUiBridge::take(void) { return std::nullopt; }
void ScriptUiBridge::respond(UiResponse) {}
void ScriptUiBridge::cancelAll(void) {}
std::string ScriptUiBridge::statusText(void) { return ""; }
void ScriptUiBridge::reset(void) {}

// ---- ScriptRunner: only what the bindings touch -----------------------------
bool ScriptRunner::cancelRequested(void) const { return false; }

// ---- host: titles + folder-backed archives ----------------------------------
struct SimTitle {
    HostTitle t;
};

class SimHost : public ScriptHost {
public:
    std::vector<HostTitle> titles;
    std::map<int, std::string> open; // handle -> folder
    int nextHandle = 0;

    void load(const std::string& file)
    {
        std::ifstream in(file);
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            HostTitle t;
            size_t a = line.find('|'), b = line.find('|', a + 1), c = line.find('|', b + 1);
            t.id         = strtoull(line.substr(0, a).c_str(), nullptr, 16);
            t.name       = line.substr(a + 1, b - a - 1);
            t.hasSave    = line.substr(b + 1, c - b - 1) == "1";
            t.hasExtdata = line.substr(c + 1) == "1";
            titles.push_back(t);
        }
    }
    std::string idHex(int idx)
    {
        char b[17];
        snprintf(b, 17, "%016llX", (unsigned long long)titles[idx].id);
        return b;
    }
    int titleCount(void) override { return (int)titles.size(); }
    bool titleAt(int idx, HostTitle& out) override
    {
        if (idx < 0 || idx >= (int)titles.size()) return false;
        out = titles[idx];
        return true;
    }
    int titleIndexOf(uint64_t id) override
    {
        for (size_t i = 0; i < titles.size(); i++)
            if (titles[i].id == id) return (int)i;
        return -1;
    }
    std::string titleBackupPath(int idx, int kind) override
    {
        return g_root + (kind == 0 ? "/saves/" : "/extdata/") + idHex(idx) + " " + titles[idx].name + "/";
    }
    int savOpen(int titleIdx, int kind) override
    {
        std::string dir = g_root + "/archives/" + idHex(titleIdx) + "_" + std::to_string(kind);
        struct stat st;
        if (stat(dir.c_str(), &st) != 0) return -1;
        int h   = nextHandle++;
        open[h] = dir;
        return h;
    }
    int savOpenShared(uint64_t) override { return -1; }
    int savOpenExtdata(uint32_t) override { return -1; }
    int savOpenSystem(uint32_t) override { return -1; }
    bool savValid(int handle) override { return open.count(handle) > 0; }
    int savRead(int handle, const char* path, char** outBuf, int* outSize) override
    {
        std::string p = open[handle] + path;
        FILE* f       = fopen(p.c_str(), "rb");
        if (!f) return -10;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        char* buf = (char*)malloc(n + 1);
        if (fread(buf, 1, n, f) != (size_t)n) {
            fclose(f);
            free(buf);
            return -4;
        }
        fclose(f);
        buf[n]   = 0;
        *outBuf  = buf;
        *outSize = (int)n;
        return 0;
    }
    int savWrite(int, const char*, const void*, size_t) override { return -1; }
    int savDelete(int, const char*) override { return -1; }
    int savMkdir(int, const char*) override { return -1; }
    int savRmdir(int, const char*) override { return -1; }
    int savRename(int, const char*, const char*) override { return -1; }
    bool savList(int handle, const std::string& dir, std::vector<HostDirEntry>& out) override
    {
        std::string p = open[handle] + dir;
        DIR* d        = opendir(p.c_str());
        if (!d) return false;
        while (struct dirent* e = readdir(d)) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            HostDirEntry he;
            he.name   = e->d_name;
            he.folder = e->d_type == DT_DIR;
            out.push_back(he);
        }
        closedir(d);
        return true;
    }
    int savCommit(int) override { return 0; }
    void savClose(int handle) override { open.erase(handle); }
    void savCloseAll(void) override { open.clear(); }
    int extdataDefaultId(uint64_t) override { return -1; }
    int extdataCreate(uint64_t, uint32_t, int, int) override { return -1; }
    int extdataDelete(uint32_t) override { return -1; }
    int deviceSecret(uint8_t* out, int want) override
    {
        for (int i = 0; i < want; i++) out[i] = (uint8_t)(i * 7 + 1);
        return want;
    }
    bool randomBytes(void* out, size_t size) override
    {
        for (size_t i = 0; i < size; i++) ((uint8_t*)out)[i] = (uint8_t)rand();
        return true;
    }
    void lowerPriority(void) override {}
};

static SimHost g_host;
ScriptHost& ScriptHost::get(void) { return g_host; }

int main(int argc, char** argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: host_sim <root> <script.c> [title id]\n");
        return 2;
    }
    g_root = argv[1];
    g_host.load(g_root + "/titles.txt");
    {
        std::ifstream in(g_root + "/answers.txt");
        std::string line;
        while (std::getline(in, line)) g_answers.push_back(line);
    }
    std::vector<std::string> args;
    args.push_back(argc > 3 ? argv[3] : "");
    auto out = ScriptEngine::run(argv[2], args);
    printf("\n[sim] exit value %d, answers used %zu/%zu\n", out.exitValue, g_answerPos, g_answers.size());
    return out.exitValue == 0 ? 0 : 1;
}
