// Tests the share codes (src/config.cpp): every built-in preset survives encode -> decode exactly,
// random looks do too, garbage and hostile text is refused without a crash, unknown keys are counted,
// the machine settings never travel, and the clipboard round trip works. Built for both games
// (tmvs_sharetest, mp_sharetest): the presets differ per game, the codes must not. Exit code 0 = all passed.
#include "config.h"
#include "log.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <windows.h>

using namespace tmshaders;

static int g_failed = 0;
// Of the defaults of every look setting (key and bytes). Changed on purpose? Bump the share code version first.
static const uint64_t kDefaultsFingerprint = 0xd8b11cf8bd5ea985ull; // in key order (was 0xc47f0762ad86155b in table order)
#define CHECK(...) \
    do { \
        if (!(__VA_ARGS__)) { \
            printf("FAIL line %d: %s\n", __LINE__, #__VA_ARGS__); \
            g_failed++; \
        } \
    } while (0)

static bool isMachine(const Field& f) {
    // The same list as isSystemField() in config.cpp, written out: a field that moves in or out of
    // it changes what a code carries, and this test should notice.
    static const char* const kMachine[] = {"NeonTrail", "TrailColor", "TrailWidth", "TrailTyres", "TrailDuration", "TrailKeepRuns",
                                           "DisableGameMSAA", "ReadableGameDepth", "AutoQuality", "DisableGameFX", "GameMaterials",
                                           "TargetFPS", "WeatherSound", "CinematicOnlyInReplays", "VideoExportBest", "VideoCodec",
                                           "VideoBitrate", "VideoQuality", "Video10Bit", "VideoEncoder", "VideoFfmpegPreset", "VideoOwnFrames",
                                           "VideoShutter", "VideoLensDof", "VideoGrain", "VideoAspect", "VideoHdr", "VideoHdrPeak", "VideoHdrWhite",
                                           "TAAJitter", "GameNight", "EngineMoon", "GameSkyClouds", "CleanHighlights",
                                           "GameHdr", "EngineLights", "CasterShadows", "CasterAlphaTest", "CleanWater", "WaterFarReflections", "GameGBuffer", "GameLight", "ShadeMenuBars",
                                           "RenderScale", "FsrSharpness"};
    for (const char* key : kMachine) {
        if (!strcmp(key, f.key)) return true;
    }
    return false;
}

// Look fields added after share code version 1. A new field is safe: a code that doesn't name it gets its
// default, and the default is the look from before the field. Add it here once you've checked that; a
// changed default of an older field still fails the fingerprint below. (Version 1 ships with 1.4.1, so until
// then the defaults of these may still change: LampGlare and LampHaze got theirs from my test.)
static bool isAddedSinceV1(const Field& f) {
    static const char* const kAdded[] = {"LampGlare", "LampHaze", "LampSkyHalos", "Floodlights", "DofAutofocus", "SMAA"};
    for (const char* key : kAdded) {
        if (!strcmp(key, f.key)) return true;
    }
    return false;
}

static size_t sizeOf(const Field& f) {
    return f.kind == Field::Bool ? sizeof(bool) : (f.kind == Field::Color ? sizeof(float) * 3 : sizeof(float));
}

// Bytes of the field, or false where the two differ.
static bool sameField(const Field& f, const Settings& a, const Settings& b) {
    return !memcmp(reinterpret_cast<const char*>(&a) + f.offset, reinterpret_cast<const char*>(&b) + f.offset, sizeOf(f));
}

// How many look fields differ / machine fields differ between two settings.
static int lookDiffs(const Settings& a, const Settings& b) {
    int n = 0;
    for (const Field& f : fields()) {
        if (!isMachine(f) && !sameField(f, a, b)) n++;
    }
    return n;
}
static int machineDiffs(const Settings& a, const Settings& b) {
    int n = 0;
    for (const Field& f : fields()) {
        if (isMachine(f) && !sameField(f, a, b)) n++;
    }
    return n;
}

static std::string b64(const std::string& text) {
    static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    for (size_t i = 0; i < text.size(); i += 3) {
        const unsigned v = (static_cast<unsigned char>(text[i]) << 16) | ((i + 1 < text.size() ? static_cast<unsigned char>(text[i + 1]) : 0) << 8) |
                           (i + 2 < text.size() ? static_cast<unsigned char>(text[i + 2]) : 0);
        const size_t chars = i + 2 < text.size() ? 4 : (i + 1 < text.size() ? 3 : 2);
        for (size_t k = 0; k < chars; k++) out += kAlphabet[(v >> (18 - 6 * k)) & 63];
    }
    return out;
}

// Decodes on top of `base` and says whether it worked.
static bool decode(const std::string& text, Settings& look, ShareStats& stats, std::string& error) {
    return decodeShareCode(text, look, stats, error);
}

static const Field* fieldNamed(const char* key) {
    for (const Field& f : fields()) {
        if (!strcmp(f.key, key)) return &f;
    }
    return nullptr;
}

static float floatOf(const Settings& s, const char* key) {
    return *reinterpret_cast<const float*>(reinterpret_cast<const char*>(&s) + fieldNamed(key)->offset);
}

// sharetest --dump file: the code of every built-in preset of THIS game. sharetest --check file: reads
// such a file from the other game, every code must load here and give the same code back (what
// the sender saw is what arrives, although the presets differ between the games).
static int crossGame(const char* mode, const char* path) {
    const bool dump = !strcmp(mode, "--dump");
    FILE* f = fopen(path, dump ? "w" : "r");
    if (!f) {
        printf("can't open %s\n", path);
        return 2;
    }
    int failed = 0;
    if (dump) {
        for (int i = 0; i < static_cast<int>(Preset::Custom); i++) {
            Settings s;
            applyPreset(s, static_cast<Preset>(i));
            fprintf(f, "%s\n", shareCodeFor(s).c_str());
        }
    } else {
        char line[8192];
        int n = 0;
        while (fgets(line, sizeof(line), f)) {
            std::string code = line;
            while (!code.empty() && (code.back() == '\n' || code.back() == '\r')) code.pop_back();
            Settings s;
            ShareStats stats;
            std::string error;
            const bool ok = decodeShareCode(code, s, stats, error) && shareCodeFor(s) == code && stats.unknown == 0;
            printf("preset %d from the other game: %s (%d settings)\n", n++, ok ? "same look" : "DIFFERENT", stats.applied);
            if (!ok) failed++;
        }
    }
    fclose(f);
    return failed ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc == 3) return crossGame(argv[1], argv[2]);
    log::setFileName(L"tmvs_sharetest.log"); // never the game's log
    std::string error;
    ShareStats stats;

    // The base: nothing differs, the code is the marker alone.
    const Settings defaults = shareCodeBase();
    CHECK(shareCodeFor(defaults) == "TMVS1:" + b64("V=1"));
    // A default changed since then is written out, so older versions read it right.
    CHECK(shareCodeFor(Settings()) == "TMVS1:" + b64("V=1;Sharpen=0.8"));

    // The Settings defaults are the base of every code: if one changes, old codes change meaning.
    // Bump the share code version and keep reading the old one against the old default (config.cpp).
    {
        // In key order: the table's order is only the menu's (codes name their keys), so sorting the
        // menu must not trip this.
        std::vector<const Field*> sorted;
        for (const Field& f : fields()) sorted.push_back(&f);
        std::sort(sorted.begin(), sorted.end(), [](const Field* a, const Field* b) { return strcmp(a->key, b->key) < 0; });
        uint64_t hash = 1469598103934665603ull;
        for (const Field* field : sorted) {
            const Field& f = *field;
            if (isMachine(f) || isAddedSinceV1(f)) continue;
            const char* p = reinterpret_cast<const char*>(&defaults) + f.offset;
            for (const char* k = f.key; *k; k++) hash = (hash ^ static_cast<unsigned char>(*k)) * 1099511628211ull;
            for (size_t i = 0; i < sizeOf(f); i++) hash = (hash ^ static_cast<unsigned char>(p[i])) * 1099511628211ull;
        }
        printf("defaults fingerprint %016llx\n", static_cast<unsigned long long>(hash));
        CHECK(hash == kDefaultsFingerprint);
    }

    // Every built-in preset: encode -> decode gives the same look, and the machine settings of the
    // receiver stay as they are.
    for (int i = 0; i < static_cast<int>(Preset::Custom); i++) {
        Settings sender;
        applyPreset(sender, static_cast<Preset>(i));
        const std::string code = shareCodeFor(sender);
        Settings receiver;
        receiver.neonTrail = 2.0f;
        receiver.targetFps = 144.0f;
        receiver.weatherSound = 1.5f;
        receiver.engineLights = false;
        receiver.trailColor[1] = 0.9f;
        Settings before = receiver;
        const bool ok = decode(code, receiver, stats, error);
        printf("%-14s %4zu chars, %3d settings\n", presetName(static_cast<Preset>(i)), code.size(), stats.applied);
        CHECK(ok);
        CHECK(lookDiffs(sender, receiver) == 0);
        CHECK(machineDiffs(before, receiver) == 0);
        CHECK(code.size() < 1900); // a Discord message holds 2000
        CHECK(stats.unknown == 0 && stats.invalid == 0 && stats.clamped == 0);
        // And the code of what came out is the same code.
        CHECK(shareCodeFor(receiver) == code);
    }

    // Random looks, including the odd floats a slider can produce.
    std::mt19937 rng(12345);
    for (int round = 0; round < 300; round++) {
        Settings sender;
        char* base = reinterpret_cast<char*>(&sender);
        for (const Field& f : fields()) {
            if (isMachine(f)) continue;
            auto pick = [&] {
                std::uniform_real_distribution<float> d(f.min, f.max);
                return d(rng);
            };
            if (f.kind == Field::Bool) *reinterpret_cast<bool*>(base + f.offset) = rng() & 1;
            else if (f.kind == Field::Int) *reinterpret_cast<int*>(base + f.offset) = static_cast<int>(f.min) + static_cast<int>(rng() % (static_cast<int>(f.max - f.min) + 1));
            else if (f.kind == Field::Color) for (int c = 0; c < 3; c++) reinterpret_cast<float*>(base + f.offset)[c] = pick();
            else *reinterpret_cast<float*>(base + f.offset) = (rng() % 4 == 0) ? f.min : pick();
        }
        const std::string code = shareCodeFor(sender);
        Settings receiver;
        const bool ok = decode(code, receiver, stats, error);
        CHECK(ok);
        CHECK(lookDiffs(sender, receiver) == 0);
        if (g_failed > 20) break;
    }
    printf("random looks round-tripped\n");

    // Machine settings are never in a code, whatever the sender's were.
    {
        Settings sender;
        sender.neonTrail = 2.0f;
        sender.autoQuality = false;
        sender.targetFps = 200.0f;
        sender.aoStrength = 1.4f;
        Settings receiver;
        CHECK(decode(shareCodeFor(sender), receiver, stats, error));
        CHECK(machineDiffs(Settings(), receiver) == 0);
        CHECK(floatOf(receiver, "AOStrength") == 1.4f);
        // A code that has them anyway (written by hand): ignored and counted.
        Settings other;
        CHECK(decode("TMVS1:" + b64("V=1;AOStrength=1.2;NeonTrail=3;TargetFPS=200;TrailWidth=0.9"), other, stats, error));
        CHECK(stats.applied == 1 && stats.unknown == 3);
        CHECK(machineDiffs(Settings(), other) == 0);
    }

    // Unknown keys (the other game's, a newer version's), bad values, clamping, duplicates.
    {
        Settings s;
        CHECK(decode("TMVS1:" + b64("V=1;AOStrength=1.5;FutureThing=3;Another=x;Saturation=99;Bloom=abc;Vignette=-4;SkyMode=2.5;SkyMode=3;novalue;=5"), s, stats, error));
        printf("%s\n", shareStatusLine(stats).c_str());
        CHECK(stats.applied == 4);   // AOStrength, Saturation, Vignette, SkyMode
        CHECK(stats.unknown == 4);   // FutureThing, Another, novalue, =5
        CHECK(stats.invalid == 2);   // Bloom=abc, SkyMode=2.5
        CHECK(stats.clamped == 2);   // Saturation, Vignette
        CHECK(floatOf(s, "Saturation") == fieldNamed("Saturation")->max);
        CHECK(floatOf(s, "Vignette") == 0.0f);
        CHECK(s.skyMode == 3);
        CHECK(floatOf(s, "Bloom") == defaults.bloom);
        CHECK(shareStatusLine(stats) == "Loaded 4 settings, 4 unknown ignored, 2 unreadable ignored, 2 out of range");
        ShareStats clean;
        clean.applied = 37;
        clean.unknown = 2;
        CHECK(shareStatusLine(clean) == "Loaded 37 settings, 2 unknown ignored");
    }

    // The code inside a chat message, in any case, broken over lines, in quotes or backticks.
    {
        Settings sender;
        sender.aoStrength = 0.77f;
        sender.skyColor[1] = 0.123f;
        const std::string code = shareCodeFor(sender);
        const std::string wrapped[] = {
            "my look: " + code + " enjoy!",
            "`" + code + "`",
            "```\r\n" + code + "\r\n```",
            code.substr(0, 10) + "\r\n" + code.substr(10, 7) + "\n" + code.substr(17),
            "tmvs1:" + code.substr(6),
            "  \t" + code + "  \r\n",
        };
        for (const std::string& text : wrapped) {
            Settings s;
            const bool ok = decode(text, s, stats, error);
            CHECK(ok);
            CHECK(ok && floatOf(s, "AOStrength") == 0.77f && s.skyColor[1] == 0.123f);
        }
    }

    // Garbage: each is refused with a sentence, nothing is changed, nothing crashes.
    {
        std::string huge(3 * 1024 * 1024, 'A');
        std::string longCode = "TMVS1:" + std::string(100000, 'A');
        std::string binary;
        for (int i = 0; i < 4000; i++) binary += static_cast<char>(rng());
        std::string manyTokens = "V=1";
        for (int i = 0; i < 4000; i++) manyTokens += ";AOStrength=1;Nope" + std::to_string(i) + "=1";
        const std::string bad[] = {
            "",
            " ",
            "hello",
            "TMVS1:",
            "TMVS1:   ",
            "TMVS1:!!!!",
            "TMVS1:=====",
            "TMVS1:A",
            "TMVS1:AAAA",                                // decodes to NUL bytes
            "TMVS2:" + b64("V=1;AOStrength=1"),          // a newer version
            "TMVS:" + b64("V=1;AOStrength=1"),           // no version: a map's code
            "[TMVS:" + b64("V=1;AOStrength=1") + "]",
            "[TMVS] Preset=Neon Snow=1",
            "TMVS1:" + b64("just some words"),
            "TMVS1:" + b64("a;b;c"),
            "TMVS1:" + b64(std::string("V=1;AOStrength=\x01\x02")),
            "TMVS1:" + b64("NeonTrail=3;TargetFPS=100"),  // only machine settings
            "TMVS1:" + b64("Bloom=nan;AOStrength=inf;Gain=1e999"),
            "TMVS1:" + b64(";;;;;;;;"),
            "TMVS9999999999:abc",
            "TMVSX:abc",
            "TMVS1",
            "TMVS1TMVS1:TMVS1:",
            longCode,
            huge,
            binary,
            "TMVS1:" + b64(binary),
            std::string("TMVS1:\0AAAA", 11),
            std::string("\xff\xfe\xfd TMVS1:\x80\x81"),
        };
        int index = 0;
        for (const std::string& text : bad) {
            Settings s;
            s.aoStrength = 1.23f;
            const Settings before = s;
            const bool ok = decode(text, s, stats, error);
            if (ok || error.empty() || lookDiffs(before, s) != 0) printf("garbage #%d: ok=%d error=\"%s\"\n", index, ok, error.c_str());
            CHECK(!ok);
            CHECK(!error.empty());
            CHECK(lookDiffs(before, s) == 0);
            index++;
        }
        // Many tokens: fine, bounded, counted once.
        Settings s;
        CHECK(decode("TMVS1:" + b64(manyTokens), s, stats, error) == false || stats.applied == 1);
        printf("%d garbage inputs refused; sample message: \"%s\"\n", index, error.c_str());
    }

    // Config: a code becomes your unsaved look on top of the closest built-in preset.
    {
        Config& config = Config::get();
        Settings neon;
        applyPreset(neon, Preset::Neon);
        // An exact copy of a built-in preset selects it.
        std::string message;
        CHECK(config.applyShareCode(shareCodeFor(neon), message));
        CHECK(config.preset == "Neon");
        CHECK(lookDiffs(config.settings, neon) == 0);
        CHECK(message.find("Loaded ") == 0);
        // A changed one is Custom, based on Neon, and nothing built in changes.
        neon.bloom = 0.31f;
        neon.skyColor[0] = 0.25f;
        CHECK(config.applyShareCode(shareCodeFor(neon), message));
        CHECK(config.preset == kCustomPreset);
        CHECK(config.basePreset() == "Neon");
        CHECK(lookDiffs(config.settings, neon) == 0);
        CHECK(config.shareCode() == shareCodeFor(neon));
        Settings pristine;
        applyPreset(pristine, Preset::Neon);
        CHECK(lookDiffs(config.baseline(), pristine) == 0);
        // A refused code leaves the look alone and says why.
        const Settings keep = config.settings;
        CHECK(!config.applyShareCode("nonsense", message));
        CHECK(!message.empty() && lookDiffs(keep, config.settings) == 0 && config.preset == kCustomPreset);
    }

    // The clipboard: copy, read back, an empty one, one without text. The user's text is put back.
    {
        std::string saved, why;
        const bool hadText = textFromClipboard(saved, why);
        Settings sender;
        sender.exposure = 0.5f;
        const std::string code = shareCodeFor(sender);
        std::string back;
        if (copyTextToClipboard(code)) {
            CHECK(textFromClipboard(back, why) && back == code);
            Settings s;
            CHECK(decode(back, s, stats, error) && s.exposure == 0.5f);
            CHECK(copyTextToClipboard(""));
            CHECK(textFromClipboard(back, why) && back.empty());
            CHECK(!decode(back, s, stats, error) && !error.empty());
            std::string large(2 * 1024 * 1024, 'x');
            CHECK(copyTextToClipboard(large));
            CHECK(!textFromClipboard(back, why) && !why.empty());
            if (OpenClipboard(nullptr)) { // nothing on it
                EmptyClipboard();
                CloseClipboard();
                CHECK(!textFromClipboard(back, why) && !why.empty());
                printf("empty clipboard says: \"%s\"\n", why.c_str());
            }
        } else {
            printf("SKIP clipboard (could not open it here)\n");
        }
        if (hadText) copyTextToClipboard(saved);
    }

    printf(g_failed ? "%d FAILED\n" : "all passed\n", g_failed);
    return g_failed ? 1 : 0;
}
