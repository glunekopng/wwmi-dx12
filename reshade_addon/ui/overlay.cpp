// overlay.cpp -- the addon's ImGui control panel; it only reads and writes the shared flag globals.
#include <imgui.h>
#include "overlay.h"
#include "../config.h"
#include "../build_stamp.h"
#include "../instrumentation/profiler.h"
#include "../instrumentation/diagnostics.h"
#include "../migoto/frame_analysis_driver.h"
#include "../mods/skeleton.h"
#include "../mods/claim_state.h" // ModOnScreen: the per-component isolate list shows the mods on screen
#include "../textures/texture_module.h"
#include "../textures/tex_lod.h"       // TexLodSetResemblance (A13), TexLodSetCache (A14)
#include "../hooks/resource_tracker.h" // SetAddonDisabled (A1)
#include "../../src/mod_loader.h"
#include "../instrumentation/morph_dump.h"
#include "../migoto/port_api.h"      // the loaded engine: variables for the PANELGATE dump
#include "../migoto/dx12_bridge.h"   // X30 counters
#include "../migoto/port_toggles.h"  // g_inherit_game_viewport, g_port_post_split, g_blend_write_all_rt
#include "../migoto/dx12_pipeline.h" // ArmPresentDrawProbe (PRESENTDRAW)
#include "../rabbitfx/pso_blend.h"   // GetRfxPatchStats (E2)
#include "../../src/log.h"           // PANELGATE dump
#include <cstdio>
#include <set>
#include <unordered_map>
#include <string>
#include "../instrumentation/sk_ring.h"
#include "../rt/rt_hooks.h" // RtDrawOverlay

using namespace reshade::api;

static void IdLabel(char* buf, size_t n, const char* id, const char* label)
{
    static std::unordered_map<std::string, std::string> s_seen; // id -> first label that claimed it
    static std::set<std::string>                        s_reported;
    auto                                                it = s_seen.find(id);
    if (it == s_seen.end())
        s_seen.emplace(id, label ? label : "");
    else if (it->second != (label ? label : "") && s_reported.insert(id).second)
        logger::write("OVERLAY WARNING: duplicate control id '%s' -- \"%s\" and \"%s\". "
                      "Referring to it by id is now ambiguous.",
            id, it->second.c_str(), label ? label : "");
    snprintf(buf, n, "%s. %s", id, label);
}
bool NCheckbox(const char* id, const char* label, bool* v)
{
    char buf[352];
    IdLabel(buf, sizeof(buf), id, label);
    ImGui::PushID(id);
    bool changed = ImGui::Checkbox("##v", v);
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(buf);
    ImGui::PopTextWrapPos();
    if (ImGui::IsItemClicked()) {
        *v = !*v;
        changed = true;
    }
    ImGui::PopID();
    return changed;
}
bool NButton(const char* id, const char* label)
{
    char buf[352];
    IdLabel(buf, sizeof(buf), id, label);
    const bool pressed = ImGui::Button(id);
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(label);
    ImGui::PopTextWrapPos();
    return pressed;
}
static bool NSliderInt(const char* id, const char* label, int* v, int lo, int hi)
{
    char buf[352];
    IdLabel(buf, sizeof(buf), id, label);
    return ImGui::SliderInt(buf, v, lo, hi);
}
static bool NSliderFloat(const char* id, const char* label, float* v, float lo, float hi, const char* fmt = "%.3f")
{
    char buf[352];
    IdLabel(buf, sizeof(buf), id, label);
    return ImGui::SliderFloat(buf, v, lo, hi, fmt);
}
static bool NInputInt(const char* id, const char* label, int* v)
{
    char buf[352];
    IdLabel(buf, sizeof(buf), id, label);
    return ImGui::InputInt(buf, v);
}
static bool NInputText(const char* id, const char* label, char* v, size_t n, ImGuiInputTextFlags f = 0)
{
    char buf[352];
    IdLabel(buf, sizeof(buf), id, label);
    return ImGui::InputText(buf, v, n, f);
}

struct OverlayWrapScope {
    OverlayWrapScope() { ImGui::PushTextWrapPos(0.0f); }
    ~OverlayWrapScope() { ImGui::PopTextWrapPos(); }
};

static const char kRtBlockedText[] =
    "Ray tracing is ON in the game, and this version does not support it: mods are OFF.\n"
    "Turn ray tracing off in the game's graphics settings, then restart the game.";

void DrawRtBlockedNotice(effect_runtime*)
{
    ImGui::TextColored({ 1, 0.4f, 0.4f, 1 }, "wwmi-dx12: %s", kRtBlockedText);
}

void DrawOverlay(effect_runtime*)
{
    OverlayWrapScope wrap_every_line;
    ImGui::Text("wwmi-dx12 %s (build %s)", WWMI_VERSION, WWMI_BUILD_SHA);
#ifdef WWMI_DEBUG
    ImGui::TextDisabled("Compiled: %s %s", __DATE__, __TIME__);

    {
        static uint32_t s_last_seen = 0;
        static int      s_stalls = 0;
        if (g_frame_id == s_last_seen) {
            if (s_stalls < 1000) ++s_stalls;
        } else {
            s_stalls = 0;
        }
        s_last_seen = g_frame_id;
        const bool live = (s_stalls < 30); // ~30 overlay frames with no present tick = not just paused
        ImGui::TextColored(live ? ImVec4{ 0.6f, 1, 0.6f, 1 } : ImVec4{ 1, 0.4f, 0.4f, 1 },
            "on_present frame: %u %s | log level: %d%s",
            g_frame_id, live ? "(live)" : "(STALLED -- present hook not running)",
            logger::g_level,
            logger::g_level == 0 ? "  <- silent by design; tick A2 to log" : "");
    }
#endif

    ImGui::Separator();
    if (g_rt_blocked) {
        ImGui::TextColored({ 1, 0.4f, 0.4f, 1 }, "%s", kRtBlockedText);
        return;
    }
#ifdef WWMI_DEBUG
    ImGui::TextColored({ 1, 1, 0.6f, 1 }, "*** MASTER ***");
#endif
    // Master bypass: every hot handler early-outs, for an A/B against a vanilla game with the addon loaded.
    {
        bool dis = g_addon_disabled.load();
        if (NCheckbox("A1", "Disable addon (F6 toggles)", &dis))
            SetAddonDisabled(dis);
        if (dis) {
            ImGui::TextColored({ 1, 0.4f, 0.4f, 1 }, "ADDON DISABLED -- no mods, no overrides, no skinning.");
            return;
        }
    }

    if (NCheckbox("A2", "Debug log (wwmi_dx12_addon.log -- attach it to bug reports)", &g_debug)) {
        SetDiagnostics(g_debug, g_log_verbosity);
        if (!g_addon_ini_path.empty())
            WritePrivateProfileStringW(L"wwmi-dx12", L"debug", g_debug ? L"1" : L"0", g_addon_ini_path.c_str());
    }
    if (g_debug) {
        if (NSliderInt("A3", "Debug level (1 = events/errors, 2 = +info)", &g_log_verbosity, 1, 2)) {
            SetDiagnostics(g_debug, g_log_verbosity);
            if (!g_addon_ini_path.empty())
                WritePrivateProfileStringW(L"wwmi-dx12", L"log_level", g_log_verbosity == 1 ? L"1" : L"2", g_addon_ini_path.c_str());
        }
    }
    if (NCheckbox("A4", "Decensor", &g_decensor) && !g_addon_ini_path.empty())
        WritePrivateProfileStringW(L"wwmi-dx12", L"decensor", g_decensor ? L"1" : L"0", g_addon_ini_path.c_str());
#ifdef WWMI_DEBUG
    if (NCheckbox("A8", "Hide mod outlines", &g_hide_outline) && !g_addon_ini_path.empty())
        WritePrivateProfileStringW(L"wwmi-dx12", L"hide_outline", g_hide_outline ? L"1" : L"0", g_addon_ini_path.c_str());
#endif
    {
        bool lookalike = g_texlod_resemblance;
        if (NCheckbox("A13", "Use look-alike texture matches (a distant copy that only looks like a mod's texture gets it, in that mod's own draws)", &lookalike)) {
            TexLodSetResemblance(lookalike);
            if (!g_addon_ini_path.empty())
                WritePrivateProfileStringW(L"wwmi-dx12", L"lod_resemblance", lookalike ? L"1" : L"0", g_addon_ini_path.c_str());
        }
    }
    {
        bool cache = g_texlod_cache;
        if (NCheckbox("A14", "Use texture match cache (mod textures on the game's distant copies; off: nothing is learned, read or saved)", &cache)) {
            TexLodSetCache(cache);
            if (!g_addon_ini_path.empty())
                WritePrivateProfileStringW(L"wwmi-dx12", L"lod_cache", cache ? L"1" : L"0", g_addon_ini_path.c_str());
        }
    }
    if (NCheckbox("A12", "Load mod textures in the background", &g_tex_deferred) && !g_addon_ini_path.empty())
        WritePrivateProfileStringW(L"wwmi-dx12", L"tex_background", g_tex_deferred ? L"1" : L"0", g_addon_ini_path.c_str());
    ImGui::TextDisabled("  on = no stutter when a modded character first shows; its own textures appear a moment later.\n"
                        "  off = the mod's textures from the first frame; the game pauses briefly while they load.\n"
                        "  Applies to characters that show after the change.");
    {
        int mods = 0;
        ModLoader::GetLastStats(mods);
        ImGui::Text("Active mods: %d", mods);
    }
    {
        static bool s_clear = false;
        if (NButton("A11", "Clear caches at next launch (patched shaders, texture matches)") && !g_addon_ini_path.empty()) {
            WritePrivateProfileStringW(L"wwmi-dx12", L"clear_cache", L"1", g_addon_ini_path.c_str());
            s_clear = true;
        }
        if (s_clear) ImGui::TextColored({ 1, 1, 0, 1 }, "Caches will be cleared at the next launch.");
    }
#ifdef WWMI_DEBUG
    NCheckbox("A9", "Skip the engine tick: no [Present] lists, no present pass (perf A/B only -- mods stop updating)", &g_skip_engine_tick);
    if (NButton("A10", "Verbose log for 600 frames: every line, no rate limit, per-frame telemetry, shader patch dumps"))
        VerboseArm(600);
    if (logger::unlimited())
        ImGui::TextColored({ 1, 1, 0, 1 }, "verbose log ON");
#endif
    if (NButton("B1", "Reload mods (re-reads every mod ini)")) {
        g_reload_requested.store(true);
        logger::write("RELOAD: requested from overlay -- will run at the next frame boundary");
    }
#ifdef WWMI_DEBUG

    // ---- Pipeline profiler: 1-second min/avg/max windows -----------------------
    if (ImGui::CollapsingHeader("Pipeline profiler (1-second min/avg/max)")) {
        ImGui::Checkbox("Enable profiler (adds QPC timers to hot hooks)", &g_prof_enabled);
        double fra = g_prof_disp_fr_avg;
        ImGui::Text("Frame: avg %.2f ms (%.0f fps)  |  min %.2f  max %.2f ms  (%d frames/s)",
            fra, g_prof_disp_fps, g_prof_disp_fr_min, g_prof_disp_fr_max, g_prof_disp_frames);
        if (g_prof_enabled) {
            ImGui::Separator();
            ImGui::PushTextWrapPos(-1.0f);
            if (ImGui::BeginTable("proftab", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
                ImGui::TableSetupColumn("step");
                ImGui::TableSetupColumn("incl ms");
                ImGui::TableSetupColumn("incl %");
                ImGui::TableSetupColumn("self ms");
                ImGui::TableSetupColumn("self %");
                ImGui::TableSetupColumn("us/call");
                ImGui::TableSetupColumn("max ms");
                ImGui::TableSetupColumn("calls/f");
                ImGui::TableHeadersRow();
                for (int i = 0; i < PROF_COUNT; ++i) {
                    const double   avg = g_prof_disp_avg_ms[i];
                    const double   self = g_prof_disp_self_ms[i];
                    const uint32_t nc = g_prof_disp_calls[i];
                    const double   ipct = fra > 0.0 ? 100.0 * avg / fra : 0.0;
                    const double   spct = fra > 0.0 ? 100.0 * self / fra : 0.0;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(g_prof_names[i]);
                    ImGui::TableNextColumn();
                    if (avg >= 0.05)
                        ImGui::Text("%.3f", avg);
                    else
                        ImGui::TextDisabled("%.3f", avg);
                    ImGui::TableNextColumn();
                    if (ipct >= 5.0)
                        ImGui::TextColored({ 1, 0.5f, 0.3f, 1 }, "%.1f%%", ipct);
                    else
                        ImGui::Text("%.1f%%", ipct);
                    ImGui::TableNextColumn();
                    if (self >= 0.05 && self > 0.5 * avg)
                        ImGui::TextColored({ 1, 0.85f, 0.4f, 1 }, "%.3f", self);
                    else if (self >= 0.05)
                        ImGui::Text("%.3f", self);
                    else
                        ImGui::TextDisabled("%.3f", self);
                    ImGui::TableNextColumn();
                    if (spct >= 5.0)
                        ImGui::TextColored({ 1, 0.85f, 0.4f, 1 }, "%.1f%%", spct);
                    else
                        ImGui::Text("%.1f%%", spct);
                    ImGui::TableNextColumn();
                    if (nc)
                        ImGui::Text("%.2f", avg * 1000.0 / (double)nc);
                    else
                        ImGui::TextDisabled("-");
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", g_prof_disp_max_ms[i]);
                    ImGui::TableNextColumn();
                    ImGui::Text("%u", nc);
                }
                ImGui::EndTable();
            }
            ImGui::PopTextWrapPos();
            ImGui::Separator();
            ImGui::TextColored({ 1, 0.85f, 0.4f, 1 }, "HOT PATH -- busiest thread %u: %.3f ms/frame (%.1f%% of frame), of %d thread(s)",
                g_prof_disp_thread_max_tid, g_prof_disp_thread_max_ms,
                fra > 0.0 ? 100.0 * g_prof_disp_thread_max_ms / fra : 0.0,
                g_prof_disp_thread_count);
            ImGui::PushTextWrapPos(-1.0f); // same reason as the table above: cells are columns, not prose
            if (g_prof_disp_hot_n > 0 && ImGui::BeginTable("profhot", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
                ImGui::TableSetupColumn("on that thread");
                ImGui::TableSetupColumn("self ms");
                ImGui::TableSetupColumn("% of its gross");
                ImGui::TableHeadersRow();
                for (int k = 0; k < g_prof_disp_hot_n; ++k) {
                    const char* nm = g_prof_names[g_prof_disp_hot_bucket[k]];
                    while (*nm == ' ') ++nm; // the tree indent is not wanted here
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(nm);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", g_prof_disp_hot_ms[k]);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.0f%%", g_prof_disp_thread_max_ms > 0.0 ? 100.0 * g_prof_disp_hot_ms[k] / g_prof_disp_thread_max_ms : 0.0);
                }
                ImGui::EndTable();
            }
            ImGui::PopTextWrapPos();
            if (g_prof_disp_thr_n > 0) {
                char line[512];
                int  off = 0;
                for (int k = 0; k < g_prof_disp_thr_n && off < (int)sizeof(line) - 24; ++k)
                    off += snprintf(line + off, sizeof(line) - off, "%s%.2f", k ? " | " : "", g_prof_disp_thr_ms[k]);
                ImGui::TextDisabled("per-thread ms (busiest first): %s", line);
            }
            ImGui::Separator();
            double gpct = fra > 0.0 ? 100.0 * g_prof_disp_total_ms / fra : 0.0;
            double npct = fra > 0.0 ? 100.0 * g_prof_disp_total_net_ms / fra : 0.0;
            double opct = fra > 0.0 ? 100.0 * g_prof_disp_overhead_ms / fra : 0.0;
            ImGui::Text("ADDON CPU, ALL THREADS: %.3f ms (%.1f%% frame)   |   NET (timers removed): %.3f ms (%.1f%%)",
                g_prof_disp_total_ms, gpct, g_prof_disp_total_net_ms, npct);
            ImGui::Text("BUSIEST THREAD (what the frame pays): %.3f ms on tid %u, of %d thread(s) with timed work",
                g_prof_disp_thread_max_ms, g_prof_disp_thread_max_tid, g_prof_disp_thread_count);
            ImGui::TextColored(opct >= 10.0 ? ImVec4(1, 0.5f, 0.3f, 1) : ImVec4(0.7f, 0.7f, 0.7f, 1),
                "Profiling overhead (est): %.3f ms (%.1f%% frame)  |  %u scopes/frame x %.1f ns/scope",
                g_prof_disp_overhead_ms, opct, g_prof_disp_total_calls, g_prof_percall_ns);
            ImGui::TextColored(g_prof_disp_dc_descs > 50000 ? ImVec4(1, 0.5f, 0.3f, 1) : ImVec4(0.7f, 0.7f, 0.7f, 1),
                "DescCopy volume/frame: %u copies | %u descriptors walked | %u tex-cand swaps",
                g_prof_disp_dc_copies, g_prof_disp_dc_descs, g_prof_disp_dc_swaps);
            // DUMP button: average N seconds and write a text file next to the game exe.
            bool dumping = g_prof_dump_active.load();
            if (dumping)
                ImGui::TextDisabled("Dumping %.0fs...", g_prof_dump_secs);
            else if (ImGui::Button("Dump profiler to file"))
                g_prof_dump_request.store(true);
            ImGui::SetNextItemWidth(90);
            ImGui::InputDouble("secs", &g_prof_dump_secs, 0, 0, "%.0f");
            if (g_prof_dump_last[0]) ImGui::TextDisabled("last dump: %s (in game Binaries\\Win64)", g_prof_dump_last);
            ImGui::TextDisabled("Avg/min/max over last 1s. Indented rows nest inside the hook above (DI:* in DrawIndexed; Drain* in Present) -- not summed twice. NET is the real addon cost; if overhead%% is high, the timers themselves are dragging FPS.");
            ImGui::Separator();
            if (!g_prof_mem_have) {
                ImGui::TextDisabled("Memory: no sample yet (first one lands ~1s after enabling the profiler).");
            } else {
                const double MB = 1024.0 * 1024.0;
                const double vram_pct = g_prof_mem.vram_budget ? 100.0 * (double)g_prof_mem.vram_usage / (double)g_prof_mem.vram_budget : 0.0;
                const double d_vram = ((double)(int64_t)g_prof_mem.vram_usage - (double)(int64_t)g_prof_mem_base.vram_usage) / MB;
                const double d_host = ((double)(int64_t)g_prof_mem.host_usage - (double)(int64_t)g_prof_mem_base.host_usage) / MB;
                const double d_priv = ((double)(int64_t)g_prof_mem.proc_private - (double)(int64_t)g_prof_mem_base.proc_private) / MB;
                ImGui::TextColored(vram_pct >= 100.0 ? ImVec4(1, 0.4f, 0.3f, 1) : (vram_pct >= 90.0 ? ImVec4(1, 0.8f, 0.3f, 1) : ImVec4(0.75f, 0.75f, 0.75f, 1)),
                    "VRAM (LOCAL)     %6.0f / %6.0f MB (%.0f%% of budget)   peak %6.0f   %+.0f MB since start",
                    g_prof_mem.vram_usage / MB, g_prof_mem.vram_budget / MB, vram_pct,
                    g_prof_mem_peak.vram_usage / MB, d_vram);
                ImGui::TextColored(ImVec4(0.75f, 0.75f, 0.75f, 1),
                    "HOST (NON_LOCAL) %6.0f / %6.0f MB   peak %6.0f   %+.0f MB since start   <- upload heaps + VRAM spill",
                    g_prof_mem.host_usage / MB, g_prof_mem.host_budget / MB,
                    g_prof_mem_peak.host_usage / MB, d_host);
                ImGui::TextColored(ImVec4(0.75f, 0.75f, 0.75f, 1),
                    "Process private  %6.0f MB (WS %.0f MB)   peak %6.0f   %+.0f MB over %.0f s",
                    g_prof_mem.proc_private / MB, g_prof_mem.proc_ws / MB,
                    g_prof_mem_peak.proc_private / MB, d_priv, g_prof_mem_age_s);
                ImGui::TextDisabled("tables: va_to_res %u (start %u) | res_to_va %u (%u) | cmd_state %u (%u) | sk_pending %u | geomods %u",
                    g_prof_mem.n_va_to_res, g_prof_mem_base.n_va_to_res,
                    g_prof_mem.n_res_to_va, g_prof_mem_base.n_res_to_va,
                    g_prof_mem.n_cmd_state, g_prof_mem_base.n_cmd_state,
                    g_prof_mem.n_sk_pending, g_prof_mem.n_geomods);
                const bool staging_hot = g_prof_mem.staging_cap &&
                    g_prof_mem.staging_used >= (g_prof_mem.staging_cap * 3 / 4);
                ImGui::TextColored(staging_hot ? ImVec4(1, 0.5f, 0.3f, 1) : ImVec4(0.55f, 0.55f, 0.55f, 1),
                    "port heaps: staging %u/%u high-water (%u free-listed) | rootsigs %u | PSOs %u | heap mirrors %u = %.1f MB",
                    g_prof_mem.staging_used, g_prof_mem.staging_cap, g_prof_mem.staging_freed,
                    g_prof_mem.n_rootsigs, g_prof_mem.n_psos,
                    g_prof_mem.n_heap_maps, g_prof_mem.heap_map_bytes / MB);
            }
        } else {
            ImGui::TextDisabled("Profiler off: frame-time/fps above is still live (zero overhead). "
                                "Enable it to sample memory/VRAM -- the baseline latches when you do.");
        }
    }
    ImGui::Separator();

    ImGui::Separator();
    ImGui::TextColored({ 1, 0.9f, 0.6f, 1 }, "*** INJECTION ***");

    ImGui::Separator();
    ImGui::TextColored({ 0, 1, 1, 1 }, "UAV Stalker Findings:");
    ImGui::BeginChild("StalkerList", ImVec2(0, 200), true);
    {
        std::lock_guard lock_st(g_stalker_mutex);
        for (const auto& res : g_stalker_results) {
            ImGui::BulletText("CS %llx -> Table %u[%u]", res.cs_hash, res.root, res.slot);
        }
    }
    ImGui::EndChild();
    if (NButton("B2", "Clear Discovery List")) {
        std::lock_guard lock_st(g_stalker_mutex);
        g_stalker_results.clear();
    }
    if (NButton("B3", "Re-arm distance capture (SKINPROBE/POOLDRAW)")) g_diag_gen.fetch_add(1);
    ImGui::TextDisabled("Travel to the spot (e.g. a character at far LOD), then click to start a fresh capture there.");
    if (NButton("B4", "Watch texture flicker for 600 frames (TEXFLICKER)")) {
        extern std::atomic<int> g_tex_flicker_watch;
        g_tex_flicker_watch.store(600);
        logger::write("TEXFLICKER: armed for 600 frames");
    }
    if (NButton("B5", "Dump panel gate vars (PANELGATE)")) {
        static const wchar_t*  kGates[] = { L"object_detected", L"mod_enabled", L"menu", L"ActiveCharacter",
            L"mod_id", L"show_menu", L"enable_mods" };
        std::set<std::wstring> nss;
        PortApi::ForEachVariable([](const wchar_t* fq, float, bool, void* user) {
            std::wstring s(fq ? fq : L"");
            const size_t bs = s.rfind(L'\\');
            if (bs != std::wstring::npos) ((std::set<std::wstring>*)user)->insert(s.substr(0, bs));
        },
            &nss);
        logger::write("PANELGATE: %zu namespaces", nss.size());
        for (const auto& ns : nss) {
            std::wstring line;
            for (const wchar_t* g : kGates) {
                std::wstring key = ns + L"\\" + g;
                for (auto& c : key) c = (wchar_t)towlower(c);
                wchar_t b[64];
                swprintf_s(b, L" %ls=%g", g, PortApi::GetVar(key.c_str(), -999.f));
                line += b;
            }
            logger::write("PANELGATE ns='%ls'%ls", ns.c_str(), line.c_str());
        }
    }
    if (NButton("B6", "Trace [Present] draws for 3 frames (PRESENTDRAW)"))
        MigotoPort::ArmPresentDrawProbe(3);

    ImGui::Separator();
    {
        uint32_t rc = g_draw_replace_count.n.load();
        if (rc > 0)
            ImGui::TextColored({ 0, 1, 0, 1 }, "Draw replacements this frame: %u  (last: %016llx  sub_draws=%u)",
                rc, g_last_replace_hash, g_last_replace_ib_count);
        else
            ImGui::TextDisabled("Draw replacements this frame: 0");
    }
    // Merged-skeleton (WWMI-faithful bone CB injection) status
    {
        NCheckbox("B7", "FULL-PORT draw path (execute ini command lists -- off = none run)", &g_wcl_draw);
        NCheckbox("B8", "Merged-skeleton bone CB injection", &g_merged_enable);
        NCheckbox("B9", "Restore VB/IB after draw", &g_restore_vbib);
        NCheckbox("B10", "Restore bone CB after draw", &g_restore_cb);
        NCheckbox("B11", "Restore viewport/scissor/topology/blend/stencil after draw", &g_restore_nonpso);
        NCheckbox("B12", "Inherit game viewport on injected draws (off = old swap-chain-sized viewport)",
            &MigotoPort::g_inherit_game_viewport);
        NCheckbox("B13", "Pre/post SPLIT: run post lists after the draw (off = old inline behaviour)",
            &MigotoPort::g_port_post_split);
        if (NButton("B15", "Log merged-blend draws (8 frames)")) g_log_merged.store(8);
        NCheckbox("B16", "Hide UNCLAIMED char draws (shadow/LOD proxy) [clone test]", &g_hide_unclaimed_char);
        {
            static char vsbuf[24] = "";
            ImGui::SetNextItemWidth(180);
            if (ImGui::InputText("HIDE draw by VS hash (hex)", vsbuf, sizeof(vsbuf), ImGuiInputTextFlags_CharsHexadecimal))
                g_hide_vs_hash = _strtoui64(vsbuf, nullptr, 16);
            if (g_hide_vs_hash) {
                ImGui::SameLine();
                if (ImGui::SmallButton("clear##hidevs")) {
                    g_hide_vs_hash = 0;
                    vsbuf[0] = 0;
                }
                ImGui::SameLine();
                ImGui::TextColored({ 1, 1, 0, 1 }, "hiding %016llx", (unsigned long long)g_hide_vs_hash);
            }
        }
        NCheckbox("B17", "Skip passes w/o bones@root13 (draw vanilla) [spiral test]", &g_skip_no_bone13);
        if (ImGui::CollapsingHeader("Per-component isolate (Hide / Vanilla / Freeze)")) {
            if (!g_merged_loaded || g_merged_comps.empty()) {
                ImGui::TextDisabled("  no components loaded");
            } else {
                if (NButton("B18", "Clear all component isolation"))
                    for (auto& e : g_merged_comps) e.hide = e.vanilla = e.freeze = false;
                ImGui::TextDisabled("  H = hide draw   V = vanilla (skip mod)   F = freeze (bind pose)");
                size_t shown = 0;
                for (size_t i = 0; i < g_merged_comps.size(); ++i) {
                    MergedComp& mc = g_merged_comps[i];
                    if (!ModOnScreen(mc.mod_id) && !(mc.hide || mc.vanilla || mc.freeze)) continue; // an isolated one stays visible
                    ++shown;
                    ImGui::PushID((int)i);
                    ImGui::Checkbox("H", &mc.hide);
                    ImGui::SameLine();
                    ImGui::Checkbox("V", &mc.vanilla);
                    ImGui::SameLine();
                    ImGui::Checkbox("F", &mc.freeze);
                    ImGui::SameLine();
                    ImGui::Text("mod %d comp %d  (fi=%u  n=%u)", mc.mod_id, mc.comp_idx, mc.first_index, mc.match_count);
                    ImGui::PopID();
                }
                if (!shown) ImGui::TextDisabled("  no mod on screen");
            }
        }
        if (g_merged_loaded)
            ImGui::TextColored({ 0, 1, 1, 1 }, "  comps=%zu", g_merged_comps.size());
        else
            ImGui::TextDisabled("  merged remap not loaded (no geometry mod loaded)");
    }
    NCheckbox("B19", "Disable VB Injection", &g_disable_vb);
    NCheckbox("B20", "One merged skeleton per copy of a character drawn at once (off: the copies share one and can mix)", &g_skel_copies);

    ImGui::Separator();
    ImGui::TextColored({ 1, 0.6f, 1, 1 }, "*** TEXTURE OVERRIDE ***");
    const bool swaps_were = g_global_tex && !g_disable_tex;
    NCheckbox("C1", "Disable texture override: game textures on every draw, mod meshes included (no texture swaps, no mod textures, no RabbitFX maps)", &g_disable_tex);
    NCheckbox("C2", "Placement swap, pre-GPU (g_global_tex)", &g_global_tex);
    TexSwapsSwitched(swaps_were);
    NCheckbox("C3", "Per-draw in-place stamp, live heap (g_tex_inplace_stamp)", &g_tex_inplace_stamp);
    NCheckbox("C3R", "C3 stamps only the slots the draw's shaders read (g_tex_read_slots)", &g_tex_read_slots);
    NCheckbox("C4", "RabbitFX SetTextures bind (g_rabbitfx_tex)", &g_rabbitfx_tex);
    NCheckbox("C4S", "  -> only into shaders that read t0, t1 and t2 (RabbitFX's main shaders); outline and prepass shaders keep the game's textures", &g_rabbitfx_tex_main);
    NCheckbox("C4G", "  -> only in the main passes (4+ render targets), as RabbitFX on DX11; off: every pass that draws the part, outlines included", &g_rabbitfx_tex_gbuffer);
    NCheckbox("C6", "Section `ps-tN = ResourceX` stamped in place for the injected draw (g_ps_slot_stamp)", &g_ps_slot_stamp);
    NCheckbox("C7", "  -> and `ps-tN = null` as a null descriptor (g_ps_slot_stamp_nulls)", &g_ps_slot_stamp_nulls);
    {
        uint32_t sc = g_ps_slot_stamp_count.load();
        ImGui::TextColored(sc > 0 ? ImVec4(0, 1, 0, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1), "  ps-tN stamps: %u", sc);
    }
    if (NButton("C5", "Texture target audit -> log (TEXAUDIT: per mod, targets never seen / never stamped)")) TexTargetAudit();
    {
        uint32_t rc = g_rabbitfx_tex_count.load();
        ImGui::TextColored(rc > 0 ? ImVec4(0, 1, 0, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1), "  slot binds: %u", rc);
    }

    ImGui::Separator();
    ImGui::TextColored({ 0.9f, 0.6f, 1, 1 }, "*** RABBITFX GLOW ***");
    {
        uint32_t gp = g_rfx_glow_params.load();
        ImGui::TextColored(gp > 0 ? ImVec4(0, 1, 1, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1), "  comps armed by the ini: %u", gp);
    }
    NCheckbox("E2", "RabbitFX shader patch: draw on the game's PS patched with the ini's t51 cutout and t50 glow / hue (g_rfx_patch)", &g_rfx_patch);
    {
        const RfxPatchStats cs = GetRfxPatchStats();
        const uint32_t      cd = g_rfx_patch_draws.load();
        const uint32_t      cn = g_rfx_patch_nobind.load();
        ImGui::TextColored(cs.ps_failed ? ImVec4(1, 0.5f, 0, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1), "  shaders patched this instance: %u  failed: %u  PSO variants: %u  (see RFXPATCH in the log)", cs.ps_patched, cs.ps_failed, cs.variants);
        ImGui::TextColored(cd > 0 ? ImVec4(0, 1, 0, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1), "  patched draws: %u (with glow: %u)  maps not bindable: %u", cd, g_rfx_glow_count.load(), cn);
    }

    ImGui::Separator();
    ImGui::TextColored({ 0.9f, 0.6f, 1, 1 }, "*** RENDER FIXES ***");
    NCheckbox("D2", "IBHASH-suppress LOD proxies (g_ibhash_suppress)", &g_ibhash_suppress);
    {
        uint32_t ic = g_ibhash_suppress_count.load();
        ImGui::TextColored(ic > 0 ? ImVec4(0, 1, 0, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1), "  suppressed: %u", ic);
    }
    NCheckbox("D3", "Vanilla shadow: release depth-only passes (g_shadow_vanilla)", &g_shadow_vanilla);
    NCheckbox("D4", "  Shadow via claim -- needs D3 OFF (g_shadow_via_claim)", &g_shadow_via_claim);
    if (NButton("D5", "  Watch shadow recognition (240 frames -> SHADOWRECOG)")) g_shadow_recog_watch.store(240);
    {
        int rw = g_shadow_recog_watch.load();
        if (rw > 0) ImGui::TextColored({ 0, 1, 1, 1 }, "    armed: %d frames left", rw);
    }
    ImGui::SetNextItemWidth(160);
    NSliderInt("D6", "Suppress comp5 rtv== (-1 off)", &g_dbg_comp5_suppress_rtv, -1, 8);
    NCheckbox("D7", "  Skip modded comp translucent (2-RT) pass (g_skip_translucent_cloth)", &g_skip_translucent_cloth);
    NCheckbox("D8", "ShaderOverride hash dispatch (g_shader_override_dispatch)", &g_shader_override_dispatch);
    NCheckbox("D9", "Resource-lineage tex swap: mod textures at all LODs (g_tex_lineage)", &g_tex_lineage);
    {
        uint32_t lc = g_tex_lineage_count.load();
        if (lc > 0)
            ImGui::TextColored({ 0, 1, 0, 1 }, "  lineage propagations this frame: %u", lc);
        else
            ImGui::TextDisabled("  lineage propagations this frame: 0");
    }
    NCheckbox("D12", "A/B: IGNORE learned LOD aliases -- still recorded (g_texlod_alias_off)", &g_texlod_alias_off);
    NCheckbox("D13", "Mip-exact aliases: a target's mips hashed as the residents they would be (g_mip_exact)", &g_mip_exact);
    NCheckbox("D15", "Dump degenerate LOD textures as they load or are found: <hash>.dds in DX12_Dumps\\degenerates, once each", &g_dump_degenerates);
    {
        uint32_t tc = g_tex_inject_count.load();
        if (tc > 0)
            ImGui::TextColored({ 0, 1, 0, 1 }, "Texture swaps this frame: %u", tc);
        else
            ImGui::TextDisabled("Texture swaps this frame: 0");
        uint32_t gc = g_global_tex_count.load();
        if (gc > 0)
            ImGui::TextColored({ 0, 1, 1, 1 }, "Global descriptor swaps this frame: %u", gc);
        else
            ImGui::TextDisabled("Global descriptor swaps this frame: 0");
    }
    if (NButton("D10", "Log all draws (2 frames) + TEXFLIP (8 frames: a claimed register changing identity frame to frame)")) {
        g_log_all_draws.store(2);
        g_texflip_arm.store(8);
        ++g_texflip_gen;
    }
    if (g_log_all_draws.load() > 0) ImGui::TextColored({ 1, 1, 0, 1 }, "Armed: draw log (%d frames)", g_log_all_draws.load());
    if (NButton("D14", "Compare shaders: press in the normal state, then again in the changed one (e.g. a transformation); the log lists the shaders only the second state drew"))
        g_shaderdiff_arm.store(60);
    if (g_shaderdiff_arm.load() > 0) ImGui::TextColored({ 1, 1, 0, 1 }, "Armed: shader capture (%d frames)", g_shaderdiff_arm.load());
    if (g_texflip_arm.load() > 0) ImGui::TextColored({ 1, 1, 0, 1 }, "Armed: TEXFLIP (%d frames)", g_texflip_arm.load());
    {
        bool lod = g_log_lod.load();
        if (NCheckbox("D11", "Capture LOD/char draws -> CDRAW (walk close->far, then untick)", &lod)) g_log_lod.store(lod);
        if (lod) ImGui::TextColored({ 1, 1, 0, 1 }, "  capturing");
    }

    ImGui::Separator();
    ImGui::TextColored({ 0.6f, 1, 0.6f, 1 }, "*** CAPTURE ***");
    // WHAT a frame dump writes comes from d3dx.ini [Hunting] analyse_options, not from here.
    if (NButton("G2", "CHAR DUMP: one frame, char draws + all of ours (wwmi-tools shape)"))
        MigotoPort::FrameAnalysisArm(1, /*char_only*/ true);
    if (NButton("G3", "FULL DUMP: one frame, every draw (~1600; not importable)"))
        MigotoPort::FrameAnalysisArm(1, /*char_only*/ false);
    if (NButton("G4", "SKCAP BYTES: 8 dispatches in full (J1 covers shapekeys)")) {
        MigotoPort::SkCapArm(8);
        // BRACES ARE LOAD-BEARING: unbraced, this ran every frame and re-armed every DiagOnce site.
        g_diag_epoch.fetch_add(1, std::memory_order_relaxed);
    }
    ImGui::TextDisabled("  what is IN a buffer; ~146 MB per chain, cannot span a frame boundary");
    if (NButton("G5", "SKCAP FRAMES: 10 frames (J1 covers shapekeys)"))
        MigotoPort::SkCapArmFrames(10);
    ImGui::TextDisabled("  WHICH frames the chain ran on; slots >1 MB reduced to stats");
    NCheckbox("G6", "Shapekey: drop passes whose dispatch named no mod (g_sk_strict_owner)",
        &g_sk_strict_owner);
    ImGui::TextDisabled("  off = guess the owner by hash/name. Fully un-morphed with this ON means\n"
                        "  provenance is missing at the queuing site.");
    NCheckbox("G7", "Publish the game's vertex buffers at the draw (g_push_game_vb)",
        &g_push_game_vb);
    ImGui::TextDisabled("  off = the ini's vb0 restore becomes a real NULL unbind; watch VBNULL-UNBIND");
    NCheckbox("G8", "[Present] on the game's draw queue, not the present queue (g_present_on_game_queue)",
        &g_present_on_game_queue);
    ImGui::TextDisabled("  queue-orders the mod's [Present] copies against the frame's draws, as on DX11");
    NCheckbox("G9", "[Present]: flush ReShade's immediate list first (g_present_flush_immediate_first)",
        &g_present_flush_immediate_first);
    ImGui::TextDisabled("  off = RenoDX's back-buffer copy erases the mod panels");
    NCheckbox("G10", "Skip the GPU IB/VB hash dispatch (g_ibhash_gpu_skip)", &g_ibhash_gpu_skip);
    ImGui::TextDisabled("  A/B for the skill freeze: no freeze with this ON = the hash dispatch is the hang.\n"
                        "  Mods recognised only by their ini's hash= stay vanilla. Log: IBHASH-SLOW / -RATE / -SKIP.");
    if (NButton("G11", "VB DIAG (3 frames, log only): which vertex view each injected draw reads"))
        g_vbdiag_frames.store(3);
    ImGui::TextDisabled("  shim=0 on vb0 = ResourceShapeKeyedPosition did not bind for that pass");
    if (NButton("G12", "Occlusion probe: did the injected mesh rasterise? (30 frames -> OCCL)")) g_occl_arm.store(30);
    ImGui::TextDisabled("  NO FRAGMENTS = never rasterised (culled/degenerate/state). RASTERISED = it is\n"
                        "  material or texture, not geometry. Render the character first.");
    ImGui::Separator();
    ImGui::TextColored({ 0.6f, 0.9f, 1, 1 }, "*** 3DMIGOTO PORT ***");
    NCheckbox("H1", "Give their engine the draw's command list (g_port_draw_list)", &g_port_draw_list);
    ImGui::TextDisabled("  off = the mod's GPU work is dropped and it stays vanilla; watch portDisp in HEARTBEAT");
    NCheckbox("H2", "Crash reporter -> wwmi_dx12_crash.log (next launch) (g_crash_handler)", &g_crash_handler);
    ImGui::TextDisabled("  resolve +0x offsets against wwmi_dx12.map");
    NCheckbox("H16", "Stall watchdog: no present for 1 s while focused -> STALL + the present thread's stack (g_stall_watchdog)", &g_stall_watchdog);
    ImGui::TextDisabled("  stalls so far: %u", g_stall_count.load());
    if (NButton("H3", "Trace the chain for 10 frames -> wwmi_dx12_chain_trace.log"))
        g_chaintrace_req.store(true);
    ImGui::TextDisabled("  every executed line with its section, plus all engine variables per frame");
    if (NButton("H4", "Dump the whole skeleton chain (inputs, merger output, remaps, vars)"))
        g_skelchain_req.store(true);
    ImGui::TextDisabled("  char on screen; results ~4 frames later");
    if (NButton("H5", "Dump merged skeleton (what SkeletonMerger wrote)"))
        g_skeldump_req.store(true);
    ImGui::TextDisabled("  all-zero rows = merger ran with wrong inputs; plausible rows = look downstream");
    NCheckbox("H6", "Frame dump includes the GAME's textures -- ~1.3 s/draw (g_dump_game_textures)", &g_dump_game_textures);
    if (NButton("H7", "Log each mod's component layout (self-check -> log)")) g_wwmi_parse.store(true);
    NCheckbox("H8", "LOD pin: identify char lower-LOD draws, log only (g_lod_pin)", &g_lod_pin);
    NCheckbox("H9", "Original-ini geometry for DIRECT comps (g_wwmi_geom)", &g_wwmi_geom);
    {
        uint32_t gc = g_wwmi_geom_count.load();
        if (gc > 0)
            ImGui::TextColored({ 0, 1, 0, 1 }, "  ini-geometry draws this frame: %u", gc);
        else
            ImGui::TextDisabled("  ini-geometry draws this frame: 0");
    }

    NCheckbox("H10", "Custom-material sub-draws: run = [CustomShader*] parts (g_custom_subdraw)", &g_custom_subdraw);
    NCheckbox("H11", "  -> derived blend PSO for transparency (g_custom_blend)", &g_custom_blend);
    {
        uint32_t cc = g_custom_blend_count.load();
        if (cc > 0)
            ImGui::TextColored({ 0, 1, 1, 1 }, "  sub-draws this frame: %u", cc);
        else
            ImGui::TextDisabled("  sub-draws this frame: 0");
    }
    {
        uint32_t bc = g_custom_blend_bound_count.load();
        if (bc > 0)
            ImGui::TextColored({ 0, 1, 0, 1 }, "  blend PSO bound: %u", bc);
        else if (g_custom_blend)
            ImGui::TextColored({ 1, 0.5, 0, 1 }, "  blend PSO bound: 0 -- see CUSTOMBLEND-MISS in log");
        else
            ImGui::TextDisabled("  blend PSO bound: 0");
    }
    {
        static bool  s_override = false;
        static float s_f = 0.5f;
        if (NCheckbox("H12", "  override blend factor (lower = more transparent)", &s_override))
            g_custom_blend_factor = s_override ? s_f : -1.0f;
        if (s_override) {
            if (NSliderFloat("H13", "  blend factor (0.9 = ini default)", &s_f, 0.05f, 1.0f, "%.2f")) g_custom_blend_factor = s_f;
        }
    }
    NCheckbox("H14", "  debug: show background behind piece (g_custom_blend_debug_zero)", &g_custom_blend_debug_zero);
    NCheckbox("H15", "Command-list Reset clears our per-list binding record, as D3D12 does the list's (g_cmd_reset_clears)", &g_cmd_reset_clears);
    ImGui::TextDisabled("  resets so far: %u", g_cmd_resets.load());
    NCheckbox("H18", "Claim by the INDEX BUFFER when a mod's whole component set lands on one (g_ib_anchor_claim)", &g_ib_anchor_claim);
    ImGui::TextDisabled("  needed with ray tracing on, where the game animates vb0 and every content hash dies");
    ImGui::TextDisabled("  index buffers anchored: %u   draws claimed by one: %u",
        g_ib_anchor_learned.load(), g_ib_anchor_claims.load());
    RtDrawOverlay(); // H19..H27 -- the RT module's section (rt/rt_overlay.cpp)
    NCheckbox("H17", "Refuse a char_vb claim whose VB0 was learned from ANOTHER mod (g_charvb_refuse_xmod)", &g_charvb_refuse_xmod);
    ImGui::TextDisabled("  char_vb claims: %u  cross-mod: %u  refused: %u",
        g_charvb_claims.load(), g_charvb_xmod.load(), g_charvb_refused.load());

    ImGui::Separator();
    ImGui::TextColored({ 0.6f, 0.9f, 1, 1 }, "*** SHAPE KEYS / MORPHS (eyelids, facial) ***");
    NCheckbox("I1", "Run the mod's shapekey passes (g_sk_run_passes)", &g_sk_run_passes);
    ImGui::TextDisabled("  OFF is a diagnostic: it explodes some mods' characters by design -- for them\n"
                        "  these passes are the only writer of vb0.");
    NCheckbox("I2", "Forget relocations on resource destroy (g_reloc_forget_on_destroy)",
        &g_reloc_forget_on_destroy);
    ImGui::TextDisabled("  the maps are keyed on the POINTER and Windows recycles addresses");
    NCheckbox("I3", "Freeze shapekeys: zero the deltas before the Applier (g_sk_freeze)", &g_sk_freeze);
    ImGui::TextDisabled("  the morph blocker -- unlike I1 OFF, it does not explode anyone");
    NCheckbox("I5", "Health heartbeat every 600 frames (g_health_heartbeat)",
        &g_health_heartbeat);
    {
        static char s_hex[19] = "";
        static bool s_init = false;
        if (!s_init) {
            s_init = true;
            if (g_morph_cs_hash) snprintf(s_hex, sizeof(s_hex), "%016llx", (unsigned long long)g_morph_cs_hash);
        }
        if (NInputText("I6", "morph cs_hash (hex)", s_hex, sizeof(s_hex), ImGuiInputTextFlags_CharsHexadecimal))
            g_morph_cs_hash = _strtoui64(s_hex, nullptr, 16);
        if (g_morph_cs_hash)
            ImGui::TextColored({ 0, 1, 0, 1 }, "morph cs set: %016llx", (unsigned long long)g_morph_cs_hash);
        else
            ImGui::TextDisabled("morph cs unset (capture inactive)");
    }
    NCheckbox("I7", "DX11->DX12 shader-hash alias for [ShaderOverride] (g_shader_hash_alias)", &g_shader_hash_alias);

    NCheckbox("I8", "Publish bound shader into their HackerContext (g_publish_shader_state)", &g_publish_shader_state);
    ImGui::TextDisabled("  without it the bare cs/vs/ps operand reads -0.0 and every bare shader filter is dead");

    NCheckbox("I9", "Publish the GAME's compute UAV at cs-u0 -- needs I8 (g_publish_cs_u0)", &g_publish_cs_u0);
    ImGui::TextDisabled("  lets `if cs-u0 >= 1` answer. Gated on the desc hash a [TextureOverride] declares,\n"
                        "  so a wrong resolve publishes nothing and logs REFUSED. The mesh WILL change.");

    NCheckbox("I10", "Shapekey: zero u0 before the chain runs (g_sk_u0_clear)", &g_sk_u0_clear);
    ImGui::TextDisabled("  the Loader accumulates with atomic_iadd, so u0 must start zeroed. OFF = the\n"
                        "  kaleidoscope (integers added into the previous frame's float bit patterns).");
    NCheckbox("I11", "Shapekey: zero u1, per-vertex weight sums (g_sk_u1_clear)", &g_sk_u1_clear);
    ImGui::TextDisabled("  OFF = u1 accumulates until it wraps negative and the Multiplier's normalize\n"
                        "  snaps: the flinch. Check the drain table's u1_clear column.");
    NCheckbox("I12", "Shapekey: COPY_DEST barriers around the u0/u1 zero copy (g_sk_clear_barrier)", &g_sk_clear_barrier);
    ImGui::TextDisabled("  a UAV barrier does not order a copy write against the Loader's atomics");
    NCheckbox("I13", "Shapekey: resolve u1 by declared ShapeKeyScale identity (g_sk_u1_by_identity)", &g_sk_u1_by_identity);
    ImGui::TextDisabled("  for when the register has no record; refuses when several buffers fit (U1IDENT)");
    NCheckbox("I14", "Shapekey: honour `handling = skip` for the game's dispatch (g_sk_honour_skip)", &g_sk_honour_skip);
    NCheckbox("I19", "Shapekey: fire the callbacks through the engine's own hash lookup, as the mesh skip does (g_sk_engine_dispatch)", &g_sk_engine_dispatch);
    ImGui::TextDisabled("  DX11 BeforeDispatch semantics. Faithfulness, not a known fix. DISPATCH-SKIP in log.");
    NCheckbox("I18", "Shapekey: cancel the game's u0 clear on a buffer the chain writes (g_sk_cancel_game_clear)", &g_sk_cancel_game_clear);
    ImGui::TextDisabled("  draw lists submitted before the pass list read last frame's deltas, not zeros (frozen face /\n"
                        "  flinch). cancelled so far: %u",
        g_sk_clear_cancelled.load());

    ImGui::TextDisabled("bridge: displaced=%llu  wrong-list draws=%llu  dispatches=%llu",
        (unsigned long long)MigotoPort::BridgeDisplacedCount(), (unsigned long long)MigotoPort::BridgeWrongListDraws(), (unsigned long long)MigotoPort::BridgeWrongListDispatches());
    ImGui::TextDisabled("mid-section restores=%llu  IA rebinds skipped=%llu  (must be equal)",
        (unsigned long long)g_x32_mid_total.load(), (unsigned long long)g_x32_mid_skipped_total.load());

    NCheckbox("I15", "Shapekey: recover u0's extent when the descriptor record has none (g_sk_u0_size_recover)", &g_sk_u0_size_recover);
    ImGui::TextDisabled("  a size-0 record hashes to nothing any [TextureOverride] declares, so the whole\n"
                        "  declared region is skipped. Proves liveness before dereferencing; never guesses.");
    NCheckbox("I16", "Shapekey: snapshot the game's cb0 at its dispatch (g_sk_cb0_snapshot)", &g_sk_cb0_snapshot);
    ImGui::TextDisabled("  OFF holds a VA into the game's constant ring, which is reused before the chain\n"
                        "  replays: scale row 64 reads garbage and every delta rounds to zero (frozen face).");
    NCheckbox("I17", "Blend: write all channels on all 8 render targets (g_blend_write_all_rt)", &MigotoPort::g_blend_write_all_rt);
    ImGui::TextDisabled("  only affects the full-PSO path, which this corpus takes once. Unproven in game.");

    ImGui::Separator();
    ImGui::TextColored({ 0.5, 1, 0.5, 1 }, "*** MORPH DUMP ***");
    ImGui::TextDisabled("Every compute dispatch, unfiltered, metadata only -> morph_dump-<time>.tsv beside\n"
                        "the exe. Answers: of the dispatches no mod owns, how many did we skip or fire\n"
                        "sections for? Both must be zero.");
    if (NButton("J1", "ARM SHAPEKEY CAPTURE -- tables + buffer contents + probes")) MorphDump::ArmCapture();
    if (NButton("J4", "...at the next new instance (an ult's copies)")) MorphDump::ArmCaptureAtNewInstance();
    if (NButton("J2", "Close now")) MorphDump::Close();
    if (NButton("J3", "Dump the shapekey decision ring (last ~15 s; Numpad 1 in capture mode)")) SkRing::Dump("J3");
    ImGui::Text("capture mode %s (Numpad 0)", g_capture_mode ? "ON" : "off");
    ImGui::Text("tables %s | contents %s | probes %s",
        MorphDump::Armed() ? "ARMED" : MorphDump::CaptureWaiting() ? "WAITING"
                                                                   : "idle",
        MigotoPort::SkCapArmed() ? "ARMED" : "idle",
        ProbeArmed() ? "ARMED" : "idle");
    ImGui::TextDisabled("morph_dump / morph_pass / morph_drain-<stamp>.tsv beside the exe: which dispatch\n"
                        "the chain claimed, what cs-u0 resolved to, why a pass was refused.\n"
                        "DX12_Dumps\\SKCAP-<stamp>\\: per-frame ledger, slots <1 MB in full, digests above.\n"
                        "Read the tables first to confirm the chain RAN, then the .buf files for what it\n"
                        "computed. Clean tables + a wrong character means the answer is in the .buf files.");

    ImGui::Separator();
    ImGui::TextColored({ 0, 1, 1, 1 }, "Shader Bytecode Cache:");
    {
        size_t n = 0;
        {
            std::lock_guard lock(g_shader_cache_mutex);
            n = g_shader_cache.size();
        }
        ImGui::Text("Cached shaders (all vs/ps/cs): %zu", n);
    }
    if (NButton("K6", "Dump captured shaders to disk (shader_<stage>_<hash>.bin)")) g_dump_shaders_requested.store(true);
#endif
}
