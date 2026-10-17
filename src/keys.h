#pragma once
#include <string>
#include <windows.h>

namespace tmshaders {

// The mod's hotkeys: what each does, its default key and the key the player chose (the [Keys]
// section of settings.ini, written as readable names: "F8", "Shift+F6", "Ctrl+Alt+K"). Shared by
// both games; every hotkey check goes through here, nothing else names a key.
namespace keys {

enum class Action : int {
    Menu,       // open and close the menu
    Effects,    // effects on / off (TMNF F7; ManiaPlanet's F7 is its friends list, so F5 there)
    Photo,      // photo mode on / off
    PhotoSplit, // photo mode's compare split on / off
    Capture,    // frame capture (a photo in photo mode; Shift = hi-res)
    Reload,     // reload the shaders (TMNF only)
    Trace,      // arm the trace (TMNF only)
    Count
};

constexpr unsigned kShift = 1, kCtrl = 2, kAlt = 4;

// One key with the modifiers that must be held. vk 0 = no key (the action is off).
struct Binding {
    unsigned vk = 0;
    unsigned mods = 0;
    bool operator==(const Binding& o) const { return vk == o.vk && mods == o.mods; }
    bool operator!=(const Binding& o) const { return !(*this == o); }
};

struct Info {
    const char* name;  // key in the [Keys] section
    const char* label; // what the menu calls it
    Binding def;       // the default
    bool available;    // this game has the action
};
const Info& info(Action a);

Binding get(Action a);
Binding defaultOf(Action a);

// "Shift+F6" <-> Binding. parse() takes any case, "Ctrl"/"Control", "Del"/"Delete" and the like;
// false (out untouched) for text it can't read. "None" (or "") is vk 0.
std::string name(const Binding& b);
bool parse(const char* text, Binding& out);
// Every key name a binding can have, for the test: the vk for index 0..count-1, 0 at the end.
unsigned nameableKey(int index);

// Binds `b` to `a`. A key another action already has is swapped (that action gets a's old key).
// false with note() set when the key can't be used (plain letters would eat the game's own keys:
// they need Ctrl or Alt).
bool assign(Action a, const Binding& b);
void resetDefaults();
// A line about the last change ("swapped with ...", "can't be used: ..."), "" if none.
const std::string& note();
// True once after any change, so the settings get written.
bool takeChanged();

// The action `vk` triggers right now with the modifiers held (the most specific binding wins, so
// Shift+F6 and F6 can both exist). `typing`: a text field has the keyboard, only F keys count.
// Action::Count = none.
Action resolve(unsigned vk, bool typing = false);
// True once when the action's key goes down (for polling every frame). Never while a key is
// being captured.
bool edge(Action a);
// TMNF: every hotkey is read here once a frame, not from the window messages (in the replay
// viewer those reached the menu key seconds late). Each key is read once even when two actions
// share it (F6 and Shift+F6 share its "pressed since the last look" bit); the actions that fired
// wait for take(). `typing`: a text field of the menu has the keyboard, only F keys count.
// Not mixed with edge(): both would read the same "pressed since" bits.
void poll(bool typing);
bool take(Action a);
// An action that came as a window message nobody pressed (scan code 0: dev.ini's key actions,
// posted): polling never sees those, so they wait for take() too.
void push(Action a);
// GetTickCount() when poll() last saw `vk` fire, 0 = never; lastPressAt: of any hotkey.
DWORD polledAt(unsigned vk);
DWORD lastPressAt();
// The game window lost the focus (WM_ACTIVATEAPP): what the keys did until the next poll was
// not meant for us (an F key pressed in another program).
void focusLost();
// A key some action uses (the ManiaPlanet window procedure lets those through to the game).
bool used(unsigned vk);
bool shiftHeld();
// The game's window. Once set, every polled hotkey (edge, shiftHeld) and the held modifiers only
// count while that window (or a window it owns) is in front, so an F key pressed in a browser or
// in the other mod's window does nothing here. Null = no check (the test program).
void setGameWindow(HWND window);
bool gameHasFocus();

// Capturing the next key press for an action (the menu's "Change" button).
void beginCapture(Action a);
void cancelCapture();
bool capturing();
Action captureTarget();
// A window message while capturing: true = consumed, the game must not see it. Esc cancels.
bool feed(UINT msg, WPARAM wparam, LPARAM lparam);

// settings.ini
void load(const wchar_t* ini);
void save(const wchar_t* ini);

} // namespace keys
} // namespace tmshaders
