#pragma once
#include <string>

namespace tmshaders {
namespace log {

// Output folder: <Documents>\TrackMania\TMVS\ (created on first use).
const std::wstring& dataDir();

void write(const char* fmt, ...);

} // namespace log
} // namespace tmshaders

#define TMVS_LOG(...) ::tmshaders::log::write(__VA_ARGS__)
