// Tests the inline hooker (src/inlinehook.cpp) on functions of its own: instruction lengths, a
// hook on a hot-patchable prologue, a second hook chained onto the first one's jump, a hook onto
// another hooker's hot-patch jump (MinHook's form), a short conditional jump moved into the
// trampoline, and removal in both orders. 32-bit only. Exit code 0 = all passed. No files written.
#include "inlinehook.h"
#include <cstdio>
#include <cstring>
#include <windows.h>
#include <d3d9.h>

#pragma comment(lib, "d3d9.lib")

using namespace tmshaders;

static int g_failed = 0;
#define CHECK(...) \
    do { \
        if (!(__VA_ARGS__)) { \
            printf("FAIL line %d: %s\n", __LINE__, #__VA_ARGS__); \
            g_failed++; \
        } \
    } while (0)

// a + 1, with the prologue d3d9's methods have (mov edi, edi; push ebp; mov ebp, esp).
__declspec(naked) int __stdcall plusOne(int) {
    __asm {
        mov edi, edi
        push ebp
        mov ebp, esp
        mov eax, [ebp + 8]
        add eax, 1
        pop ebp
        ret 4
    }
}

// Five bytes of padding, then a + 1 as above: the padding is where a hot-patch jump goes.
__declspec(naked) void paddedPlusOne() {
    __asm {
        nop
        nop
        nop
        nop
        nop
        mov edi, edi
        push ebp
        mov ebp, esp
        mov eax, [ebp + 8]
        add eax, 1
        pop ebp
        ret 4
    }
}

// a == 0 ? 7 : 3, starting with a short conditional jump (test ecx, ecx; jz).
__declspec(naked) int __fastcall branchy(int) {
    __asm {
        test ecx, ecx
        jz zero
        mov eax, 3
        ret
    zero:
        mov eax, 7
        ret
    }
}

using Fn = int(__stdcall*)(int);
using FastFn = int(__fastcall*)(int);
static Fn g_inner = nullptr;
static Fn g_outer = nullptr;
static Fn g_foreignOriginal = nullptr;
static Fn g_afterForeign = nullptr;
static FastFn g_branchy = nullptr;

static int __stdcall innerDetour(int a) { return g_inner(a) + 100; }
static int __stdcall outerDetour(int a) { return g_outer(a) * 2; }
static int __stdcall foreignDetour(int a) { return g_foreignOriginal(a) + 1000; }
static int __stdcall afterForeignDetour(int a) { return g_afterForeign(a) + 100; }
static int __fastcall branchyDetour(int a) { return g_branchy(a) + 10; }

static void lengths() {
    struct Case {
        unsigned char bytes[16];
        size_t length;
    };
    const Case cases[] = {
        {{0x8B, 0xFF}, 2},                                         // mov edi, edi
        {{0x55}, 1},                                               // push ebp
        {{0x8B, 0xEC}, 2},                                         // mov ebp, esp
        {{0x6A, 0x14}, 2},                                         // push 14h
        {{0x68, 1, 2, 3, 4}, 5},                                   // push imm32
        {{0x8B, 0x44, 0x24, 0x04}, 4},                             // mov eax, [esp+4]
        {{0x81, 0xEC, 0x00, 0x01, 0x00, 0x00}, 6},                 // sub esp, 100h
        {{0x83, 0xEC, 0x10}, 3},                                   // sub esp, 10h
        {{0x64, 0xA1, 0, 0, 0, 0}, 6},                             // mov eax, fs:[0]
        {{0xF6, 0x05, 1, 2, 3, 4, 0x01}, 7},                       // test byte ptr [abs], 1
        {{0xF7, 0xD8}, 2},                                         // neg eax
        {{0x0F, 0xB6, 0x45, 0x08}, 4},                             // movzx eax, byte ptr [ebp+8]
        {{0x66, 0x89, 0x45, 0xF0}, 4},                             // mov [ebp-10h], ax
        {{0x66, 0xC7, 0x45, 0xF0, 1, 0}, 6},                       // mov word ptr [ebp-10h], 1
        {{0xC7, 0x45, 0xFC, 0, 0, 0, 0}, 7},                       // mov dword ptr [ebp-4], 0
        {{0xF3, 0x0F, 0x10, 0x45, 0x08}, 5},                       // movss xmm0, [ebp+8]
        {{0x0F, 0x1F, 0x44, 0x00, 0x00}, 5},                       // nop dword ptr [eax+eax]
        {{0x8D, 0x84, 0x24, 0x00, 0x01, 0x00, 0x00}, 7},           // lea eax, [esp+100h]
        {{0x8B, 0x0D, 1, 2, 3, 4}, 6},                             // mov ecx, [abs]
        {{0xE8, 0, 0, 0, 0}, 5},                                   // call rel32
        {{0xE9, 0, 0, 0, 0}, 5},                                   // jmp rel32
        {{0xFF, 0x25, 1, 2, 3, 4}, 6},                             // jmp [abs]
        {{0x74, 0x05}, 2},                                         // jz short
        {{0x0F, 0x84, 0, 0, 0, 0}, 6},                             // jz rel32
        {{0xC2, 0x08, 0x00}, 3},                                   // ret 8
        {{0x56}, 1},                                               // push esi
        {{0x33, 0xC0}, 2},                                         // xor eax, eax
        {{0xA1, 1, 2, 3, 4}, 5},                                   // mov eax, [abs]
        {{0xD9, 0x45, 0x08}, 3},                                   // fld dword ptr [ebp+8]
        {{0x0F, 0x28, 0xC1}, 3},                                   // movaps xmm0, xmm1
        {{0x66, 0x0F, 0x6F, 0x45, 0x10}, 5},                       // movdqa xmm0, [ebp+10h]
    };
    int n = 0;
    for (const Case& c : cases) {
        const size_t got = inlinehook::instructionLength(c.bytes);
        if (got != c.length) {
            printf("FAIL length of %02X %02X %02X: %u, expected %u\n", c.bytes[0], c.bytes[1], c.bytes[2], static_cast<unsigned>(got),
                   static_cast<unsigned>(c.length));
            g_failed++;
        }
        n++;
    }
    printf("%d instruction lengths checked\n", n);
}

// The real d3d9.dll of this Windows: a device made as the game makes its own (HAL, mixed vertex
// processing, depth buffer) in a hidden window. Every method the mod hooks must take an inline
// hook (and give it back); Present and Clear are called through theirs.
using PresentFn = HRESULT(APIENTRY*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using ClearFn = HRESULT(APIENTRY*)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
static PresentFn g_present = nullptr;
static ClearFn g_clear = nullptr;
static int g_presents = 0, g_clears = 0;
static HRESULT APIENTRY presentDetour(IDirect3DDevice9* d, const RECT* a, const RECT* b, HWND w, const RGNDATA* r) {
    g_presents++;
    return g_present(d, a, b, w, r);
}
static HRESULT APIENTRY clearDetour(IDirect3DDevice9* d, DWORD n, const D3DRECT* r, DWORD f, D3DCOLOR c, float z, DWORD s) {
    g_clears++;
    return g_clear(d, n, r, f, c, z, s);
}

static void realDevice() {
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    HWND window = CreateWindowW(L"STATIC", L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, nullptr, nullptr);
    IDirect3DDevice9* device = nullptr;
    if (d3d && window) {
        D3DPRESENT_PARAMETERS pp{};
        pp.Windowed = TRUE;
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.hDeviceWindow = window;
        pp.BackBufferWidth = 64;
        pp.BackBufferHeight = 64;
        pp.EnableAutoDepthStencil = TRUE;
        pp.AutoDepthStencilFormat = D3DFMT_D24S8;
        d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_MIXED_VERTEXPROCESSING, &pp, &device);
    }
    if (!device) {
        printf("d3d9 device: not available here, skipped\n");
    } else {
        const int slots[] = {16, 17, 23, 24, 25, 26, 27, 28, 29, 34, 37, 38, 39, 41, 42, 43, 44, 47, 48, 51, 53, 65, 75, 76, 81, 82, 83, 84, 92, 94, 106, 107, 109};
        void** table = *reinterpret_cast<void***>(device);
        int hooked = 0, first[16] = {}, firstCount = 0;
        for (int slot : slots) {
            inlinehook::Hook h;
            const char* why = "";
            const unsigned char* code = static_cast<const unsigned char*>(table[slot]);
            if (slot == 17 || slot == 43) continue; // below, with detours that call through
            if (inlinehook::install(table[slot], reinterpret_cast<void*>(&clearDetour), h, &why)) {
                hooked++;
                CHECK(inlinehook::remove(h));
            } else {
                printf("FAIL d3d9 slot %d: %s (%02X %02X %02X %02X %02X %02X)\n", slot, why, code[0], code[1], code[2], code[3], code[4], code[5]);
                g_failed++;
            }
            if (firstCount < 16) first[firstCount++] = code[0];
        }
        inlinehook::Hook p, c;
        const char* why = "";
        CHECK(inlinehook::install(table[17], reinterpret_cast<void*>(&presentDetour), p, &why));
        CHECK(inlinehook::install(table[43], reinterpret_cast<void*>(&clearDetour), c, &why));
        g_present = reinterpret_cast<PresentFn>(p.trampoline);
        g_clear = reinterpret_cast<ClearFn>(c.trampoline);
        for (int i = 0; i < 3; i++) {
            CHECK(SUCCEEDED(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(i, 0, 0), 1.0f, 0)));
            CHECK(SUCCEEDED(device->Present(nullptr, nullptr, nullptr, nullptr)));
        }
        CHECK(g_presents == 3 && g_clears == 3);
        CHECK(inlinehook::remove(p) && inlinehook::remove(c));
        CHECK(SUCCEEDED(device->Present(nullptr, nullptr, nullptr, nullptr)) && g_presents == 3);
        printf("d3d9 device: %d methods hooked and unhooked, Present and Clear called through their hooks (first bytes:", hooked + 2);
        for (int i = 0; i < firstCount; i++) printf(" %02X", first[i]);
        printf(")\n");
        device->Release();
    }
    if (window) DestroyWindow(window);
    if (d3d) d3d->Release();
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0); // a crash still shows how far it got
    lengths();
    realDevice();
    const char* why = "";

    // Our hook first, then a second one of ours chained onto its jump (the order of a mod that
    // hooks after us).
    {
        inlinehook::Hook inner, outer;
        CHECK(plusOne(1) == 2);
        CHECK(inlinehook::install(reinterpret_cast<void*>(&plusOne), reinterpret_cast<void*>(&innerDetour), inner, &why));
        g_inner = reinterpret_cast<Fn>(inner.trampoline);
        CHECK(!inner.chained);
        CHECK(plusOne(1) == 102);
        CHECK(inlinehook::install(reinterpret_cast<void*>(&plusOne), reinterpret_cast<void*>(&outerDetour), outer, &why));
        g_outer = reinterpret_cast<Fn>(outer.trampoline);
        CHECK(outer.chained && outer.previous == reinterpret_cast<void*>(&innerDetour));
        CHECK(plusOne(1) == 204);
        CHECK(!inlinehook::intact(inner) && inlinehook::intact(outer));
        // The inner one can't come out while the outer one jumps to it: left as it is.
        CHECK(!inlinehook::remove(inner));
        CHECK(plusOne(1) == 204);
        CHECK(inlinehook::remove(outer));
        CHECK(plusOne(1) == 102);
        CHECK(inlinehook::intact(inner));
        CHECK(inlinehook::remove(inner));
        CHECK(plusOne(1) == 2);
    }

    // Another hooker first, in MinHook's hot-patch form: E9 in the padding above, EB F9 at the
    // entry, its original call continues at the entry + 2. Ours retargets the E9.
    {
        unsigned char* padding = reinterpret_cast<unsigned char*>(&paddedPlusOne);
        unsigned char* entry = padding + 5;
        auto fn = reinterpret_cast<Fn>(entry);
        CHECK(fn(1) == 2);
        unsigned char original[7];
        memcpy(original, padding, 7);
        DWORD old = 0;
        VirtualProtect(padding, 7, PAGE_EXECUTE_READWRITE, &old);
        padding[0] = 0xE9;
        const int rel = static_cast<int>(reinterpret_cast<intptr_t>(&foreignDetour) - reinterpret_cast<intptr_t>(padding + 5));
        memcpy(padding + 1, &rel, 4);
        entry[0] = 0xEB;
        entry[1] = 0xF9;
        FlushInstructionCache(GetCurrentProcess(), padding, 7);
        g_foreignOriginal = reinterpret_cast<Fn>(entry + 2);
        CHECK(fn(1) == 1002);
        inlinehook::Hook ours;
        CHECK(inlinehook::install(entry, reinterpret_cast<void*>(&afterForeignDetour), ours, &why));
        g_afterForeign = reinterpret_cast<Fn>(ours.trampoline);
        CHECK(ours.chained && ours.site == padding && ours.previous == reinterpret_cast<void*>(&foreignDetour));
        CHECK(fn(1) == 1102);
        CHECK(inlinehook::remove(ours));
        CHECK(fn(1) == 1002);
        memcpy(padding, original, 7);
        FlushInstructionCache(GetCurrentProcess(), padding, 7);
        CHECK(fn(1) == 2);
    }

    // Another hooker first with an absolute jump (jmp [slot], the form some hook libraries write):
    // ours chains onto it the same way.
    {
        unsigned char* stub = static_cast<unsigned char*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        CHECK(stub != nullptr);
        if (stub) {
            void* slot = reinterpret_cast<void*>(&foreignDetour);
            const unsigned slotAddress = static_cast<unsigned>(reinterpret_cast<uintptr_t>(stub + 32));
            memcpy(stub + 32, &slot, sizeof(slot));
            stub[0] = 0xFF;
            stub[1] = 0x25;
            memcpy(stub + 2, &slotAddress, 4);
            auto fn = reinterpret_cast<Fn>(stub);
            g_foreignOriginal = reinterpret_cast<Fn>(&plusOne);
            CHECK(fn(1) == 1002);
            inlinehook::Hook ours;
            CHECK(inlinehook::install(stub, reinterpret_cast<void*>(&afterForeignDetour), ours, &why));
            g_afterForeign = reinterpret_cast<Fn>(ours.trampoline);
            CHECK(ours.chained && ours.previous == reinterpret_cast<void*>(&foreignDetour));
            CHECK(fn(1) == 1102);
            CHECK(inlinehook::remove(ours));
            CHECK(fn(1) == 1002);
            VirtualFree(stub, 0, MEM_RELEASE);
        }
    }

    // A short conditional jump among the moved instructions becomes a long one in the trampoline.
    {
        inlinehook::Hook hook;
        CHECK(branchy(0) == 7 && branchy(5) == 3);
        CHECK(inlinehook::install(reinterpret_cast<void*>(&branchy), reinterpret_cast<void*>(&branchyDetour), hook, &why));
        g_branchy = reinterpret_cast<FastFn>(hook.trampoline);
        CHECK(branchy(0) == 17 && branchy(5) == 13);
        CHECK(inlinehook::remove(hook));
        CHECK(branchy(0) == 7 && branchy(5) == 3);
    }

    printf("%s\n", g_failed ? "FAILED" : "all passed");
    return g_failed ? 1 : 0;
}
