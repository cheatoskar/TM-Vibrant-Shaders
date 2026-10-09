#pragma once
#include <string>

namespace tmshaders {
namespace update {

constexpr const char* kRepository = "cheatoskar/TM-Vibrant-Shaders";

enum class State { Off, Checking, UpToDate, Available, Failed };

struct Info {
    State state = State::Off;
    std::string version; // the newest release ("1.3.2"), when known
    std::string url;     // its page on GitHub
    std::string notes;   // its release notes (Markdown as written)
};

// Asks GitHub for the newest release (releases/latest: no pre-releases) on a background
// thread, at most once per call; the game never waits for it.
void check();
// What the last check found.
Info latest();

// "1.3.10" > "1.3.9"; a suffix ("-beta.1") is ignored.
bool newer(const std::string& version, const std::string& than);

} // namespace update
} // namespace tmshaders
