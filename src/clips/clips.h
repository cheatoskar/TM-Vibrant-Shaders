#pragma once
#include <d3d9.h>
#include <string>

// Instant replay clips ("TM Clips"): the game is encoded into a buffer in memory all the time, and a
// run (its start minus a few seconds to its finish plus a few) is saved as an MP4 with the game's
// sound right after the finish, or the last seconds on a key. Nothing is rendered again, no replay is
// saved, nothing in the game is changed.
//
// One code base, two hosts:
//  - TM Vibrant Shaders (Host::Tmvs): its Present calls present(), its menu shows the settings
//    (Clips page), its overlay draws the message. With "Without HUD" the picture is the scene it
//    shaded before the HUD (sceneDone()).
//  - TM Clips, the standalone mod (Host::Standalone, src/clips/standalone): its own Present hook and
//    a small window of its own.
// Both installed: TM Vibrant Shaders records, TM Clips stays idle, whichever loads first. TM Vibrant
// Shaders holds the named mutex Local\TMClips_Host_<pid> and a counter in the named mapping
// Local\TMClips_Beat_<pid> (both made in boot()), counted up in every present(). TM Clips looks for
// the mutex before it hooks anything (found: no hooks at all); once hooked it looks for the mapping
// every frame: there, it doesn't record (from the first frame on, so the two never record at once)
// and waits for the counter; it records again only when the counter stood still for 300 of its
// frames (TM Vibrant Shaders' clips don't run). Each says in its log who records.
//
// TrackMania Nations ESWC: in TM Vibrant Shaders (its d3d9.dll), the race read as race.h says.
// Not yet: TrackMania 2 (maniaplanet/: a D3D11 grabber, the race from ManiaPlanet's app, the menu's host).
namespace tmshaders {
namespace clips {

enum class Host { Tmvs, Standalone };

void boot(Host host);
// The standalone: TM Vibrant Shaders (with clips) runs in this process.
bool tmvsRecords();
// The standalone found TM Vibrant Shaders and stays idle (stopped recording).
bool idle();

// Every Present, on the device's thread, before anything of the host's own is drawn over the game
// (menus, messages): this is the picture that is recorded.
void present(IDirect3DDevice9* device);
// TM Vibrant Shaders: the game's frame is finished (its HUD drawn) and about to be presented. Another
// mod's Present hook in front of ours (Horizon Host's menu and bar) has drawn nothing yet, so with the
// HUD the picture is taken here; present() takes it only in frames without this call.
void frameEnd(IDirect3DDevice9* device);
// The back buffer at present() carries another mod's interface (Horizon's, in a frame without
// frameEnd): such frames are left out unless "Without HUD" took the scene.
void setPresentCovered(bool covered);
// TM Vibrant Shaders: the scene is shaded, the HUD not drawn yet (with "Without HUD" the picture is
// taken here).
bool wantsScene();
void sceneDone(IDirect3DDevice9* device, IDirect3DSurface9* target);
// The host's video export runs: no recording meanwhile.
void setPaused(bool paused);
void setInternalCalls(void (*begin)(), void (*end)());

void preReset();
void postReset();
// At the end (not at process exit: the threads are gone then).
void shutdown();

// The UI, between ImGui::NewFrame and Render. drawPanel: all of it (the standalone's window);
// TM Vibrant Shaders' Clips page shows its two halves side by side: drawSaving (save on finish, the
// keys, the last runs, the last clips) and drawRecording (buffer, size, encoder, sound).
void drawPanel();
void drawSaving();
void drawRecording();
// The "Clip saved" message (fades): drawn when toastActive().
bool toastActive();
void drawToast();
// The standalone's window key (settings: MenuKey).
std::string menuKey();

} // namespace clips
} // namespace tmshaders
