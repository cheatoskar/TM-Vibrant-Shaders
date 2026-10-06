#pragma once
#include <string>

namespace tmshaders {
namespace log {

// Output folder: <Documents>\TrackMania\TMVS\ (created on first use).
const std::wstring& dataDir();

// Log file name in dataDir() (default tmvs.log). Set before the first write: the previewer
// uses its own file so it never truncates the game's log.
void setFileName(const wchar_t* name);

void write(const char* fmt, ...);

} // namespace log
} // namespace tmshaders

#define TMVS_LOG(...) ::tmshaders::log::write(__VA_ARGS__)
