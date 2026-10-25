#include "misc/internal.h"
#include "menu.h"
#include "imgui.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <vector>

// Finish effects: confetti when a run crosses the finish, and fireworks too for a new best run
// ("Big"; "Subtle" is a short shower from the top). Cosmetic only, in screen space over the game's
// image, drawn at Present with the mod's other windows: photos and the video export never get it
// (both take the image before Present), nor does a replay (the race alone tells a finish).
namespace tmshaders {
namespace misc {
namespace confetti {
namespace {

struct Piece {
    float x, y, vx, vy;    // pixels, pixels/s
    float angle, spin;     // radians, radians/s
    float w, h;            // size (pixels)
    float life, age;       // s
    ImU32 colour;
    bool spark;            // a firework's spark (a glowing dot with a short tail) or a paper piece
};

struct Rocket {
    float x, y, vy, fuse;  // flies up until the fuse is out, then bursts
    ImU32 colour;
};

std::vector<Piece> g_pieces;
std::vector<Rocket> g_rockets;
unsigned g_seed = 1;
LARGE_INTEGER g_last{};

float random01() {
    g_seed = g_seed * 1664525u + 1013904223u;
    return static_cast<float>(g_seed >> 8) / 16777216.0f;
}

float between(float a, float b) {
    return a + (b - a) * random01();
}

ImU32 brightColour() {
    // The mod's accent orange among bright party colours.
    static const ImU32 kColours[] = {IM_COL32(255, 170, 60, 255), IM_COL32(255, 80, 90, 255), IM_COL32(80, 200, 255, 255),
                                     IM_COL32(120, 255, 140, 255), IM_COL32(255, 230, 80, 255), IM_COL32(200, 120, 255, 255),
                                     IM_COL32(255, 255, 255, 255)};
    return kColours[static_cast<int>(random01() * 7.0f) % 7];
}

void burstAt(float x, float y, ImU32 colour, int count, float scale) {
    for (int i = 0; i < count; i++) {
        const float a = between(0.0f, 6.2832f), v = between(120.0f, 420.0f) * scale;
        Piece p{};
        p.x = x;
        p.y = y;
        p.vx = cosf(a) * v;
        p.vy = sinf(a) * v;
        p.w = p.h = between(1.5f, 3.0f) * scale;
        p.life = between(0.9f, 1.6f);
        p.colour = random01() < 0.8f ? colour : IM_COL32(255, 255, 255, 255);
        p.spark = true;
        g_pieces.push_back(p);
    }
}

} // namespace

void start(bool big, bool personalBest) {
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    const float scale = screenScale() * 1440.0f / 1080.0f; // 1 at 1080p, in proportion to the window
    g_seed ^= GetTickCount() | 1u;
    QueryPerformanceCounter(&g_last);
    // Paper from the top (subtle) or from both lower corners up into the screen (big).
    const int pieces = big ? (personalBest ? 260 : 170) : (personalBest ? 110 : 70);
    for (int i = 0; i < pieces; i++) {
        Piece p{};
        if (big) {
            const bool left = i % 2 == 0;
            p.x = left ? between(-10.0f, size.x * 0.08f) : between(size.x * 0.92f, size.x + 10.0f);
            p.y = size.y + 10.0f;
            p.vx = (left ? 1.0f : -1.0f) * between(120.0f, 520.0f) * scale;
            p.vy = -between(650.0f, 1150.0f) * scale;
        } else {
            p.x = between(0.0f, size.x);
            p.y = between(-size.y * 0.25f, -10.0f);
            p.vx = between(-40.0f, 40.0f) * scale;
            p.vy = between(60.0f, 160.0f) * scale;
        }
        p.angle = between(0.0f, 6.2832f);
        p.spin = between(-9.0f, 9.0f);
        p.w = between(5.0f, 10.0f) * scale;
        p.h = p.w * between(0.35f, 0.6f);
        p.life = big ? between(3.0f, 4.5f) : between(2.5f, 3.5f);
        p.colour = brightColour();
        g_pieces.push_back(p);
    }
    // Fireworks for a new best run (big).
    if (big && personalBest) {
        for (int i = 0; i < 5; i++) {
            Rocket r{};
            r.x = size.x * between(0.2f, 0.8f);
            r.y = size.y + 5.0f;
            r.vy = -between(700.0f, 950.0f) * scale;
            r.fuse = between(0.55f, 0.9f) + i * 0.25f;
            r.colour = brightColour();
            g_rockets.push_back(r);
        }
    }
}

bool active() {
    return !g_pieces.empty() || !g_rockets.empty();
}

void draw(const Frame&) {
    if (!active()) return;
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    const float dt = std::clamp(static_cast<float>(now.QuadPart - g_last.QuadPart) / static_cast<float>(frequency.QuadPart), 0.0f, 0.1f);
    g_last = now;
    const float scale = screenScale() * 1440.0f / 1080.0f; // 1 at 1080p, in proportion to the window
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    for (Rocket& r : g_rockets) {
        r.fuse -= dt;
        r.y += r.vy * dt;
        r.vy += 380.0f * scale * dt;
        draw->AddCircleFilled(ImVec2(r.x, r.y), 2.5f * scale, r.colour);
        draw->AddLine(ImVec2(r.x, r.y), ImVec2(r.x, r.y - r.vy * 0.04f), (r.colour & 0x00FFFFFF) | 0x60000000, 1.5f * scale);
        if (r.fuse <= 0.0f) burstAt(r.x, r.y, r.colour, 90, scale);
    }
    g_rockets.erase(std::remove_if(g_rockets.begin(), g_rockets.end(), [](const Rocket& r) { return r.fuse <= 0.0f; }), g_rockets.end());
    for (Piece& p : g_pieces) {
        p.age += dt;
        if (p.spark) {
            p.vy += 260.0f * scale * dt;
            p.vx *= 1.0f - 1.8f * dt;
            p.vy *= 1.0f - 1.8f * dt;
        } else {
            // Paper: gravity, air holding it back, a flutter sideways.
            p.vy += 520.0f * scale * dt;
            p.vx *= 1.0f - 1.2f * dt;
            p.vy = std::min(p.vy * (1.0f - 1.2f * dt), 170.0f * scale);
            p.vx += sinf(p.age * 5.0f + p.spin) * 30.0f * scale * dt;
            p.angle += p.spin * dt;
        }
        p.x += p.vx * dt;
        p.y += p.vy * dt;
        const float fade = std::clamp((p.life - p.age) / 0.6f, 0.0f, 1.0f);
        const ImU32 colour = (p.colour & 0x00FFFFFF) | (static_cast<ImU32>(fade * 255.0f) << 24);
        if (p.spark) {
            draw->AddLine(ImVec2(p.x, p.y), ImVec2(p.x - p.vx * 0.05f, p.y - p.vy * 0.05f), (colour & 0x00FFFFFF) | (static_cast<ImU32>(fade * 110.0f) << 24),
                          p.w);
            draw->AddCircleFilled(ImVec2(p.x, p.y), p.w, colour, 6);
        } else {
            // A turning paper piece: its width shrinks as it flips.
            const float c = cosf(p.angle), s = sinf(p.angle), flip = fabsf(cosf(p.age * 7.0f + p.spin));
            const float hw = p.w * 0.5f, hh = p.h * 0.5f * (0.25f + 0.75f * flip);
            const ImVec2 q[4] = {ImVec2(p.x + (-hw * c - -hh * s), p.y + (-hw * s + -hh * c)), ImVec2(p.x + (hw * c - -hh * s), p.y + (hw * s + -hh * c)),
                                 ImVec2(p.x + (hw * c - hh * s), p.y + (hw * s + hh * c)), ImVec2(p.x + (-hw * c - hh * s), p.y + (-hw * s + hh * c))};
            draw->AddQuadFilled(q[0], q[1], q[2], q[3], colour);
        }
    }
    const float bottom = ImGui::GetIO().DisplaySize.y + 40.0f;
    g_pieces.erase(std::remove_if(g_pieces.begin(), g_pieces.end(), [bottom](const Piece& p) { return p.age >= p.life || (p.y > bottom && p.vy > 0.0f); }),
                   g_pieces.end());
}

} // namespace confetti
} // namespace misc
} // namespace tmshaders
