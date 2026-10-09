// The [ShaderOverride*] dispatch and the hash alias table it looks through. See shader_alias.h.
#include "../addon_common.h"
#include "shader_alias.h"

thread_local uint64_t t_cur_vs_hash = 0;
thread_local uint64_t t_cur_ps_hash = 0;

// Does any [ShaderOverride*] target this shader hash? (optionally return the first section name)
bool ShaderOverrideMatches(uint64_t shader_hash, std::wstring* first_section)
{
    if (!shader_hash) return false;
    if (!PortApi::ShaderOverrideMatches(shader_hash)) return false;
    if (first_section) {
        wchar_t sec[256] = {};
        PortApi::ShaderOverrideSection(shader_hash, sec, std::size(sec));
        *first_section = sec;
    }
    return true;
}

struct ShaderHashAlias {
    uint64_t    dx12;
    uint64_t    dx11;
    const char* what;
};
static const ShaderHashAlias kShaderAliases[] = {
    { 0x4e91ecddc203a6cfULL, 0x9bf4420c82102011ULL, "ShapeKeyLoaderCS" },
    { 0xc1a7f1fec51b482fULL, 0x7a8396180d416117ULL, "ShapeKeyMultiplierCS" },
    { 0xba57f3dd0e35591cULL, 0x30ab50e715dce218ULL, "a character's second-form trigger: the body PS in that state" },
};

uint64_t AliasShaderHashAlways(uint64_t h)
{
    for (const ShaderHashAlias& a : kShaderAliases)
        if (a.dx12 == h) return a.dx11;
    return h;
}

uint64_t AliasShaderHash(uint64_t h)
{
    if (!g_shader_hash_alias) return h;
    for (const ShaderHashAlias& a : kShaderAliases) {
        if (a.dx12 != h) continue;
        if (g_debug) { // one-shot per entry: proves the alias fired, and which
            static std::mutex         m;
            static std::set<uint64_t> seen;
            bool                      fr;
            {
                std::lock_guard l(m);
                fr = seen.insert(h).second;
            }
            if (fr) logger::write("SHADER-ALIAS: dx12 %016llx -> dx11 %016llx (%s)",
                (unsigned long long)a.dx12, (unsigned long long)a.dx11, a.what);
        }
        return a.dx11;
    }
    return h;
}

bool RunShaderOverrides(uint64_t shader_hash, const PortApi::DrawInfo* draw, command_list* cmd_list)
{
    if (!shader_hash || !g_wcl_draw) return false;
    PortApi::DrawInfo di = draw ? *draw : PortApi::DrawInfo();
    di.is_pixel_shader = (shader_hash == t_cur_ps_hash);
    di.partner_shader_hash = di.is_pixel_shader ? t_cur_vs_hash : t_cur_ps_hash;
    if (cmd_list) {
        const CmdRT& rt = CmdRt(cmd_list);
        if (rt.valid) {
            di.depth_active = rt.has_dsv;
            di.depth_known = true;
        }
    }
    const uint64_t lookup = AliasShaderHash(shader_hash);
    const bool     ran = PortApi::DispatchShaderOverride(lookup, &di);
    if (g_debug && lookup != shader_hash) {
        static std::mutex         m;
        static std::set<uint64_t> seen;
        bool                      fr;
        {
            std::lock_guard l(m);
            fr = ProbeArmed() && seen.insert(shader_hash).second;
        }
        if (fr) logger::write("SHADER-ALIAS-RESULT: dx12 %016llx -> dx11 %016llx  ran=%d skip=%d",
            (unsigned long long)shader_hash, (unsigned long long)lookup,
            (int)ran, (int)di.skip);
    }
    return ran && di.skip;
}
