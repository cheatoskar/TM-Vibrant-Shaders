#pragma once
#include <windows.h>
#include <functional>
#include <string>

// The F12 capture's files are written on a thread of their own: a capture at 2560x1440 is
// 100-200 MB (the .tmcap, the float images, the screenshot), which took half a second of the
// game's frame, on a busy disk 15 s. The game's thread only reads the targets back (D3D9 calls
// stay on it) and hands the copies over; the writer owns them.
namespace tmshaders {
namespace capwriter {

// Runs `task` on the writer thread, after the ones posted before it (in order). The task owns
// what it captured; it must not touch the device or the game's memory.
void post(std::function<void()> task);

// A task is queued or running: one capture in flight at a time, a new F12 waits for this.
bool busy();

// Waits up to `timeoutMs` until every posted task is done; false = still writing.
bool drain(DWORD timeoutMs);

// Writes `path` under a temporary name (path + ".part") through `write`, renamed to `path` only
// when `write` succeeded: a capture cut off half-way (the game closed) never leaves a broken
// file under the name the previewer opens. Call on the writer thread.
bool writeVia(const std::wstring& path, const std::function<bool(const std::wstring& temporary)>& write);

// Deletes the unfinished files (`pattern`, e.g. L"capture_*.part") an earlier session left in
// `dir`; returns how many.
int removeUnfinished(const std::wstring& dir, const wchar_t* pattern);

// DLL unload: waits for the writer (up to a few seconds). At the game's exit Windows has already
// ended the thread; a file it was writing stays as *.part (the log has no "wrote" line for it)
// and the next session's first capture removes it.
void shutdown();

} // namespace capwriter
} // namespace tmshaders
