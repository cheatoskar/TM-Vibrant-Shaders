#pragma once

// The video export's sound. With the dialog's sound button on, DoShoot replays the clip in real
// time after the frames and records it with OpenAL capture from the system's DEFAULT RECORDING
// device (alcCaptureOpenDevice(NULL, 44100, stereo 16-bit)): a microphone on many PCs, nothing at
// all on others. While an export runs, the game's capture calls (its OpenAL32.dll imports) get a
// device that always opens and delivers the game's own sound instead, from WASAPI loopback
// (src/loopback.cpp); the game still decides when the pass starts and how long it lasts. Outside
// an export, or when no loopback works, the game's own device as before.
namespace tmshaders {
namespace soundcapture {

void install();      // patches the imports (at start-up)
void exportOpened(); // the export's file was made: is its sound on? prepare the loopback early
void exportClosed(); // the export's file is closed: the log line, the loopback released
// While the game's sound pass records: the seconds since its capture started, the clip time it
// plays (the pass plays the clip from 0). False otherwise.
bool passClock(double& seconds);

} // namespace soundcapture
} // namespace tmshaders
