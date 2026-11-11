// TMVS 2.0 logic test (development tool), no device: the module states (src/modules/modules.h), the
// settings shared by the standalone and the Horizon modules (a 1.5 settings.ini taken over, the TMVS
// module's block that never writes Enabled=0, SettingsVersion), and the checkpoint clips' sectors
// (clips::CheckpointTracker). Its files go to <exe folder>\testdata (tools/testenv.cpp).
#include "clips/clips.h"
#include "config.h"
#include "log.h"
#include "modules/modules.h"
#include <windows.h>
#include <cstdio>
#include <fstream>
#include <string>

using namespace tmshaders;

namespace {

int g_failures = 0, g_checks = 0;

void check(bool ok, const char* what) {
    g_checks++;
    if (!ok) g_failures++;
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
}

std::string readText(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

void writeText(const std::wstring& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary);
    f << text;
}

void testModules() {
    printf("module states\n");
    check(!modules::hosted(), "not hosted until tmvs-core.dll says so");
    for (int i = 0; i < modules::kCount; i++) check(modules::on(static_cast<modules::Id>(i)), "standalone: every module counts as on");
    check(!modules::upscaleOnly(0.5f), "standalone: never upscale-only (the standalone's FSR runs with the shaders, as in 1.5)");
    modules::setHosted();
    check(!modules::on(modules::kShaders) && !modules::on(modules::kCapture), "hosted: nothing on before the plugins attach");
    modules::setAttached(modules::kUpscaling, true);
    modules::setEnabled(modules::kUpscaling, true);
    check(modules::on(modules::kUpscaling), "hosted: attached and enabled is on");
    check(modules::upscaleOnly(0.67f) && !modules::upscaleOnly(1.0f), "Upscaling without Shaders: upscale-only below scale 1");
    modules::setAttached(modules::kShaders, true);
    modules::setEnabled(modules::kShaders, true);
    check(!modules::upscaleOnly(0.67f), "with Shaders the pipeline scales as before");
    modules::setEnabled(modules::kShaders, false);
    check(!modules::on(modules::kShaders) && modules::attached(modules::kShaders), "switched off: attached, not on");
    const uint32_t changes = modules::takeChanges();
    check((changes & (1u << modules::kShaders)) && (changes & (1u << modules::kUpscaling)) && modules::takeChanges() == 0,
          "changes are reported once");
}

void testSettings() {
    printf("settings shared with the standalone (settings.ini)\n");
    const std::wstring ini = log::dataDir() + L"\\settings.ini";
    // A 1.5 file: no SettingsVersion, the effects on, a preset and a key of its own, a section of someone else's.
    writeText(ini, "[General]\r\nPreset=Golden Hour\r\nEnabled=1\r\nCheckForUpdates=0\r\n[Keys]\r\nEffects=F5\r\n[Mine]\r\nkeep=yes\r\n");
    Config& config = Config::get();
    config.load();
    check(config.settingsVersionRead() == 1, "a 1.5 file reads as version 1");
    check(config.preset == "Golden Hour" && config.settings.enabled, "its preset and Enabled are taken over");
    config.setShadersBlocked(true);
    check(!config.settings.enabled && config.shadersBlocked(), "TMVS module off: the effects are off");
    config.save();
    std::string text = readText(ini);
    check(text.find("Enabled=1") != std::string::npos && text.find("Enabled=0") == std::string::npos,
          "saved while blocked: settings.ini keeps Enabled=1 (the standalone finds the effects on)");
    check(text.find("SettingsVersion=2") != std::string::npos, "SettingsVersion=2 written");
    check(text.find("[Mine]") != std::string::npos && text.find("keep=yes") != std::string::npos, "a section it doesn't know is kept");
    config.load();
    check(!config.settings.enabled && config.settingsVersionRead() == 2, "loaded again while blocked: still off, version 2");
    config.setShadersBlocked(false);
    check(config.settings.enabled, "the block ends: the effects are back as they were");
    // F7 off in the standalone, then the modules: off stays off after the block too.
    config.settings.enabled = false;
    config.save();
    config.setShadersBlocked(true);
    config.setShadersBlocked(false);
    check(!config.settings.enabled, "effects switched off by you stay off after a block");
    config.settings.enabled = true;
    config.save();
}

void testCheckpoints() {
    printf("checkpoint clips: the sectors\n");
    clips::CheckpointTracker t;
    clips::SectorClip s;
    const int race = 1;
    check(!t.update(&race, 0, false, -1, s), "the start: nothing");
    check(t.update(&race, 1, false, 12340, s) && s.index == 1 && s.fromMs == -1 && s.toMs == 12340, "CP 1: from the start to 12.34 s");
    check(!t.update(&race, 1, false, 12340, s), "the same frame again: nothing");
    check(t.update(&race, 2, false, 25010, s) && s.index == 2 && s.fromMs == 12340 && s.toMs == 25010, "CP 2: from CP 1 to CP 2");
    check(!t.update(&race, 3, true, 40000, s), "the finish: no sector clip (the run's clip has it)");
    check(!t.update(&race, 0, false, -1, s), "a restart: nothing");
    check(t.update(&race, 1, false, 11000, s) && s.fromMs == -1 && s.toMs == 11000, "after the restart CP 1 counts from the start again");
    const int other = 2;
    check(!t.update(&other, 2, false, 30000, s), "another race (a new map) starts over without a clip");
    check(t.update(&other, 4, false, 50000, s) && s.index == 4 && s.fromMs == 30000, "two checkpoints in one read: the last one's sector");
}

} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    testModules();
    testSettings();
    testCheckpoints();
    printf("modulelogictest: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
