// draw_shadow.cpp -- a character's depth / shadow pass recognised, so the mod components are cast into it.
// See draw_parts.h.
#include "../addon_common.h"
#include "draw_hook.h"
#include "../mods/claim_state.h"
#include "../hooks/cmd_bind_hooks.h"
#include "../mods/layout_registry.h"
#include "draw_ctx.h"
#include "draw_parts.h"
#include "draw_inject.h"
#include <set>

using namespace reshade::api;

static void ShadowRecog(int stage)
{
    if (g_shadow_recog_watch.load(std::memory_order_relaxed) <= 0) return;
    static std::mutex m;
    static uint32_t   frame = 0xFFFFFFFFu;
    static uint32_t   frames = 0;
    static uint32_t   n_pass = 0;
    static uint32_t   n_mesh = 0;
    static uint32_t   n_ok = 0;
    std::lock_guard   l(m);
    if (frame != g_frame_id) {
        frame = g_frame_id;
        if (++frames % 120 == 0) {
            logger::write("SHADOWRECOG last-120-frames: char-depth-passes=%u meshClaimed=%u fullyRecognised=%u"
                          "  (passes>recognised => some frames fall back to the VANILLA shadow = flicker)",
                n_pass, n_mesh, n_ok);
            n_pass = n_mesh = n_ok = 0;
        }
    }
    ++n_pass;
    if (stage >= 1) ++n_mesh;
    if (stage >= 2) ++n_ok;
}

void RecogniseShadowPass(const DrawArgs& da, BoneRoots& roots, DrawClaim& dc)
{
    reshade::api::command_list* const cmd_list = da.cmd_list;
    const uint32_t                    index_count = da.index_count;
    const uint32_t                    first_index = da.first_index;
    const uint64_t                    vs_hash = da.vs_hash;
    const CmdRT&                      draw_rt = da.draw_rt;
    MergedComp*&                      mc = dc.mc;
    MergedComp*&                      depth_claim = dc.depth_claim;
    int&                              game_cb13_root = dc.cb13_root;
    int&                              game_cb14_root = dc.cb14_root;
    bool&                             is_full_shadow_pass = dc.full_shadow;
    uint64_t&                         shadow_dsv = dc.shadow_dsv;
    uint32_t&                         shadow_pass = dc.shadow_pass;
    if (index_count >= 64 && !mc && g_merged_loaded && !g_shadow_vanilla) { // OFF = draw mod silhouette; ON = let game draw vanilla
        const CmdRT& rt = draw_rt;
        if (rt.rtv_count == 0 && rt.has_dsv) { // depth/shadow caster
            const int         b4r = roots.Get().b4;
            const int         b3r = roots.Get().b3;
            const RootCbvBind b4_cb = roots.Get().b4cb;
            // The claim recognised this depth draw (its mesh + range) and released it: that is the identity.
            const bool char_mesh = depth_claim != nullptr;
            ShadowRecog(char_mesh ? 1 : 0); // 1 = mesh claimed, 0 = not ours (see the counters above)
            bool any_mod_onscreen = false;
            for (int dm = 0; dm < (int)g_geomods.size(); ++dm)
                if (ModOnScreen(dm) && ModGeomEnabled(dm)) {
                    any_mod_onscreen = true;
                    break;
                } // ENABLED mod (a disabled mod keeps its vanilla shadow)
            int shadow_comp = -1;
            if (depth_claim)
                shadow_comp = (int)(depth_claim - g_merged_comps.data());
            else {
                auto hit = g_first_to_comp.find(FirstCountKey(first_index, index_count));
                if (hit != g_first_to_comp.end()) shadow_comp = hit->second;
            }
            const bool comp_range = shadow_comp >= 0 && ModOnScreen(g_merged_comps[shadow_comp].mod_id) && ModGeomEnabled(g_merged_comps[shadow_comp].mod_id);
            if (char_mesh && any_mod_onscreen && comp_range) {
                is_full_shadow_pass = true;
                t_x23_shadow = 1;          // X23: the game-draw table's `shadow` column
                shadow_dsv = rt.dsv;       // which shadow map this pass casts into (per light / per cascade)
                shadow_pass = rt.pass_gen; // ...and which pass wrote it, in case the target is reused
                game_cb13_root = b4r;
                game_cb14_root = -1;
                ShadowRecog(2); // 2 = fully recognised, this pass will be replaced
                if (g_debug) {
                    const uint64_t k = vs_hash ^ ((uint64_t)index_count << 24);
                    if (DiagOnce("DRAW-VSCOUNT", (int)(k ^ (k >> 32)))) {
                        logger::write("SHADOWCLAIM FULL vs=%016llx first=%u count=%u b4root=%d b3root=%d boneVA=%llx",
                            vs_hash, first_index, index_count, b4r, b3r, (unsigned long long)b4_cb.va);
                        uint64_t dvb0 = 0;
                        {
                            dvb0 = CmdGfx(cmd_list).vb_va[0];
                        }
                        bool d_pool = InSkinBand(dvb0);
                        logger::trace("SKINSRC pass=DEPTHCAST idx=%u vb0=%llx inSkinPool=%d b4root=%d boneVA=%llx",
                            index_count, (unsigned long long)dvb0, (int)d_pool, b4r, (unsigned long long)b4_cb.va);
                    }
                }
            }
        }
    }
}

bool CastModShadows(InjectFrame& fr)
{
    reshade::api::command_list* const cmd_list = fr.da.cmd_list;
    const uint64_t                    shadow_dsv = fr.dc.shadow_dsv;
    const uint32_t                    shadow_pass = fr.dc.shadow_pass;
    const int                         game_cb13_root = fr.dc.cb13_root;
    const int                         game_cb14_root = fr.dc.cb14_root;
    auto                              restore_state = [&]() { RestoreGameState(fr); };
    auto                              draw_comp = [&](int dm, const WwmiMod::Component* wc) { return DrawComponent(fr, dm, wc); };
    PROF(PROF_DI_ShadowCast); // N mods x N components per depth target, all inside one game draw
    struct ShadowTarget {
        uint64_t dsv;
        uint32_t pass;
        float    vx;
        float    vy;
        float    vw;
        float    vh;
        bool     replaced;
    };
    auto vp_of = [&](float& x, float& y, float& w, float& h) {
        auto& gs = CmdGfx(cmd_list);
        if (gs.vp_count) {
            x = gs.vp[0].TopLeftX;
            y = gs.vp[0].TopLeftY;
            w = gs.vp[0].Width;
            h = gs.vp[0].Height;
        } else {
            x = y = w = h = 0.0f;
        } // never set -> one region, old behaviour
    };
    float vx;
    float vy;
    float vw;
    float vh;
    vp_of(vx, vy, vw, vh);
    auto same_target = [&](const ShadowTarget& t) {
        return t.dsv == shadow_dsv && t.pass == shadow_pass && t.vx == vx && t.vy == vy && t.vw == vw && t.vh == vh;
    };
    static std::mutex                s_shadow_mtx;
    static uint32_t                  s_shadow_frame = 0xFFFFFFFFu;
    static std::vector<ShadowTarget> s_shadow_done; // depth targets handled this frame
    bool                             cast_here = false;
    bool                             already_replaced = false;
    {
        std::lock_guard l(s_shadow_mtx);
        if (s_shadow_frame != g_frame_id) {
            s_shadow_frame = g_frame_id;
            s_shadow_done.clear();
        }
        auto it = std::find_if(s_shadow_done.begin(), s_shadow_done.end(), same_target);
        if (it == s_shadow_done.end()) {
            s_shadow_done.push_back({ shadow_dsv, shadow_pass, vx, vy, vw, vh, false });
            cast_here = true;
            t_x23_cast = 1; // X23
        } else
            already_replaced = it->replaced;
    }
    if (cast_here) {
        int comp_n = 0;
        int drew_n = 0;
        for (int dm = 0; dm < (int)g_geomods.size(); ++dm) {
            if (!ModOnScreen(dm)) continue;
            if (!ModGeomEnabled(dm)) continue; // WWMI: disabled mod casts its vanilla shadow, not ours
            for (const auto& c : g_geomods[dm].mod.comps) {
                ++comp_n;
                if (draw_comp(dm, &c)) ++drew_n;
            }
        }
        already_replaced = (drew_n > 0);
        {
            std::lock_guard l(s_shadow_mtx);
            auto            it = std::find_if(s_shadow_done.begin(), s_shadow_done.end(), same_target);
            if (it != s_shadow_done.end()) it->replaced = already_replaced;
        }
        if (g_shadow_recog_watch.load(std::memory_order_relaxed) > 0) {
            static std::mutex         m;
            static std::set<uint64_t> seen;
            static uint32_t           s_lastf = 0xFFFFFFFFu;
            bool                      fresh;
            {
                std::lock_guard l(m);
                if (s_lastf != g_frame_id && (g_frame_id % 120) == 0) {
                    s_lastf = g_frame_id;
                    seen.clear();
                } // re-report every 120 frames so a LATER failure is visible
                const uint64_t vkey = ((uint64_t)(uint32_t)vx << 32) ^ (uint64_t)(uint32_t)vy ^ ((uint64_t)(uint32_t)vw << 16) ^ (uint64_t)(uint32_t)vh;
                fresh = ProbeArmed() && seen.size() < 64 && seen.insert((shadow_dsv ^ ((uint64_t)shadow_pass << 32)) ^ ((uint64_t)(drew_n > 0) << 63) ^ vkey).second;
            }
            if (fresh) {
                logger::write("SHADOWDRAW dsv=%llx pass=%u vp=[%.0f,%.0f %.0fx%.0f] comps=%d drew=%d "
                              "cb13root=%d cb14root=%d -> %s",
                    (unsigned long long)shadow_dsv, shadow_pass, vx, vy, vw, vh, comp_n, drew_n,
                    game_cb13_root, game_cb14_root,
                    drew_n > 0 ? "REPLACED (suppress vanilla)" : "NOT replaced (vanilla kept)");
            }
        }
    }
    restore_state();
    return already_replaced; // suppress only what we actually replaced
}
