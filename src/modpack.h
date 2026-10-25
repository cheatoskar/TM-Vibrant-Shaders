#pragma once
#include "config.h"
#include <cstdint>
#include <string>
#include <vector>

namespace tmshaders {

// Resource packs: TrackMania's environment mods (zips of textures by file name in
// Documents\TrackMania\Skins\<Environment>\Mod) on any map, chosen in the menu per environment
// (and per map mood). The game itself applies a mod only when a map links one; the engine side
// (engine::setModPackChooser) answers the game's question at map load with the choice here.
namespace modpack {

// What kind of pack. Only the game's mods so far; TMVS's own packs (textures for the mod's
// effects plus parameters, Documents\TrackMania\TMVS\Packs\<name>\pack.ini or a zip) will be a
// second kind in the same list, with their own way of being used.
enum class Kind { GameMod };

struct Pack {
    Kind kind = Kind::GameMod;
    std::string environment;  // the folder it lies in ("Stadium")
    std::string name;         // shown: the file name without .zip (UTF-8)
    std::wstring file;        // "DarkBlue.zip": the choice saved in settings.ini
    std::wstring path;        // full path
    uint64_t size = 0;        // bytes
    uint64_t written = 0;     // last write time (FILETIME): a changed file gets a new preview
    bool linked = false;      // a .loc next to it: the game downloaded it for a map
};

// Documents\TrackMania\Skins.
std::wstring skinsFolder();
// Every pack on disk, by environment (Stadium first) and name. Reads folders only (no zip).
std::vector<Pack> scan();
// The environments to offer: those with packs, Stadium always.
std::vector<std::string> environments(const std::vector<Pack>& packs);

// The pack's preview, at most maxWidth pixels wide (0xAARRGGBB, rows top-down): the zip's
// Image/LoadScreen.dds (most mods have one), else a preview.png / .jpg / .dds anywhere in it.
// False when it has none or it can't be read; `error` says why (for the log).
bool loadPreview(const std::wstring& zipPath, int maxWidth, std::vector<uint32_t>& pixels, int& width, int& height, std::string& error);
// A DDS file (DXT1/3/5, 32/24-bit RGB, 8-bit luminance) into pixels: its largest mip level that
// is at most maxWidth wide when it has mips, scaled down to maxWidth otherwise.
bool decodeDds(const unsigned char* data, size_t size, int maxWidth, std::vector<uint32_t>& pixels, int& width, int& height,
               std::string& error);
// One file of a zip (stored or deflated), by its name in the zip (case-insensitive, '/' or '\').
bool readZipEntry(const std::wstring& zipPath, const char* name, std::vector<unsigned char>& data);
// The names of a zip's files ("Image/LoadScreen.dds").
bool listZip(const std::wstring& zipPath, std::vector<std::string>& names);

// The map mood a decoration name stands for ("Day", "Sunrise", "Sunset", "Night"), Unknown for
// others: known before the decoration loads, unlike its time of day.
Mood moodOfDecoration(const char* decoration);

// The choice for a map: "" the map's own, "None", or a file name; `note` says which mood's
// choice it is, or why the all-moods one.
constexpr const char* kNone = "None";
std::string choiceFor(const Config& config, const char* environment, const char* decoration, std::string& note);

} // namespace modpack
} // namespace tmshaders
