#pragma once
#include <cstddef>
#include <cstdint>

namespace tmvs_mp {

// What an engine pixel shader is for. Only the tone map and the post effects after it matter for
// placing the mod's pass; the rest is for the frame trace.
enum class Role : uint8_t { Unknown = 0, Scene = 1, ToneMap = 2, Post = 3, Ui = 4, Other = 5 };

// Engine textures the mod reads, found by their binding name in each shader (tools/gen_shaderdb.py).
enum Slot : int { kSlotNormal = 0, kSlotDiffuse, kSlotSpecular, kSlotLightMask, kSlotMotion, kSlotDeferredDepth, kSlotCount };
constexpr uint8_t kNoSlot = 255;

struct ShaderEntry {
    uint64_t hi, lo;   // DXBC checksum (bytes 4..19 of the blob)
    uint16_t name;     // index into the name table
    uint8_t role;
    uint8_t slots[kSlotCount]; // texture slot of each Slot in this shader, kNoSlot if not read
};

struct ShaderInfo {
    Role role = Role::Unknown;
    const char* name = nullptr; // null: not one of the engine's cached shaders
    uint8_t slots[kSlotCount] = {kNoSlot, kNoSlot, kNoSlot, kNoSlot, kNoSlot, kNoSlot};
    bool reads(Slot s) const { return slots[s] != kNoSlot; }
};

namespace shaderdb {

// Looks up DXBC bytecode by its checksum.
ShaderInfo identify(const void* bytecode, size_t size);
const char* roleName(Role role);
size_t count();

} // namespace shaderdb
} // namespace tmvs_mp
