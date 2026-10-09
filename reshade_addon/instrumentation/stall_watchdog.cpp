// stall_watchdog.cpp -- see device_watch.h.
#include "../addon_common.h"
#include "device_watch.h"
#include "../mods/ib_hash.h"
#include "removal_pin.h"             // DumpOnStall -- the close / execute rings (a failed Close asserts the engine)
#include "../migoto/dump_watchdog.h" // LogThreadStack -- the suspend-and-unwind stack of a thread
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace {
std::atomic<uint32_t>      g_ticks{ 0 };
std::atomic<unsigned long> g_present_tid{ 0 };
std::atomic<bool>          g_focused{ false };
std::atomic<bool>          g_started{ false };
std::atomic<bool>          g_stop{ false };
struct WatchdogThread {
    std::thread t;
    ~WatchdogThread()
    {
        if (t.joinable()) t.detach();
    }
} g_thread;
std::mutex g_thread_mtx; // start (present thread) vs stop (the destroy / uninit thread)
}

void StallWatchdogTick(bool focused)
{
    g_present_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    g_focused.store(focused, std::memory_order_relaxed);
    g_ticks.fetch_add(1, std::memory_order_release);
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true)) return;
    std::lock_guard<std::mutex> lk(g_thread_mtx);
    if (g_stop.load(std::memory_order_acquire)) return; // stopped before it could start (teardown)
    g_thread.t = std::thread([] {
        using clock = std::chrono::steady_clock;
        uint32_t          last = g_ticks.load(std::memory_order_acquire);
        clock::time_point since = clock::now();
        bool              stalled = false;
        unsigned          stalls = 0;
        uint32_t          stall_frame = 0;
        while (!g_stop.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (g_stop.load(std::memory_order_acquire)) break;
            const uint32_t now = g_ticks.load(std::memory_order_acquire);
            if (now != last) {
                if (stalled) {
                    const unsigned ms = (unsigned)std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - since).count();
                    logger::report("STALL #%u ended: presents resumed after %u ms (frame %u -> %u)", stalls, ms, stall_frame, g_frame_id);
                    stalled = false;
                }
                last = now;
                since = clock::now();
                continue;
            }
            if (!g_stall_watchdog || stalled) continue;
            // Not while the game sits in the background: it presents nothing there, and that is not a stall.
            if (!g_focused.load(std::memory_order_relaxed)) {
                since = clock::now();
                continue;
            }
            const unsigned ms = (unsigned)std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - since).count();
            if (ms < g_stall_ms) continue;
            stalled = true;
            ++stalls;
            stall_frame = g_frame_id;
            g_stall_count.fetch_add(1, std::memory_order_relaxed);
            if (stalls > 32 && (stalls % 64) != 0) continue;
            const unsigned long tid = g_present_tid.load(std::memory_order_relaxed);
            logger::report("STALL #%u: no present for %u ms (frame %u, threshold %u ms) -- the present thread %lu:",
                stalls, ms, stall_frame, g_stall_ms, tid);
            MigotoPort::LogThreadStack(tid, "STALL");
            IbHash::LogInflight("stall");
            RemovalPin::DumpOnStall(tid);
        }
    });
    logger::report("STALL watchdog started (threshold %u ms, 100 ms poll)", g_stall_ms);
}

void StallWatchdogStop()
{
    g_stop.store(true, std::memory_order_release);
    std::lock_guard<std::mutex> lk(g_thread_mtx);
    if (g_thread.t.joinable()) {
        g_thread.t.join();
        logger::report("STALL watchdog stopped (device destroyed / addon unloading)");
    }
}
