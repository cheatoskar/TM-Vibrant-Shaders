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

namespace {

// The first texture binding named one of `names` (its RDEF chunk), or kNoSlot.
uint8_t findTexture(const void* bytecode, size_t size, const char* const* names, size_t nameCount) {
    // DXBC: "DXBC", checksum, 1, total size, chunk count, chunk offsets; RDEF: constant buffer
    // count and offset, binding count and offset, then 32-byte bindings (name offset, type, return
    // type, dimension, samples, bind point, bind count, flags). Offsets are from the chunk's data.
    const auto* b = static_cast<const unsigned char*>(bytecode);
    auto u32 = [&](size_t at) {
        uint32_t v = 0;
        memcpy(&v, b + at, 4);
        return v;
    };
    if (!b || size < 32 || memcmp(b, "DXBC", 4) != 0) return kNoSlot;
    const uint32_t chunks = u32(28);
    for (uint32_t k = 0; k < chunks && 32 + 4 * k + 4 <= size; k++) {
        const uint32_t at = u32(32 + 4 * k);
        if (static_cast<size_t>(at) + 8 > size || memcmp(b + at, "RDEF", 4) != 0) continue;
        const size_t data = at + 8, length = u32(at + 4);
        if (data + length > size || length < 16) return kNoSlot;
        const uint32_t count = u32(data + 8), offset = u32(data + 12);
        for (uint32_t i = 0; i < count && offset + 32 * (i + 1) <= length; i++) {
            const size_t bind = data + offset + 32 * i;
            const uint32_t name = u32(bind), type = u32(bind + 4), point = u32(bind + 20);
            if (type != 2 || name >= length || point > 127) continue; // textures only
            const char* s = reinterpret_cast<const char*>(b + data + name);
            const size_t room = length - name;
            for (size_t j = 0; j < nameCount; j++) {
                const size_t len = strlen(names[j]);
                if (room > len && !memcmp(s, names[j], len + 1)) return static_cast<uint8_t>(point);
            }
        }
        return kNoSlot;
    }
    return kNoSlot;
}

} // namespace

uint8_t lightTextureSlot(const void* bytecode, size_t size) {
    static const char* const kNames[] = {"TMapSelfIllum", "TMapAdvert", "TMapIllum"};
    return findTexture(bytecode, size, kNames, std::size(kNames));
}

uint8_t textureSlot(const void* bytecode, size_t size, const char* name) {
    return findTexture(bytecode, size, &name, 1);
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
