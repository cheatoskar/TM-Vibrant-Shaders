#include "shaderdb.h"
#include <algorithm>
#include <cstring>
#include <iterator>

namespace tmvs_mp {
namespace {

#include "shaderdb_table.inc"

} // namespace

namespace shaderdb {

ShaderInfo identify(const void* bytecode, size_t size) {
    ShaderInfo info;
    if (!bytecode || size < 20 || memcmp(bytecode, "DXBC", 4) != 0) return info;
    uint64_t hi = 0, lo = 0;
    memcpy(&hi, static_cast<const char*>(bytecode) + 4, 8);
    memcpy(&lo, static_cast<const char*>(bytecode) + 12, 8);
    const auto* end = std::end(kShaderEntries);
    const auto* it = std::lower_bound(std::begin(kShaderEntries), end, ShaderEntry{hi, lo, 0, 0, {}},
                                      [](const ShaderEntry& a, const ShaderEntry& b) {
                                          return a.hi != b.hi ? a.hi < b.hi : a.lo < b.lo;
                                      });
    if (it == end || it->hi != hi || it->lo != lo) return info;
    info.role = static_cast<Role>(it->role);
    info.name = kShaderNames[it->name];
    memcpy(info.slots, it->slots, sizeof(info.slots));
    return info;
}

const char* roleName(Role role) {
    switch (role) {
    case Role::Scene: return "scene";
    case Role::ToneMap: return "tonemap";
    case Role::Post: return "post";
    case Role::Ui: return "2d";
    case Role::Other: return "other";
    default: return "?";
    }
}

size_t count() {
    return std::size(kShaderEntries);
}

} // namespace shaderdb
} // namespace tmvs_mp
