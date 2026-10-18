#pragma once
#include <cstddef>

// A small x86 (32-bit) inline hooker: a jump to the detour is written over the first bytes of a
// function, and a trampoline (the overwritten instructions, then a jump back) calls on into it.
//
// Why not the vtable: the Direct3D 9 device's method table can be rewritten under us (with
// CenterSpeed loaded, every slot of the game's device table held d3d9's own functions again after
// the first frame, and our Present never ran again). The code of the methods stays where it is.
//
// Other mods hook the same functions (CenterSpeed: kiero + MinHook). Either order chains:
// - they came first: the function starts with their jump (E9, FF 25, or a short EB to a jump just
//   above it, MinHook's hot-patch form). That jump is pointed at us and our trampoline goes where it
//   went, so the call runs us, then them, then the function.
// - we came first: they find our E9 and MinHook relocates it into its trampoline (it follows a
//   leading jump), so the call runs them, then us, then the function.
namespace tmshaders {
namespace inlinehook {

struct Hook {
    void* function = nullptr;       // the address hooked (the device method as the vtable gives it)
    unsigned char* site = nullptr;  // where our jump was written: the entry, or another mod's jump
    void* trampoline = nullptr;     // call this to run what was there before
    unsigned char saved[8] = {};    // the bytes at `site` before
    unsigned char written[8] = {};  // our bytes there
    size_t size = 0;                // of saved/written
    bool chained = false;           // onto another mod's jump (`previous` is where it went)
    void* previous = nullptr;
};

// Hooks `function`; false (with `why` set) when its first instructions can't be moved (then nothing
// was written).
bool install(void* function, void* detour, Hook& out, const char** why);

// Our jump still at its site (nobody wrote over it since).
bool intact(const Hook& hook);

// Puts the old bytes back if our jump is still there; otherwise (another mod hooked on top of us
// and calls our detour) leaves everything as it is. The trampoline is never freed.
bool remove(Hook& hook);

// Length of the x86 instruction at `code` (32-bit mode), 0 for one it doesn't know.
size_t instructionLength(const unsigned char* code);

} // namespace inlinehook
} // namespace tmshaders
