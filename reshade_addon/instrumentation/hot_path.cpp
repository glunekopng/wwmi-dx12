#include "../addon_common.h"
#include "hot_path.h"
#include "profiler.h"
#include <TraceLoggingProvider.h>
#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

TRACELOGGING_DEFINE_PROVIDER(g_hp_etw, "WWMI-DX12-HotPath",
    (0x3ab0fca7, 0xf037, 0x4eee, 0xb9, 0x1d, 0x56, 0x81, 0xd8, 0xb5, 0x10, 0xe1));

namespace HotPath {

struct ListRec {
    uint32_t reset_tid = 0;
    uint32_t close_tid = 0;
    int64_t  t_reset = 0;
    int64_t  t_close = 0;
    uint64_t ours_ns = 0;
    uint32_t claimed = 0;
    uint64_t hook_ns[HK_COUNT] = {};
    uint32_t hook_n[HK_COUNT] = {};
    uint64_t prof_ns[kProfMaxBuckets] = {};
};

struct Rec {
    ListRec cur;
    ListRec last;
    GpuRec* gpu = nullptr;
};

GpuRec*& GpuOf(Rec* r)
{
    return r->gpu;
}

namespace {

const char* const kHookNames[HK_COUNT] = {
#define WWMI_HP_NAME(n) #n,
    WWMI_HP_HOOKS(WWMI_HP_NAME)
#undef WWMI_HP_NAME
};

int64_t Now()
{
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return li.QuadPart;
}

double MsOf(int64_t ticks)
{
    static const double ms_per_tick = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1e3 / (double)f.QuadPart;
    }();
    return (double)ticks * ms_per_tick;
}

struct ThreadTot {
    uint64_t in_list_ns = 0;
    uint64_t out_list_ns = 0;
    int64_t  span = 0;
    uint32_t lists = 0;
    uint32_t draws = 0;
    uint32_t claimed = 0;
    uint32_t submits = 0;
    uint32_t waited_for = 0;
};

struct Exec {
    uint64_t list;
    uint64_t queue;
    uint32_t tid;
    int64_t  t;
};

struct OrdinalAgg {
    uint32_t                     n = 0;
    double                       at_ms = 0.0;
    double                       lists = 0.0;
    uint32_t                     waited_n = 0;
    double                       span_ms = 0.0;
    double                       ours_ms = 0.0;
    double                       slack_ms = 0.0;
    double                       claimed = 0.0;
    double                       draws = 0.0;
    std::map<uint32_t, uint32_t> waited_tid;
    double                       own_n = 0.0;
    double                       own_span_ms = 0.0;
    double                       own_ours_ms = 0.0;
};

std::mutex                                         g_mtx;
std::unordered_map<uint64_t, std::unique_ptr<Rec>> g_lists;
std::vector<Exec>                                  g_execs;
std::map<uint32_t, ThreadTot>                      g_threads;
std::vector<OrdinalAgg>                            g_ord;
uint64_t                                           g_wait_hook_ns[HK_COUNT] = {};
uint64_t                                           g_wait_prof_ns[kProfMaxBuckets] = {};
double                                             g_wait_span_ms = 0.0;
double                                             g_wait_ours_ms = 0.0;
uint64_t                                           g_all_hook_ns[HK_COUNT] = {};
uint64_t                                           g_all_hook_n[HK_COUNT] = {};
uint32_t                                           g_no_record = 0;
uint32_t                                           g_closed_late = 0;
uint32_t                                           g_no_reset = 0;
uint32_t                                           g_cross_thread = 0;
int64_t                                            g_frame_start = 0;
uint32_t                                           g_frame_no = 0;

thread_local uint64_t t_last_list = 0;
thread_local Rec*     t_last_rec = nullptr;
thread_local uint64_t t_out_ns = 0;
thread_local uint64_t t_out_hook_ns[HK_COUNT] = {};
thread_local uint32_t t_out_hook_n[HK_COUNT] = {};

void FlushOutLocked(uint32_t tid)
{
    g_threads[tid].out_list_ns += t_out_ns;
    t_out_ns = 0;
    for (int h = 0; h < HK_COUNT; ++h) {
        g_all_hook_ns[h] += t_out_hook_ns[h];
        g_all_hook_n[h] += t_out_hook_n[h];
        t_out_hook_ns[h] = 0;
        t_out_hook_n[h] = 0;
    }
}

void ProfSink(int bucket, uint64_t self_ns)
{
    if (!g_on.load(std::memory_order_relaxed) || !t_rec) return;
    if (bucket >= 0 && bucket < kProfMaxBuckets) t_rec->cur.prof_ns[bucket] += self_ns;
}

} // namespace

Rec* RecFor(reshade::api::command_list* cl)
{
    const uint64_t key = reinterpret_cast<uint64_t>(cl);
    if (key == t_last_list) return t_last_rec;
    Rec* r;
    {
        std::lock_guard       l(g_mtx);
        std::unique_ptr<Rec>& p = g_lists[key];
        if (!p) p = std::make_unique<Rec>();
        r = p.get();
    }
    t_last_list = key;
    t_last_rec = r;
    return r;
}

void HookDone(int hook, uint64_t self_ns, Rec* rec)
{
    if (!rec) {
        t_out_ns += self_ns;
        t_out_hook_ns[hook] += self_ns;
        ++t_out_hook_n[hook];
        return;
    }
    rec->cur.hook_ns[hook] += self_ns;
    ++rec->cur.hook_n[hook];
    rec->cur.ours_ns += self_ns;
}

void Init()
{
    static bool s_done = false;
    if (s_done) return;
    s_done = true;
    TraceLoggingRegister(g_hp_etw);
    g_prof_self_sink = &ProfSink;
}

void OnReset(reshade::api::command_list* cl)
{
    if (!g_on.load(std::memory_order_relaxed) || !cl) return;
    Rec* r = RecFor(cl);
    r->cur = ListRec{};
    r->cur.reset_tid = GetCurrentThreadId();
    r->cur.t_reset = Now();
    GpuListReset(r);
    TraceLoggingWrite(g_hp_etw, "ListReset", TraceLoggingPointer(cl, "list"));
}

void NoteClaimed()
{
    if (t_rec) ++t_rec->cur.claimed;
}

void OnClose(reshade::api::command_list* cl)
{
    if (!g_on.load(std::memory_order_relaxed) || !cl) return;
    Rec*     r = RecFor(cl);
    ListRec& c = r->cur;
    c.t_close = Now();
    c.close_tid = GetCurrentThreadId();
    TraceLoggingWrite(g_hp_etw, "ListClose", TraceLoggingPointer(cl, "list"),
        TraceLoggingUInt32((uint32_t)(c.ours_ns / 1000), "ours_us"),
        TraceLoggingUInt32(c.hook_n[HK_draw_indexed], "draws"),
        TraceLoggingUInt32(c.claimed, "claimed"),
        TraceLoggingUInt32(c.hook_n[HK_dispatch], "dispatches"));
    GpuListClose(r, cl, c.claimed);
    std::lock_guard l(g_mtx);
    FlushOutLocked(c.close_tid);
    if (!c.t_reset) {
        ++g_no_reset;
        c.reset_tid = c.close_tid;
        c.t_reset = c.t_close;
    }
    if (c.reset_tid != c.close_tid) ++g_cross_thread;
    ThreadTot& t = g_threads[c.close_tid];
    t.in_list_ns += c.ours_ns;
    t.span += c.t_close - c.t_reset;
    ++t.lists;
    t.draws += c.hook_n[HK_draw_indexed];
    t.claimed += c.claimed;
    for (int h = 0; h < HK_COUNT; ++h) {
        g_all_hook_ns[h] += c.hook_ns[h];
        g_all_hook_n[h] += c.hook_n[h];
    }
    r->last = c;
}

void OnExecute(reshade::api::command_queue* queue, reshade::api::command_list* cl)
{
    if (!g_on.load(std::memory_order_relaxed) || !cl) return;
    TraceLoggingWrite(g_hp_etw, "Execute", TraceLoggingPointer(cl, "list"), TraceLoggingPointer(queue, "queue"));
    GpuExecuted(RecFor(cl), queue);
    const uint32_t  tid = GetCurrentThreadId();
    std::lock_guard l(g_mtx);
    FlushOutLocked(tid);
    g_execs.push_back(Exec{ reinterpret_cast<uint64_t>(cl), reinterpret_cast<uint64_t>(queue), tid, Now() });
}

void SessionStart()
{
    std::lock_guard l(g_mtx);
    g_threads.clear();
    g_ord.clear();
    for (uint64_t& v : g_wait_hook_ns) v = 0;
    for (uint64_t& v : g_wait_prof_ns) v = 0;
    for (uint64_t& v : g_all_hook_ns) v = 0;
    for (uint64_t& v : g_all_hook_n) v = 0;
    g_wait_span_ms = 0.0;
    g_wait_ours_ms = 0.0;
    g_no_record = 0;
    g_closed_late = 0;
    g_no_reset = 0;
    g_cross_thread = 0;
}

void FrameEnd(bool dump_active)
{
    const bool was_on = g_on.load(std::memory_order_relaxed);
    const bool etw = TraceLoggingProviderEnabled(g_hp_etw, 0, 0);
    const bool on = dump_active || etw;
    g_on.store(on, std::memory_order_relaxed);
    g_gpu_on.store(etw, std::memory_order_relaxed);
    g_gpu_detail.store(etw && TraceLoggingProviderEnabled(g_hp_etw, 0, 0x2), std::memory_order_relaxed);
    const int64_t now = Now();
    if (!was_on) {
        std::lock_guard l(g_mtx);
        g_execs.clear();
        g_frame_start = now;
        return;
    }
    TraceLoggingWrite(g_hp_etw, "Frame", TraceLoggingUInt32(++g_frame_no, "frame"));
    std::vector<Exec> ex;
    std::lock_guard   l(g_mtx);
    FlushOutLocked(GetCurrentThreadId());
    ex.swap(g_execs);
    std::sort(ex.begin(), ex.end(), [](const Exec& a, const Exec& b) { return a.t < b.t; });
    const int64_t gap = (int64_t)(0.1 / MsOf(1));
    size_t        ord = 0;
    for (size_t i = 0; i < ex.size();) {
        size_t j = i + 1;
        while (j < ex.size() && ex[j].tid == ex[i].tid && ex[j].queue == ex[i].queue && ex[j].t - ex[j - 1].t < gap) ++j;
        const uint32_t submitter = ex[i].tid;
        const ListRec* waited = nullptr;
        uint32_t       lists = 0;
        uint32_t       own_n = 0;
        double         own_span = 0.0;
        double         own_ours = 0.0;
        for (size_t k = i; k < j; ++k) {
            auto it = g_lists.find(ex[k].list);
            if (it == g_lists.end() || !it->second->last.t_close) {
                ++g_no_record;
                continue;
            }
            const ListRec& r = it->second->last;
            if (r.t_close > ex[k].t) {
                ++g_closed_late;
                continue;
            }
            ++lists;
            if (r.close_tid == submitter) {
                ++own_n;
                own_span += MsOf(r.t_close - r.t_reset);
                own_ours += (double)r.ours_ns / 1e6;
                continue;
            }
            if (!waited || r.t_close > waited->t_close) waited = &r;
        }
        ++g_threads[submitter].submits;
        if (g_ord.size() <= ord) g_ord.resize(ord + 1);
        OrdinalAgg& o = g_ord[ord];
        ++o.n;
        o.at_ms += MsOf(ex[i].t - g_frame_start);
        o.lists += lists;
        o.own_n += own_n;
        o.own_span_ms += own_span;
        o.own_ours_ms += own_ours;
        if (waited) {
            ++o.waited_n;
            o.span_ms += MsOf(waited->t_close - waited->t_reset);
            o.ours_ms += (double)waited->ours_ns / 1e6;
            o.slack_ms += MsOf(ex[i].t - waited->t_close);
            o.claimed += waited->claimed;
            o.draws += waited->hook_n[HK_draw_indexed];
            ++o.waited_tid[waited->close_tid];
            ++g_threads[waited->close_tid].waited_for;
            for (int h = 0; h < HK_COUNT; ++h) g_wait_hook_ns[h] += waited->hook_ns[h];
            for (int b = 0; b < kProfMaxBuckets; ++b) g_wait_prof_ns[b] += waited->prof_ns[b];
            g_wait_span_ms += MsOf(waited->t_close - waited->t_reset);
            g_wait_ours_ms += (double)waited->ours_ns / 1e6;
        }
        ++ord;
        i = j;
    }
    g_frame_start = now;
}

void Report(FILE* f, int frames)
{
    if (!f || frames <= 0) return;
    std::lock_guard l(g_mtx);
    const double    fr = (double)frames;
    std::fprintf(f, "\nHOT PATH -- per submission (a run of ExecuteCommandLists entries on one thread and queue), in frame order.\n");
    std::fprintf(f, "  WAITED-FOR = of the submitted lists recorded on OTHER threads, the one that closed last: span = its Reset\n");
    std::fprintf(f, "  to Close, ours = the add-on's handlers inside it, slack = its Close to the submit. OWN = lists the\n");
    std::fprintf(f, "  submitting thread recorded itself (serial on its path). Averages over the frames the submission was seen in.\n");
    std::fprintf(f, "  %3s %5s %7s %5s | %5s %8s %8s %8s %7s %6s %-22s | %5s %8s %8s\n", "#", "seen", "at ms", "lists",
        "has", "span ms", "ours ms", "slack ms", "claimed", "draws", "waited-for thread", "own", "span ms", "ours ms");
    for (size_t k = 0; k < g_ord.size(); ++k) {
        const OrdinalAgg& o = g_ord[k];
        if (!o.n) continue;
        const double                               n = (double)o.n;
        const double                               wn = o.waited_n ? (double)o.waited_n : 1.0;
        std::vector<std::pair<uint32_t, uint32_t>> ts(o.waited_tid.begin(), o.waited_tid.end());
        std::sort(ts.begin(), ts.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        char   who[64] = "";
        size_t at = 0;
        for (size_t t = 0; t < ts.size() && t < 2 && at < sizeof(who); ++t)
            at += (size_t)std::snprintf(who + at, sizeof(who) - at, "%u(%.0f%%) ", ts[t].first, 100.0 * ts[t].second / wn);
        std::fprintf(f, "  %3zu %5.2f %7.2f %5.1f | %4.0f%% %8.3f %8.3f %8.3f %7.1f %6.0f %-22s | %5.1f %8.3f %8.3f\n", k,
            n / fr, o.at_ms / n, o.lists / n, 100.0 * o.waited_n / n, o.span_ms / wn, o.ours_ms / wn,
            o.slack_ms / wn, o.claimed / wn, o.draws / wn, who, o.own_n / n, o.own_span_ms / n, o.own_ours_ms / n);
    }
    std::fprintf(f, "  waited-for lists, per frame: span %.3f ms, ours %.3f ms\n", g_wait_span_ms / fr, g_wait_ours_ms / fr);
    std::fprintf(f, "  executed lists unmatched: %.1f/frame with no closed record, %.1f/frame closed after the submit; "
                    "%.1f/frame closed with no Reset seen, %.1f/frame reset and closed on different threads\n",
        g_no_record / fr, g_closed_late / fr, g_no_reset / fr, g_cross_thread / fr);

    std::fprintf(f, "\n  ours inside the waited-for lists, by handler (ms/frame):\n");
    std::vector<std::pair<double, int>> rows;
    for (int h = 0; h < HK_COUNT; ++h)
        if (g_wait_hook_ns[h]) rows.push_back({ (double)g_wait_hook_ns[h] / 1e6 / fr, h });
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& r : rows) std::fprintf(f, "    %-28s %8.3f\n", kHookNames[r.second], r.first);

    std::fprintf(f, "\n  ours inside the waited-for lists, by profiler row (self ms/frame; rows only while the profiler times):\n");
    rows.clear();
    for (int b = 0; b < PROF_COUNT; ++b)
        if (g_wait_prof_ns[b]) rows.push_back({ (double)g_wait_prof_ns[b] / 1e6 / fr, b });
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (size_t i = 0; i < rows.size() && i < 25; ++i) {
        const char* nm = g_prof_names[rows[i].second];
        while (*nm == ' ') ++nm;
        std::fprintf(f, "    %-28s %8.3f\n", nm, rows[i].first);
    }

    std::fprintf(f, "\n  every handler, all threads (one timestamp pair per call, ~25 ns of it the timer itself):\n");
    std::fprintf(f, "    %-28s %9s %9s %8s\n", "handler", "ms/frame", "calls/fr", "ns/call");
    rows.clear();
    for (int h = 0; h < HK_COUNT; ++h)
        if (g_all_hook_n[h]) rows.push_back({ (double)g_all_hook_ns[h] / 1e6 / fr, h });
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& r : rows)
        std::fprintf(f, "    %-28s %9.3f %9.0f %8.0f\n", kHookNames[r.second], r.first, (double)g_all_hook_n[r.second] / fr,
            (double)g_all_hook_ns[r.second] / (double)g_all_hook_n[r.second]);

    std::fprintf(f, "\n  per thread, per frame (lists by the thread that closed them):\n");
    std::fprintf(f, "  %8s %6s %9s %9s %9s %7s %7s %8s %10s\n", "tid", "lists", "span ms", "ours-in", "ours-out",
        "draws", "claimed", "submits", "waited-for");
    std::vector<std::pair<uint32_t, ThreadTot>> th(g_threads.begin(), g_threads.end());
    std::sort(th.begin(), th.end(), [](const auto& a, const auto& b) { return a.second.in_list_ns + a.second.out_list_ns > b.second.in_list_ns + b.second.out_list_ns; });
    for (size_t i = 0; i < th.size() && i < 24; ++i) {
        const ThreadTot& t = th[i].second;
        std::fprintf(f, "  %8u %6.1f %9.3f %9.3f %9.3f %7.0f %7.1f %8.2f %10.2f\n", th[i].first, t.lists / fr,
            MsOf(t.span) / fr, (double)t.in_list_ns / 1e6 / fr, (double)t.out_list_ns / 1e6 / fr,
            t.draws / fr, t.claimed / fr, t.submits / fr, t.waited_for / fr);
    }
}

} // namespace HotPath
