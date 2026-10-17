#include "capwriter.h"
#include "log.h"
#include <deque>
#include <exception>
#include <new>
#include <utility>

namespace tmshaders {
namespace capwriter {
namespace {

// Plain Win32 objects, no destructors: a std::thread still running at the game's exit would end
// the process with std::terminate when the DLL's statics are destroyed.
SRWLOCK g_lock = SRWLOCK_INIT;
CONDITION_VARIABLE g_work = CONDITION_VARIABLE_INIT; // a task was posted
CONDITION_VARIABLE g_idle = CONDITION_VARIABLE_INIT; // the queue ran empty
std::deque<std::function<void()>>* g_queue = nullptr; // never freed (see above)
bool g_running = false;                                // a task is being run
HANDLE g_thread = nullptr;

DWORD WINAPI writerThread(LPVOID) {
    AcquireSRWLockExclusive(&g_lock);
    for (;;) {
        while (g_queue->empty()) SleepConditionVariableSRW(&g_work, &g_lock, INFINITE, 0);
        std::function<void()> task = std::move(g_queue->front());
        g_queue->pop_front();
        g_running = true;
        ReleaseSRWLockExclusive(&g_lock);
        try {
            task();
        } catch (const std::bad_alloc&) {
            TMVS_LOG("capture: the writer ran out of memory, this capture is incomplete");
        } catch (...) {
            TMVS_LOG("capture: the writer failed, this capture is incomplete");
        }
        task = nullptr; // the capture's copies are freed here, not on the game's thread
        AcquireSRWLockExclusive(&g_lock);
        g_running = false;
        if (g_queue->empty()) WakeAllConditionVariable(&g_idle);
    }
}

} // namespace

void post(std::function<void()> task) {
    AcquireSRWLockExclusive(&g_lock);
    if (!g_queue) g_queue = new std::deque<std::function<void()>>();
    if (!g_thread) g_thread = CreateThread(nullptr, 0, writerThread, nullptr, 0, nullptr);
    if (!g_thread) {
        // No thread: written here, as before.
        ReleaseSRWLockExclusive(&g_lock);
        TMVS_LOG("capture: no writer thread, writing on the game's thread");
        task();
        return;
    }
    g_queue->push_back(std::move(task));
    ReleaseSRWLockExclusive(&g_lock);
    WakeConditionVariable(&g_work);
}

bool busy() {
    AcquireSRWLockShared(&g_lock);
    const bool result = g_running || (g_queue && !g_queue->empty());
    ReleaseSRWLockShared(&g_lock);
    return result;
}

bool drain(DWORD timeoutMs) {
    const DWORD start = GetTickCount();
    AcquireSRWLockExclusive(&g_lock);
    bool idle;
    for (;;) {
        idle = !g_running && (!g_queue || g_queue->empty());
        if (idle) break;
        const DWORD spent = GetTickCount() - start;
        if (spent >= timeoutMs) break;
        SleepConditionVariableSRW(&g_idle, &g_lock, timeoutMs - spent, 0);
    }
    ReleaseSRWLockExclusive(&g_lock);
    return idle;
}

bool writeVia(const std::wstring& path, const std::function<bool(const std::wstring& temporary)>& write) {
    const std::wstring temporary = path + L".part";
    if (!write(temporary)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
}

int removeUnfinished(const std::wstring& dir, const wchar_t* pattern) {
    int removed = 0;
    WIN32_FIND_DATAW found{};
    HANDLE find = FindFirstFileW((dir + L"\\" + pattern).c_str(), &found);
    if (find == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && DeleteFileW((dir + L"\\" + found.cFileName).c_str())) removed++;
    } while (FindNextFileW(find, &found));
    FindClose(find);
    return removed;
}

void shutdown() {
    // At the game's exit the thread is gone already (signalled): nothing to wait for, and the
    // log could be held by it. Only a DLL unload with the thread alive waits.
    if (!g_thread || WaitForSingleObject(g_thread, 0) != WAIT_TIMEOUT) return;
    drain(10000);
}

} // namespace capwriter
} // namespace tmshaders
