// settings.ini writer test: IniFile (src/inifile.cpp) must leave exactly the bytes that the same
// WritePrivateProfileStringW calls leave, in one write instead of one per key.
//   tmvs_initest [copy of a real settings.ini]
// Works in its own folder under %TEMP%; the given file is only read.
#include "inifile.h"
#include <windows.h>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>
#ifdef TMVS_INITEST_CONFIG
#include "config.h"
#endif

using tmshaders::IniFile;

namespace {

int g_failed = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            std::printf("FAILED line %d: %s\n", __LINE__, #cond);     \
            g_failed++;                                                \
        }                                                              \
    } while (0)

struct Write {
    std::wstring section, key, value;
};

std::wstring g_dir;

std::string readFile(const std::wstring& path) {
    std::string out;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return out;
    char buffer[4096];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0) out.append(buffer, n);
    fclose(f);
    return out;
}

void writeFile(const std::wstring& path, const std::string& bytes) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    fwrite(bytes.data(), 1, bytes.size(), f);
    fclose(f);
}

void show(const char* label, const std::string& bytes) {
    std::printf("  %s:", label);
    for (unsigned char c : bytes) {
        if (c >= 32 && c < 127) std::printf("%c", c);
        else std::printf("\\x%02x", c);
    }
    std::printf("\n");
}

void applyOld(const std::wstring& path, const std::vector<Write>& writes) {
    for (const Write& w : writes) WritePrivateProfileStringW(w.section.c_str(), w.key.c_str(), w.value.c_str(), path.c_str());
}

bool applyNew(const std::wstring& path, const std::vector<Write>& writes) {
    IniFile ini;
    if (!ini.load(path)) return false;
    for (const Write& w : writes) ini.set(w.section.c_str(), w.key.c_str(), w.value.c_str());
    return ini.save(path);
}

// initial == nullptr: no file at the start.
bool same(const char* name, const std::string* initial, const std::vector<Write>& writes) {
    const std::wstring oldPath = g_dir + L"\\old.ini", newPath = g_dir + L"\\new.ini";
    DeleteFileW(oldPath.c_str());
    DeleteFileW(newPath.c_str());
    if (initial) {
        writeFile(oldPath, *initial);
        writeFile(newPath, *initial);
    }
    applyOld(oldPath, writes);
    const bool saved = applyNew(newPath, writes);
    const std::string a = readFile(oldPath), b = readFile(newPath);
    const bool ok = saved && a == b && GetFileAttributesW((newPath + L".tmp").c_str()) == INVALID_FILE_ATTRIBUTES;
    if (!ok) {
        std::printf("FAILED case %s (saved %d)\n", name, saved ? 1 : 0);
        show("windows", a);
        show("inifile", b);
        g_failed++;
    }
    // What the game reads back is the same too.
    for (const Write& w : writes) {
        wchar_t x[512] = {}, y[512] = {};
        GetPrivateProfileStringW(w.section.c_str(), w.key.c_str(), L"<none>", x, 512, oldPath.c_str());
        GetPrivateProfileStringW(w.section.c_str(), w.key.c_str(), L"<none>", y, 512, newPath.c_str());
        if (wcscmp(x, y) != 0) {
            std::printf("FAILED case %s: [%ls] %ls reads \"%ls\" vs \"%ls\"\n", name, w.section.c_str(), w.key.c_str(), x, y);
            g_failed++;
        }
    }
    return ok;
}

std::string utf16(const std::wstring& text) {
    std::string out("\xFF\xFE", 2);
    out.append(reinterpret_cast<const char*>(text.data()), text.size() * sizeof(wchar_t));
    return out;
}

// Every key of the file (in order), through GetPrivateProfileString itself.
std::vector<Write> keysOf(const std::wstring& path) {
    std::vector<Write> out;
    std::vector<wchar_t> names(1 << 16), keys(1 << 16);
    GetPrivateProfileSectionNamesW(names.data(), static_cast<DWORD>(names.size()), path.c_str());
    for (const wchar_t* s = names.data(); *s; s += wcslen(s) + 1) {
        GetPrivateProfileStringW(s, nullptr, L"", keys.data(), static_cast<DWORD>(keys.size()), path.c_str());
        for (const wchar_t* k = keys.data(); *k; k += wcslen(k) + 1) {
            wchar_t value[512] = {};
            GetPrivateProfileStringW(s, k, L"", value, 512, path.c_str());
            out.push_back({s, k, value});
        }
    }
    return out;
}

double ms(std::chrono::steady_clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    g_dir = std::wstring(temp) + L"tmvs_initest_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(g_dir.c_str(), nullptr);

    // The cases found by trying WritePrivateProfileStringW (Windows 11).
    struct Case {
        const char* name;
        const char* initial; // nullptr: no file
        size_t length;
        std::vector<Write> writes;
    };
#define BYTES(s) s, sizeof(s) - 1
    const std::vector<Case> cases = {
        {"new file", nullptr, 0, {{L"A", L"k", L"v"}, {L"A", L"k2", L"v2"}, {L"B", L"x", L"1"}}},
        {"empty file", BYTES(""), {{L"A", L"k", L"1"}}},
        {"existing", BYTES("[A]\r\nk = old \r\n\r\n[B]\r\ny=2"), {{L"A", L"k", L"new"}, {L"A", L"new", L"1"}, {L"B", L"z", L"3"}, {L"C", L"c", L"1"}}},
        {"case", BYTES("[a]\r\nK=1\r\n"), {{L"A", L"k", L"2"}}},
        {"duplicate keys", BYTES("[A]\r\nk=1\r\nk=2\r\n"), {{L"A", L"k", L"3"}}},
        {"duplicate sections", BYTES("[A]\r\nk=1\r\n[A]\r\nj=1\r\n"), {{L"A", L"j", L"2"}, {L"A", L"n", L"1"}}},
        {"comments", BYTES("; top\r\n[A]\r\nk=1\r\n; mid\r\n\r\nj=2\r\n; tail\r\n\r\n\r\n[B]\r\n"), {{L"A", L"n", L"1"}, {L"A", L"j", L"9"}}},
        {"comment keys", BYTES("[A]\r\n; k=1\r\n;k=2\r\n  ;k=5\r\n"), {{L"A", L"k", L"3"}, {L"A", L";k", L"4"}}},
        {"spaces", BYTES("[A]\r\n"), {{L"A", L"k", L" v "}, {L"A", L" sp ", L"x"}, {L" A ", L"t", L"\t"}}},
        {"LF only", BYTES("[A]\nk=1\n[B]\ny=1\n"), {{L"A", L"k", L"2"}, {L"A", L"n", L"1"}, {L"B", L"z", L"1"}}},
        {"CR only", BYTES("[A]\rk=1\r[B]\r"), {{L"A", L"n", L"1"}, {L"B", L"m", L"1"}}},
        {"no key line", BYTES("[A]\r\nfoo\r\nk=1\r\nbar\r\n; c=1\r\n\r\n[B]\r\n"), {{L"A", L"foo", L"2"}, {L"A", L"n", L"1"}}},
        {"indented", BYTES("  [ A ]  \r\n  k  =1\r\n\tj\t=1\r\n"), {{L"A", L"k", L"2"}, {L" A ", L"k", L"3"}, {L"A", L"j", L"4"}}},
        {"ANSI", BYTES("[A]\r\nk=\xe9\r\nraw=\x81\x8d\x8f\x90\x9d\x80\xff\r\n"), {{L"A", L"k", L"\u00e8"}, {L"A", L"u", L"\u4e2d \u0101 \u20ac"}, {L"\u00c4", L"\u00e4", L"1"}}},
        {"UTF-8 BOM", BYTES("\xef\xbb\xbf[A]\r\nk=1\r\n"), {{L"A", L"k", L"\u00e9"}}},
        {"only blank lines", BYTES("\r\n\r\n"), {{L"A", L"k", L"1"}}},
        {"no line break at the end", BYTES("[A]\r\nk=1"), {{L"A", L"n", L"1"}}},
        {"no line break, new section", BYTES("[A]\r\nk=1"), {{L"B", L"n", L"1"}}},
        {"no line break, other section", BYTES("[A]\r\nk=1\r\n[B]\r\nj=1"), {{L"A", L"k", L"2"}}},
        {"empty section", BYTES("[A]\r\n[B]\r\nk=1\r\n"), {{L"A", L"n", L"1"}}},
        {"before any section", BYTES("k=1\r\n[A]\r\nj=1\r\n"), {{L"A", L"k", L"2"}}},
        {"odd headers", BYTES("[A] junk\r\nk=1\r\n[B\r\nj=1\r\n[\tC\t]\r\n"), {{L"A", L"k", L"2"}, {L"B", L"j", L"2"}, {L"C", L"c", L"2"}}},
        {"equals in value", BYTES("[A]\r\nk=a=b\r\n"), {{L"A", L"k", L"x=y"}}},
        {"empty values", BYTES("[A]\r\nk=\r\n=1\r\n"), {{L"A", L"k", L""}, {L"A", L"n", L""}, {L"A", L"", L"2"}}},
        {"inner spaces", BYTES("[A]\r\nmy key=1\r\n"), {{L"A", L"my key", L"2"}, {L"A", L"MY KEY", L"3"}}},
        {"NULs at the end", BYTES("[A]\r\nk=1\r\n\0\0"), {{L"A", L"k", L"2"}}},
        {"NUL inside", BYTES("[A]\r\nk=1\0junk\r\nj=1\r\n"), {{L"A", L"j", L"2"}}},
        {"trailing blanks", BYTES("[A]   \r\nk=1   \r\n[B]\r\n"), {{L"A", L"n", L"2"}, {L"A", L"k", L"3"}}},
        {"blank values", BYTES("[A]\r\nk=   \r\nj=\t1 \t\r\ni = \r\n"), {{L"A", L"k", L"1"}, {L"A", L"j", L" 2 "}, {L"A", L"i", L""}}},
    };
#undef BYTES
    for (const Case& c : cases) {
        const std::string initial = c.initial ? std::string(c.initial, c.length) : std::string();
        same(c.name, c.initial ? &initial : nullptr, c.writes);
    }
    {
        // UTF-16 (byte order mark FF FE) stays UTF-16.
        const std::string initial = utf16(L"[A]\r\nk=1\r\n; \u00fc\r\n");
        same("UTF-16", &initial, {{L"A", L"k", L"\u00e9"}, {L"B", L"n", L"\u4e2d"}, {L"A", L"\u00c4", L"\u20ac"}});
    }

    // A real settings.ini (or a small one): every key again with some values changed, keys and a
    // section the mod doesn't know, a preset name that isn't ASCII.
    std::string real = "[General]\r\nPreset=Vibrant\r\nEnabled=1\r\n\r\n[Settings]\r\nBloom=1\r\nExposure=0.5000\r\n[Keys]\r\nMenu=F8\r\n";
    if (argc > 1) {
        real = readFile(argv[1]);
        CHECK(!real.empty());
    }
    const std::wstring source = g_dir + L"\\source.ini";
    writeFile(source, real);
    std::vector<Write> writes = keysOf(source);
    CHECK(writes.size() >= 5);
    for (size_t i = 0; i < writes.size(); i += 3) writes[i].value += L"9";
    writes.push_back({L"General", L"PresetNight", L"Caf\u00e9 \u0101"});
    writes.push_back({L"Settings", L"NewSetting", L"0.2500"});
    writes.push_back({L"Keys", L"KeysVersion", L"2"});
    writes.push_back({L"Extra", L"Fresh", L"1"});
    std::printf("real file: %zu bytes, %zu writes\n", real.size(), writes.size());
    same("real file", &real, writes);
    same("real file into a new file", nullptr, writes);

    // A section someone else added (a later version, a hand edit) and its comment stay.
    {
        const std::wstring path = g_dir + L"\\unknown.ini";
        writeFile(path, real + "\r\n[MyStuff]\r\n; mine\r\nfoo = bar\r\n");
        CHECK(applyNew(path, writes));
        wchar_t text[64] = {};
        GetPrivateProfileStringW(L"MyStuff", L"foo", L"", text, 64, path.c_str());
        CHECK(wcscmp(text, L"bar") == 0);
        CHECK(readFile(path).find("[MyStuff]\r\n; mine\r\nfoo = bar\r\n") != std::string::npos);
        GetPrivateProfileStringW(L"Extra", L"Fresh", L"", text, 64, path.c_str());
        CHECK(wcscmp(text, L"1") == 0);
        GetPrivateProfileStringW(L"General", L"PresetNight", L"", text, 64, path.c_str());
        CHECK(wcscmp(text, L"Caf\u00e9 a") == 0 || wcscmp(text, L"Caf\u00e9 ?") == 0); // ANSI: what the code page has
    }

    // Locked by another program: nothing written, the file as it was, no .tmp left behind.
    {
        const std::wstring path = g_dir + L"\\locked.ini";
        writeFile(path, real);
        HANDLE exclusive = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        CHECK(exclusive != INVALID_HANDLE_VALUE);
        IniFile ini;
        CHECK(!ini.load(path));
        ini.set(L"General", L"Preset", L"Locked");
        CHECK(!ini.save(path)); // a file that couldn't be read is never replaced
        CloseHandle(exclusive);
        HANDLE reading = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        CHECK(applyNew(path, writes) == false);
        CHECK(GetFileAttributesW((path + L".tmp").c_str()) == INVALID_FILE_ATTRIBUTES);
        CloseHandle(reading);
        CHECK(readFile(path) == real);
        CHECK(applyNew(path, writes)); // later it works
    }

    // Time: the old way (one call per key) against one read and one write.
    {
        const std::wstring path = g_dir + L"\\time.ini";
        double oldBest = 1e9, newBest = 1e9, oldFresh = 1e9, newFresh = 1e9;
        for (int run = 0; run < 5; run++) {
            writeFile(path, real);
            auto t0 = std::chrono::steady_clock::now();
            applyOld(path, writes);
            oldBest = (std::min)(oldBest, ms(std::chrono::steady_clock::now() - t0));
            writeFile(path, real);
            t0 = std::chrono::steady_clock::now();
            applyNew(path, writes);
            newBest = (std::min)(newBest, ms(std::chrono::steady_clock::now() - t0));
            DeleteFileW(path.c_str());
            t0 = std::chrono::steady_clock::now();
            applyOld(path, writes);
            oldFresh = (std::min)(oldFresh, ms(std::chrono::steady_clock::now() - t0));
            DeleteFileW(path.c_str());
            t0 = std::chrono::steady_clock::now();
            applyNew(path, writes);
            newFresh = (std::min)(newFresh, ms(std::chrono::steady_clock::now() - t0));
        }
        std::printf("save, existing file: WritePrivateProfileString %.1f ms, IniFile %.2f ms\n", oldBest, newBest);
        std::printf("save, new file:      WritePrivateProfileString %.1f ms, IniFile %.2f ms\n", oldFresh, newFresh);
        DeleteFileW(path.c_str());
    }

#ifdef TMVS_INITEST_CONFIG
    // Config::save itself (ManiaPlanet's build only: its data folder can be pointed elsewhere with
    // TMVS_MP_DATA; without it this part is skipped, never the game's folder).
    wchar_t data[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"TMVS_MP_DATA", data, MAX_PATH)) {
        const std::wstring ini = std::wstring(data) + L"\\settings.ini";
        CreateDirectoryW(data, nullptr);
        writeFile(ini, real + "[MyStuff]\r\n; mine\r\nfoo=bar\r\n");
        tmshaders::Config& config = tmshaders::Config::get();
        config.load();
        const std::string code = config.shareCode();
        auto t0 = std::chrono::steady_clock::now();
        config.save();
        std::printf("Config::save: %.2f ms\n", ms(std::chrono::steady_clock::now() - t0));
        const std::string saved = readFile(ini);
        CHECK(saved.find("[MyStuff]\r\n; mine\r\nfoo=bar\r\n") != std::string::npos);
        CHECK(GetFileAttributesW((ini + L".tmp").c_str()) == INVALID_FILE_ATTRIBUTES);
        config.load();
        CHECK(config.shareCode() == code); // what was saved reads back the same
        config.save();
        CHECK(readFile(ini) == saved); // and saves the same again
        std::printf("Config::save on %ls: %s\n", ini.c_str(), saved == real + "[MyStuff]\r\n; mine\r\nfoo=bar\r\n" ? "file unchanged" : "file changed");
        // A file from before SMAA (FXAA, no SMAA) keeps FXAA; one that names SMAA gets it.
        writeFile(ini, "[General]\r\nPreset=Custom\r\nBasePreset=Vibrant\r\n[Settings]\r\nFXAA=1\r\n");
        config.load();
        CHECK(config.settings.fxaa && !config.settings.smaa);
        writeFile(ini, "[General]\r\nPreset=Custom\r\nBasePreset=Vibrant\r\n[Settings]\r\nFXAA=1\r\nSMAA=1\r\n");
        config.load();
        CHECK(config.settings.fxaa && config.settings.smaa);
        writeFile(ini, "[General]\r\nPreset=Vibrant\r\n");
        config.load();
        CHECK(config.settings.fxaa && config.settings.smaa); // the built-in presets: SMAA
        writeFile(ini, saved);
    }
#endif

    for (const wchar_t* name : {L"old.ini", L"new.ini", L"source.ini", L"unknown.ini", L"locked.ini"})
        DeleteFileW((g_dir + L"\\" + name).c_str());
    RemoveDirectoryW(g_dir.c_str());
    std::printf(g_failed ? "%d FAILED\n" : "all passed\n", g_failed);
    return g_failed ? 1 : 0;
}
