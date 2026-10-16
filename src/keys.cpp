#include "keys.h"
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

namespace tmshaders {
namespace keys {
namespace {

constexpr int kCount = static_cast<int>(Action::Count);

#ifdef TMVS_MANIAPLANET
constexpr bool kTmnf = false;
constexpr unsigned kEffectsKey = VK_F5; // ManiaPlanet's F7 opens the friends list
#else
constexpr bool kTmnf = true;
constexpr unsigned kEffectsKey = VK_F7;
#endif

const Info kInfo[kCount] = {
    {"Menu", "Menu", {VK_F8, 0}, true},
    {"Effects", "Effects on / off", {kEffectsKey, 0}, true},
    {"Photo", "Photo mode", {VK_F6, 0}, true},
    {"PhotoSplit", "Photo mode: compare split", {VK_F6, kShift}, true},
    {"Capture", "Frame capture / photo", {VK_F12, 0}, true},
    {"Reload", "Reload shaders", {VK_F9, 0}, kTmnf},
    {"Trace", "Trace", {VK_F11, 0}, kTmnf},
};

struct Named {
    unsigned vk;
    const char* name;
};
// The keys that are not a letter, digit, F key or numpad digit.
const Named kNamed[] = {
    {VK_BACK, "Backspace"}, {VK_TAB, "Tab"},         {VK_RETURN, "Enter"},   {VK_PAUSE, "Pause"},
    {VK_SPACE, "Space"},    {VK_PRIOR, "PageUp"},    {VK_NEXT, "PageDown"},  {VK_END, "End"},
    {VK_HOME, "Home"},      {VK_LEFT, "Left"},       {VK_UP, "Up"},          {VK_RIGHT, "Right"},
    {VK_DOWN, "Down"},      {VK_INSERT, "Insert"},   {VK_DELETE, "Delete"},  {VK_NUMLOCK, "NumLock"},
    {VK_SCROLL, "ScrollLock"}, {VK_MULTIPLY, "NumMul"}, {VK_ADD, "NumAdd"}, {VK_SUBTRACT, "NumSub"},
    {VK_DECIMAL, "NumDecimal"}, {VK_DIVIDE, "NumDiv"},
    {VK_OEM_1, "Semicolon"}, {VK_OEM_PLUS, "Equals"}, {VK_OEM_COMMA, "Comma"}, {VK_OEM_MINUS, "Minus"},
    {VK_OEM_PERIOD, "Period"}, {VK_OEM_2, "Slash"},  {VK_OEM_3, "Backquote"}, {VK_OEM_4, "LBracket"},
    {VK_OEM_5, "Backslash"}, {VK_OEM_6, "RBracket"}, {VK_OEM_7, "Quote"},
};
// Other spellings parse() takes.
const Named kAlias[] = {
    {VK_BACK, "Back"}, {VK_RETURN, "Return"}, {VK_PRIOR, "PgUp"}, {VK_NEXT, "PgDn"}, {VK_DELETE, "Del"},
    {VK_INSERT, "Ins"}, {VK_SCROLL, "Scroll"}, {VK_PAUSE, "Break"},
};

bool iequals(const char* a, const char* b) {
    for (; *a && *b; a++, b++)
        if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b))) return false;
    return *a == *b;
}

bool isF(unsigned vk) { return vk >= VK_F1 && vk <= VK_F24; }
bool isPad(unsigned vk) { return vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9; }
bool isModifierKey(unsigned vk) {
    return vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
           vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU || vk == VK_LWIN || vk == VK_RWIN;
}

// The name of a key; "" if it has none (then it can't be bound).
std::string keyName(unsigned vk) {
    char buf[16];
    if (isF(vk)) {
        snprintf(buf, sizeof(buf), "F%u", vk - VK_F1 + 1);
        return buf;
    }
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, static_cast<char>(vk));
    if (isPad(vk)) {
        snprintf(buf, sizeof(buf), "Num%u", vk - VK_NUMPAD0);
        return buf;
    }
    for (const Named& n : kNamed)
        if (n.vk == vk) return n.name;
    return std::string();
}

unsigned keyFromName(const char* text) {
    if (!text || !*text) return 0;
    if ((text[0] == 'F' || text[0] == 'f') && std::isdigit(static_cast<unsigned char>(text[1]))) {
        char* end = nullptr;
        const long n = strtol(text + 1, &end, 10);
        if (end && !*end && n >= 1 && n <= 24) return static_cast<unsigned>(VK_F1 + n - 1);
    }
    if (!text[1] && std::isalnum(static_cast<unsigned char>(text[0]))) return static_cast<unsigned>(std::toupper(static_cast<unsigned char>(text[0])));
    if ((text[0] == 'N' || text[0] == 'n') && (text[1] == 'u' || text[1] == 'U') && (text[2] == 'm' || text[2] == 'M') && text[3] >= '0' &&
        text[3] <= '9' && !text[4])
        return static_cast<unsigned>(VK_NUMPAD0 + (text[3] - '0'));
    for (const Named& n : kNamed)
        if (iequals(text, n.name)) return n.vk;
    for (const Named& n : kAlias)
        if (iequals(text, n.name)) return n.vk;
    return 0;
}

// Keys that don't get in the way of the game's own (and of typing) without Ctrl or Alt.
bool plainOk(unsigned vk) {
    return isF(vk) || isPad(vk) || (vk >= VK_MULTIPLY && vk <= VK_DIVIDE) || vk == VK_PAUSE || vk == VK_SCROLL || vk == VK_NUMLOCK ||
           vk == VK_INSERT || vk == VK_DELETE || vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT;
}

Binding g_binding[kCount];
bool g_init = false;
bool g_changed = false;
std::string g_note;
bool g_capturing = false;
Action g_target = Action::Count;

void ensure() {
    if (g_init) return;
    g_init = true;
    for (int i = 0; i < kCount; i++) g_binding[i] = kInfo[i].def;
}

unsigned modsHeld() {
    unsigned m = 0;
    if (GetAsyncKeyState(VK_SHIFT) < 0) m |= kShift;
    if (GetAsyncKeyState(VK_CONTROL) < 0) m |= kCtrl;
    if (GetAsyncKeyState(VK_MENU) < 0) m |= kAlt;
    return m;
}

int bits(unsigned m) {
    return (m & 1) + ((m >> 1) & 1) + ((m >> 2) & 1);
}

Action resolveWith(unsigned vk, unsigned held, bool typing) {
    ensure();
    if (!vk || (typing && !isF(vk))) return Action::Count;
    Action best = Action::Count;
    int bestBits = -1;
    for (int i = 0; i < kCount; i++) {
        if (!kInfo[i].available) continue;
        const Binding& b = g_binding[i];
        if (b.vk != vk || (b.mods & ~held) != 0) continue;
        if (bits(b.mods) > bestBits) {
            best = static_cast<Action>(i);
            bestBits = bits(b.mods);
        }
    }
    return best;
}

} // namespace

const Info& info(Action a) {
    return kInfo[static_cast<int>(a)];
}

Binding get(Action a) {
    ensure();
    return g_binding[static_cast<int>(a)];
}

Binding defaultOf(Action a) {
    return kInfo[static_cast<int>(a)].def;
}

std::string name(const Binding& b) {
    if (!b.vk) return "None";
    const std::string key = keyName(b.vk);
    if (key.empty()) return "None";
    std::string s;
    if (b.mods & kCtrl) s += "Ctrl+";
    if (b.mods & kAlt) s += "Alt+";
    if (b.mods & kShift) s += "Shift+";
    return s + key;
}

bool parse(const char* text, Binding& out) {
    if (!text) return false;
    std::string s(text);
    // Trim.
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) start++;
    s.erase(0, start);
    if (s.empty() || iequals(s.c_str(), "None")) {
        out = Binding{};
        return true;
    }
    Binding b;
    size_t pos = 0;
    for (;;) {
        const size_t plus = s.find('+', pos);
        std::string token = s.substr(pos, plus == std::string::npos ? std::string::npos : plus - pos);
        while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back()))) token.pop_back();
        size_t t = 0;
        while (t < token.size() && std::isspace(static_cast<unsigned char>(token[t]))) t++;
        token.erase(0, t);
        if (token.empty()) return false;
        if (plus == std::string::npos) {
            b.vk = keyFromName(token.c_str());
            if (!b.vk) return false;
            break;
        }
        if (iequals(token.c_str(), "Shift")) b.mods |= kShift;
        else if (iequals(token.c_str(), "Ctrl") || iequals(token.c_str(), "Control")) b.mods |= kCtrl;
        else if (iequals(token.c_str(), "Alt")) b.mods |= kAlt;
        else return false;
        pos = plus + 1;
    }
    out = b;
    return true;
}

unsigned nameableKey(int index) {
    unsigned n = 0;
    for (unsigned vk = 1; vk < 256; vk++) {
        if (keyName(vk).empty()) continue;
        if (static_cast<int>(n++) == index) return vk;
    }
    return 0;
}

bool assign(Action a, const Binding& b) {
    ensure();
    const int ai = static_cast<int>(a);
    g_note.clear();
    if (b.vk) {
        if (keyName(b.vk).empty()) {
            g_note = "That key can't be used.";
            return false;
        }
        if (!plainOk(b.vk) && !(b.mods & (kCtrl | kAlt))) {
            g_note = "Letters, digits and the game's own keys need Ctrl or Alt (else they stop working in the game). F keys, Insert, Delete, Home, End and the number pad work alone.";
            return false;
        }
    }
    const Binding old = g_binding[ai];
    if (b == old) return true;
    if (b.vk) {
        for (int i = 0; i < kCount; i++) {
            if (i == ai || !kInfo[i].available || g_binding[i] != b) continue;
            g_binding[i] = old; // swap: the other one gets this one's old key
            g_note = std::string(kInfo[i].label) + " had " + name(b) + ": it now has " + name(old) + ".";
        }
    }
    g_binding[ai] = b;
    g_changed = true;
    return true;
}

void resetDefaults() {
    ensure();
    for (int i = 0; i < kCount; i++) g_binding[i] = kInfo[i].def;
    g_note.clear();
    g_changed = true;
}

const std::string& note() {
    return g_note;
}

bool takeChanged() {
    const bool c = g_changed;
    g_changed = false;
    return c;
}

Action resolve(unsigned vk, bool typing) {
    return resolveWith(vk, modsHeld(), typing);
}

bool edge(Action a) {
    ensure();
    static bool was[kCount] = {};
    const int i = static_cast<int>(a);
    const unsigned vk = g_binding[i].vk;
    if (!vk || !kInfo[i].available) {
        was[i] = false;
        return false;
    }
    const SHORT state = GetAsyncKeyState(static_cast<int>(vk));
    const bool down = (state & 0x8000) != 0;
    // A tap between two frames still counts (the low bit says it went down since the last look).
    const bool fire = (down && !was[i]) || (state & 1) != 0;
    was[i] = down;
    return fire && !g_capturing && resolve(vk) == a;
}

bool used(unsigned vk) {
    ensure();
    for (int i = 0; i < kCount; i++)
        if (kInfo[i].available && g_binding[i].vk == vk) return true;
    return false;
}

bool shiftHeld() {
    return GetAsyncKeyState(VK_SHIFT) < 0;
}

void beginCapture(Action a) {
    g_capturing = true;
    g_target = a;
    g_note.clear();
}

void cancelCapture() {
    g_capturing = false;
    g_target = Action::Count;
}

bool capturing() {
    return g_capturing;
}

Action captureTarget() {
    return g_target;
}

bool feed(UINT msg, WPARAM wparam, LPARAM lparam) {
    if (!g_capturing) return false;
    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        if (lparam & (1 << 30)) return true; // key repeat
        const unsigned vk = static_cast<unsigned>(wparam);
        if (isModifierKey(vk)) return true; // wait for the key itself
        if (vk == VK_ESCAPE) {
            cancelCapture();
            return true;
        }
        Binding b;
        b.vk = vk;
        b.mods = modsHeld();
        // Only the capture ends on a key that works; else the player tries again.
        if (assign(g_target, b)) cancelCapture();
        return true;
    }
    case WM_KEYUP:
    case WM_SYSKEYUP:
    case WM_CHAR:
    case WM_SYSCHAR:
    case WM_DEADCHAR:
        return true;
    default:
        return false;
    }
}

void load(const wchar_t* ini) {
    ensure();
    for (int i = 0; i < kCount; i++) {
        wchar_t text[64] = {};
        char narrow[64] = {};
        g_binding[i] = kInfo[i].def;
        wchar_t key[32] = {};
        for (int c = 0; kInfo[i].name[c] && c < 31; c++) key[c] = static_cast<wchar_t>(kInfo[i].name[c]);
        if (!GetPrivateProfileStringW(L"Keys", key, L"", text, 64, ini) || !text[0]) continue;
        for (int c = 0; text[c] && c < 63; c++) narrow[c] = text[c] < 128 ? static_cast<char>(text[c]) : '?';
        Binding b;
        if (parse(narrow, b)) g_binding[i] = b;
    }
    // Two actions on one key (edited by hand): the first keeps it, the later ones go back to
    // their default (or off if that is taken too).
    for (int i = 0; i < kCount; i++) {
        for (int j = 0; j < i; j++) {
            if (!kInfo[i].available || !kInfo[j].available || !g_binding[i].vk || g_binding[i] != g_binding[j]) continue;
            g_binding[i] = kInfo[i].def;
            for (int k = 0; k < kCount; k++)
                if (k != i && kInfo[k].available && g_binding[k] == g_binding[i]) g_binding[i] = Binding{};
        }
    }
}

void save(const wchar_t* ini) {
    ensure();
    for (int i = 0; i < kCount; i++) {
        if (!kInfo[i].available) continue;
        wchar_t key[32] = {}, value[64] = {};
        for (int c = 0; kInfo[i].name[c] && c < 31; c++) key[c] = static_cast<wchar_t>(kInfo[i].name[c]);
        const std::string text = name(g_binding[i]);
        for (size_t c = 0; c < text.size() && c < 63; c++) value[c] = static_cast<wchar_t>(text[c]);
        WritePrivateProfileStringW(L"Keys", key, value, ini);
    }
}

} // namespace keys
} // namespace tmshaders
