// presence_tick.cpp -- the hash-trigger sections no draw fires, run once per frame per on-screen mod.
// See present_parts.h.
#include "../addon_common.h"
#include "../mods/claim_state.h"
#include "../mods/port_section.h"
#include "present_parts.h"

using namespace reshade::api;

void PresenceTick()
{
    {
        static std::vector<std::pair<std::wstring, std::wstring>> s_trigs; // (ns, section)
        static bool                                               s_trigs_built = false;
        static uint32_t                                           s_trigs_gen = 0;
        if (s_trigs_gen != PortApi::LoadGeneration()) {
            s_trigs_gen = PortApi::LoadGeneration();
            s_trigs_built = false;
            s_trigs.clear();
            {
                std::lock_guard<std::mutex> ls(g_skip_vbhash_mtx);
                g_skip_vbhash_snap.reset();
                g_skip_vbhash_any = false;
            } // rebuilt below
            std::lock_guard<std::mutex> l(g_cs_trigs_mtx);
            g_cs_trigs.clear();                                              // rebuilt below with s_trigs
            g_cs_trigs_snap = std::make_shared<const std::vector<CsTrig>>(); // nothing fires until the rebuild publishes
        }
        if (!s_trigs_built && g_texovr_ready) {
            PortApi::ForEachTextureOverride(
                [](uint32_t hash, const wchar_t* fq, bool ranged, const wchar_t*, void* user) {
                    auto* out = (std::vector<std::pair<std::wstring, std::wstring>>*)user;
                    if (!fq) return;
                    if (ranged) return; // driven per-draw by draw_comp
                    const wchar_t* leaf = wcsrchr(fq, L'\\');
                    if (!leaf) return;                                     // no namespace: a global section
                    if (_wcsnicmp(leaf + 1, L"component", 9) == 0) return; // draw-triggered
                    if (hash && TexRulesNow().hash_to_rule.count(hash)) return;
                    const wchar_t* ns_beg = wcschr(fq, L'\\'); // skip the type prefix
                    if (!ns_beg || ns_beg >= leaf) return;
                    if (SectionReadsPipelineState(fq)) {
                        std::lock_guard<std::mutex> l(g_cs_trigs_mtx);
                        g_cs_trigs.push_back({ std::wstring(ns_beg + 1, leaf), std::wstring(fq), hash });
                        return;
                    }
                    out->emplace_back(std::wstring(ns_beg + 1, leaf), std::wstring(fq));
                },
                &s_trigs);
            s_trigs_built = true;
            { // the skip sections: texture targets are the texture pipeline's, the rest are meshes
                auto skips = std::make_shared<std::unordered_set<uint32_t>>();
                PortApi::ForEachSkipTextureOverride(
                    [](uint32_t hash, const wchar_t* fq, void* user) {
                        if (!hash || TexRulesNow().hash_to_rule.count(hash)) return;
                        ((std::unordered_set<uint32_t>*)user)->insert(hash);
                        logger::write("SKIPONLY: %08x [%ls] -- a game draw of this mesh is dropped", hash, fq ? fq : L"?");
                    },
                    skips.get());
                std::lock_guard<std::mutex> ls(g_skip_vbhash_mtx);
                g_skip_vbhash_snap = skips->empty() ? nullptr : std::shared_ptr<const std::unordered_set<uint32_t>>(skips);
                g_skip_vbhash_any = !skips->empty();
            }
            size_t ncs = 0;
            {
                std::lock_guard<std::mutex> l(g_cs_trigs_mtx);
                ncs = g_cs_trigs.size();
                g_cs_trigs_snap = std::make_shared<const std::vector<CsTrig>>(g_cs_trigs);
            } // the dispatch hook's copy, made once
            logger::write("Wcl: %zu presence-trigger sections (+%zu DISPATCH-bound: bodies that "
                          "read cs/vs/ps, moved off the per-frame path)",
                s_trigs.size(), ncs);
        }
        constexpr uint32_t kRetryFrames = 60;
        if (s_trigs_built)
            for (int md = 0; md < (int)g_geomods.size(); ++md) {
                const uint32_t age = ModClaimAge(md);
                const bool     fresh = (age <= 2u);
                const bool     retry = !fresh && (g_frame_id % kRetryFrames == (uint32_t)md % kRetryFrames);
                if (!fresh && !retry) continue;
                if (retry && logger::enabled(1)) {
                    static std::vector<uint32_t> s_last_log; // per unit, grown to the load
                    if (s_last_log.size() <= (size_t)md) s_last_log.resize(md + 1);
                    if (g_frame_id - s_last_log[md] > 600) {
                        s_last_log[md] = g_frame_id;
                        logger::write("RECOVER: mod %d claim stale (age=%u) -- re-running presence triggers", md, age);
                    }
                }
                const std::wstring& uns = g_geomods[md].wcl_ns;
                if (uns.empty()) continue;
                for (auto& [tns, tfq] : s_trigs)
                    if (_wcsicmp(tns.c_str(), uns.c_str()) == 0)
                        PortApi::RunSectionByName(tfq.c_str(), nullptr);
            }
    }
}
