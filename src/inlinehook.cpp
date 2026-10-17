#include "inlinehook.h"
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace tmshaders {
namespace inlinehook {
namespace {

constexpr size_t kJump = 5;         // E9 rel32
constexpr size_t kTrampoline = 48;  // per hook: the moved instructions (at most 4 + 15 bytes, grown by relocation) and the jump back

std::mutex& lock() {
    static std::mutex m;
    return m;
}

// Trampolines live in pages of their own (executable, never freed: another mod may have copied a
// jump into one).
unsigned char* allocateTrampoline() {
    static unsigned char* page = nullptr;
    static size_t used = 0;
    const size_t pageSize = 4096;
    if (!page || used + kTrampoline > pageSize) {
        page = static_cast<unsigned char*>(VirtualAlloc(nullptr, pageSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        used = 0;
        if (!page) return nullptr;
    }
    unsigned char* t = page + used;
    used += kTrampoline;
    return t;
}

bool readBytes(const void* address, void* out, size_t size) {
    __try {
        memcpy(out, address, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Writes code bytes. When they fit in one aligned 8-byte word (functions start 16-byte aligned, so
// the usual 5 bytes at an entry do), the word is swapped in one locked exchange: a thread running
// into the function at that moment sees the old bytes or the new ones, never half a jump.
bool writeCode(unsigned char* site, const unsigned char* bytes, size_t size) {
    DWORD old = 0;
    if (!VirtualProtect(site, size, PAGE_EXECUTE_READWRITE, &old)) return false;
    const uintptr_t a = reinterpret_cast<uintptr_t>(site);
    const uintptr_t base = a & ~static_cast<uintptr_t>(7);
    if (a + size <= base + 8) {
        DWORD oldWord = 0;
        // The aligned word may begin before `site` (same page): make all of it writable.
        VirtualProtect(reinterpret_cast<void*>(base), 8, PAGE_EXECUTE_READWRITE, &oldWord);
        volatile LONGLONG* word = reinterpret_cast<volatile LONGLONG*>(base);
        LONGLONG was = *word;
        for (;;) {
            LONGLONG now = was;
            memcpy(reinterpret_cast<unsigned char*>(&now) + (a - base), bytes, size);
            const LONGLONG seen = InterlockedCompareExchange64(word, now, was);
            if (seen == was) break;
            was = seen;
        }
    } else {
        memcpy(site, bytes, size);
    }
    VirtualProtect(site, size, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, size);
    return true;
}

// A jump to `to` that will run at `runsAt` (written into `at`, which may be a buffer).
void putJump(unsigned char* at, const void* to, const unsigned char* runsAt) {
    at[0] = 0xE9;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<intptr_t>(to) - reinterpret_cast<intptr_t>(runsAt + 5));
    memcpy(at + 1, &rel, 4);
}

void putJump(unsigned char* at, const void* to) {
    putJump(at, to, at);
}

int32_t rel32(const unsigned char* p) {
    int32_t v;
    memcpy(&v, p, 4);
    return v;
}

// ModRM, SIB and displacement bytes (32-bit addressing).
size_t modrmLength(const unsigned char* p) {
    const unsigned mod = p[0] >> 6, rm = p[0] & 7;
    size_t n = 1;
    if (mod == 3) return n;
    if (rm == 4) {
        n++;
        if (mod == 0 && (p[1] & 7) == 5) n += 4;
    } else if (mod == 0 && rm == 5) {
        n += 4;
    }
    if (mod == 1) n += 1;
    if (mod == 2) n += 4;
    return n;
}

bool isPrefix(unsigned char b) {
    return b == 0xF0 || b == 0xF2 || b == 0xF3 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 || b == 0x64 || b == 0x65 ||
           b == 0x66;
}

size_t twoByteLength(const unsigned char* start, const unsigned char* p) {
    const unsigned char op = *p++;
    const size_t before = static_cast<size_t>(p - start);
    if (op >= 0x80 && op <= 0x8F) return before + 4; // jcc rel32
    if ((op >= 0x05 && op <= 0x09) || op == 0x0B || (op >= 0x30 && op <= 0x37) || op == 0x77 || (op >= 0xA0 && op <= 0xA2) ||
        (op >= 0xA8 && op <= 0xAA) || (op >= 0xC8 && op <= 0xCF)) {
        return before;
    }
    if (op == 0x38) return before + 1 + modrmLength(p + 1);
    if (op == 0x3A) return before + 1 + modrmLength(p + 1) + 1;
    const bool modrm = op <= 0x03 || op == 0x0D || (op >= 0x10 && op <= 0x23) || (op >= 0x28 && op <= 0x2F) || (op >= 0x40 && op <= 0x76) ||
                       (op >= 0x7C && op <= 0x7F) || (op >= 0x90 && op <= 0x9F) || op == 0xA3 || op == 0xA4 || op == 0xA5 ||
                       (op >= 0xAB && op <= 0xC7) || op >= 0xD0;
    if (!modrm) return 0;
    size_t n = before + modrmLength(p);
    if ((op >= 0x70 && op <= 0x73) || op == 0xA4 || op == 0xAC || op == 0xBA || op == 0xC2 || (op >= 0xC4 && op <= 0xC6)) n += 1;
    return n;
}

} // namespace

size_t instructionLength(const unsigned char* code) {
    const unsigned char* p = code;
    bool operand16 = false;
    for (int i = 0; i < 4 && isPrefix(*p); i++, p++) operand16 = operand16 || *p == 0x66;
    if (*p == 0x67) return 0; // 16-bit addressing: not something a function starts with
    const size_t immz = operand16 ? 2 : 4;
    const unsigned char op = *p++;
    const size_t n = static_cast<size_t>(p - code);
    if (op == 0x0F) return twoByteLength(code, p);
    if (op < 0x40) {
        const unsigned low = op & 7;
        if (low < 4) return n + modrmLength(p);
        if (low == 4) return n + 1;
        if (low == 5) return n + immz;
        return n; // push/pop segment, daa and the like
    }
    if (op < 0x60) return n; // inc, dec, push, pop
    switch (op) {
        case 0x60: case 0x61: case 0x6C: case 0x6D: case 0x6E: case 0x6F: return n;
        case 0x62: case 0x63: return n + modrmLength(p);
        case 0x68: return n + immz;
        case 0x69: return n + modrmLength(p) + immz;
        case 0x6A: return n + 1;
        case 0x6B: return n + modrmLength(p) + 1;
        default: break;
    }
    if (op >= 0x70 && op <= 0x7F) return n + 1;
    if (op == 0x80 || op == 0x82 || op == 0x83) return n + modrmLength(p) + 1;
    if (op == 0x81) return n + modrmLength(p) + immz;
    if (op >= 0x84 && op <= 0x8F) return n + modrmLength(p);
    if (op >= 0x90 && op <= 0x9F) return op == 0x9A ? 0 : n;
    if (op >= 0xA0 && op <= 0xA3) return n + 4;
    if (op >= 0xA4 && op <= 0xAF) return op == 0xA8 ? n + 1 : op == 0xA9 ? n + immz : n;
    if (op >= 0xB0 && op <= 0xB7) return n + 1;
    if (op >= 0xB8 && op <= 0xBF) return n + immz;
    switch (op) {
        case 0xC0: case 0xC1: case 0xC6: return n + modrmLength(p) + 1;
        case 0xC7: return n + modrmLength(p) + immz;
        case 0xC2: case 0xCA: return n + 2;
        case 0xC8: return n + 3;
        case 0xC3: case 0xC9: case 0xCB: case 0xCC: case 0xCE: case 0xCF: return n;
        case 0xC4: case 0xC5: return n + modrmLength(p);
        case 0xCD: return n + 1;
        default: break;
    }
    if ((op >= 0xD0 && op <= 0xD3) || (op >= 0xD8 && op <= 0xDF)) return n + modrmLength(p);
    if (op == 0xD4 || op == 0xD5) return n + 1;
    if (op == 0xD6 || op == 0xD7) return n;
    if (op >= 0xE0 && op <= 0xE7) return n + 1;
    if (op == 0xE8 || op == 0xE9) return operand16 ? 0 : n + 4;
    if (op == 0xEA) return 0;
    if (op == 0xEB) return n + 1;
    if (op >= 0xEC && op <= 0xEF) return n;
    if (op == 0xF6) return n + modrmLength(p) + (((p[0] >> 3) & 7) < 2 ? 1 : 0);
    if (op == 0xF7) return n + modrmLength(p) + (((p[0] >> 3) & 7) < 2 ? immz : 0);
    if (op == 0xFE || op == 0xFF) return n + modrmLength(p);
    return n; // F1, F4, F5, F8-FD
}

namespace {

// Copies whole instructions from `from` until at least kJump bytes are covered, relocating
// relative branches. Returns the bytes covered at `from` (0: can't), `*outSize` what was emitted.
size_t relocate(const unsigned char* from, unsigned char* to, size_t* outSize, const char** why) {
    size_t covered = 0, emitted = 0;
    while (covered < kJump) {
        const unsigned char* src = from + covered;
        const size_t length = instructionLength(src);
        if (!length) {
            *why = "an instruction it doesn't know";
            return 0;
        }
        size_t prefixes = 0;
        while (prefixes < length && isPrefix(src[prefixes])) prefixes++;
        const unsigned char op = src[prefixes];
        unsigned char* dst = to + emitted;
        const intptr_t next = reinterpret_cast<intptr_t>(src + length);
        bool ends = false; // control doesn't fall through
        if (op == 0xE8 || op == 0xE9 || op == 0xEB || (op >= 0x70 && op <= 0x7F) || (op == 0x0F && src[prefixes + 1] >= 0x80 && src[prefixes + 1] <= 0x8F)) {
            intptr_t target;
            if (op == 0xEB || (op >= 0x70 && op <= 0x7F)) target = next + static_cast<int8_t>(src[length - 1]);
            else target = next + rel32(src + length - 4);
            // A branch back into the bytes the jump overwrites can't be moved.
            if (target > reinterpret_cast<intptr_t>(from) && target < reinterpret_cast<intptr_t>(from + kJump)) {
                *why = "a branch into its own first bytes";
                return 0;
            }
            memcpy(dst, src, prefixes);
            size_t at = prefixes;
            if (op == 0xEB || op == 0xE9) {
                dst[at++] = 0xE9;
                ends = true;
            } else if (op == 0xE8) {
                dst[at++] = 0xE8;
            } else if (op == 0x0F) {
                dst[at++] = 0x0F;
                dst[at++] = src[prefixes + 1];
            } else {
                dst[at++] = 0x0F;
                dst[at++] = static_cast<unsigned char>(0x80 + (op - 0x70));
            }
            const int32_t rel = static_cast<int32_t>(target - reinterpret_cast<intptr_t>(dst + at + 4));
            memcpy(dst + at, &rel, 4);
            emitted += at + 4;
        } else if (op >= 0xE0 && op <= 0xE3) {
            *why = "a loop / jcxz at its start";
            return 0;
        } else if (op == 0xCC) {
            *why = "too short (padding after it)";
            return 0;
        } else {
            memcpy(dst, src, length);
            emitted += length;
            const unsigned reg = (src[prefixes + 1] >> 3) & 7;
            ends = op == 0xC3 || op == 0xC2 || (op == 0xFF && (reg == 4 || reg == 5));
        }
        covered += length;
        if (ends && covered < kJump) {
            *why = "too short (returns or jumps within 5 bytes)";
            return 0;
        }
        if (ends) {
            *outSize = emitted;
            return covered; // nothing falls through: no jump back needed
        }
    }
    putJump(to + emitted, from + covered);
    *outSize = emitted + kJump;
    return covered;
}

} // namespace

bool install(void* function, void* detour, Hook& out, const char** why) {
    static const char* none = "";
    if (!why) why = &none;
    std::lock_guard<std::mutex> guard(lock());
    out = Hook{};
    out.function = function;
    unsigned char head[16];
    if (!function || !readBytes(function, head, sizeof(head))) {
        *why = "code not readable";
        return false;
    }
    unsigned char* entry = static_cast<unsigned char*>(function);
    unsigned char* trampoline = allocateTrampoline();
    if (!trampoline) {
        *why = "no memory for the trampoline";
        return false;
    }
    unsigned char* site = entry;
    // MinHook's hot-patch form: a short jump back into the padding above, where its E9 is.
    if (head[0] == 0xEB) {
        unsigned char* above = entry + 2 + static_cast<int8_t>(head[1]);
        unsigned char b[5];
        if (!readBytes(above, b, 5) || b[0] != 0xE9) {
            *why = "starts with a short jump that doesn't lead to a jump";
            return false;
        }
        site = above;
        memcpy(head, b, 5);
    }
    if (head[0] == 0xE9) {
        // Another mod's jump: ours takes its place, our trampoline goes where it went.
        out.previous = site + 5 + rel32(head + 1);
        putJump(trampoline, out.previous);
        out.size = kJump;
        out.chained = true;
    } else if ((head[0] == 0xFF && head[1] == 0x25) || (head[0] == 0x68 && head[5] == 0xC3)) {
        // jmp [address] / push address; ret: absolute, copied as they are.
        memcpy(trampoline, head, 6);
        if (head[0] == 0x68) {
            out.previous = reinterpret_cast<void*>(static_cast<uintptr_t>(static_cast<uint32_t>(rel32(head + 1))));
        } else {
            void* const* slot = reinterpret_cast<void* const*>(static_cast<uintptr_t>(static_cast<uint32_t>(rel32(head + 2))));
            readBytes(slot, &out.previous, sizeof(void*));
        }
        out.size = 6;
        out.chained = true;
    } else {
        size_t emitted = 0;
        const size_t covered = relocate(entry, trampoline, &emitted, why);
        if (!covered) return false;
        out.size = kJump; // the rest of the moved instructions stays as it is, never reached
    }
    memcpy(out.saved, site, out.size);
    putJump(out.written, detour, site);
    for (size_t i = kJump; i < out.size; i++) out.written[i] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), trampoline, kTrampoline);
    if (!writeCode(site, out.written, out.size)) {
        *why = "VirtualProtect refused";
        return false;
    }
    out.site = site;
    out.trampoline = trampoline;
    return true;
}

bool intact(const Hook& hook) {
    unsigned char now[8];
    return hook.site && readBytes(hook.site, now, hook.size) && memcmp(now, hook.written, hook.size) == 0;
}

bool remove(Hook& hook) {
    std::lock_guard<std::mutex> guard(lock());
    if (!intact(hook)) return false;
    const bool ok = writeCode(hook.site, hook.saved, hook.size);
    hook.site = nullptr;
    return ok;
}

} // namespace inlinehook
} // namespace tmshaders
