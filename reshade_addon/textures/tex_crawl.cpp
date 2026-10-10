#include "tex_crawl.h"
#include "tex_lod.h"
#include "tex_look.h"
#include "../config.h"
#include "../../src/log.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {
std::atomic<bool>                                    g_on{ false };
std::mutex                                           g_mtx;
std::unordered_map<uint64_t, std::vector<uint32_t>> g_seen;          // mesh place -> every texture drawn there
bool                                                 g_dirty = false; // a texture seen since the last save -- under g_mtx
uint64_t                                             g_new_at = 0;    // GetTickCount64 of the latest one
uint64_t                                             g_saved_at = 0;  // GetTickCount64 of the last save
std::wstring                                         g_file;          // this session's proofs file, named at its first save

void save()
{
    std::unordered_map<uint64_t, std::vector<uint32_t>> seen;
    {
        std::lock_guard l(g_mtx);
        seen = g_seen;
    }
    std::unordered_set<uint32_t> textures;
    unsigned                     placed = 0;
    unsigned                     split = 0;
    for (const auto& kv : seen) {
        uint32_t one = 0;
        bool     two = false;
        for (const uint32_t h : kv.second) {
            const uint32_t known = TexLodKnown(h);
            const uint32_t top = known ? known : h;
            textures.insert(top);
            two = two || (one && one != top);
            if (!one) one = top;
        }
        if (two) {
            ++split;
            continue;
        }
        TexLookAddMeshPlace(kv.first, one);
        ++placed;
    }
    if (g_data_dir.empty()) {
        logger::write("TexCrawl: %u mesh place(s) kept; no data folder, so no proofs written", placed);
        return;
    }
    if (g_file.empty()) {
        const std::filesystem::path dir = std::filesystem::path(g_data_dir) / L"crawl";
        std::error_code             ec;
        std::filesystem::create_directories(dir, ec);
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t name[48];
        swprintf_s(name, L"crawl_%04u%02u%02u_%02u%02u%02u.lodcache", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
            st.wSecond);
        g_file = (dir / name).wstring();
    }
    const std::wstring& path = g_file;
    const bool          ok = TexLodExport(
        path, [](uint32_t high, void* u) { return static_cast<std::unordered_set<uint32_t>*>(u)->count(high) != 0; },
        &textures);
    logger::write("TexCrawl: %u of %zu mesh place(s) kept, %u drawn with two textures; %zu character texture(s), their "
                  "proofs %s %S",
        placed, seen.size(), split, textures.size(), ok ? "written to" : "NOT written to", path.c_str());
}
}

bool TexCrawlOn()
{
    return g_on.load(std::memory_order_relaxed);
}

void TexCrawlSet(bool on)
{
    g_on.store(on, std::memory_order_relaxed);
    logger::write("TexCrawl: %s", on ? "on -- every texture's mips proven, every character draw's slots recorded" : "off");
    if (!on) TexCrawlFlushIfQuiet(0);
}

void TexCrawlNote(uint64_t place, uint32_t hash)
{
    std::lock_guard        l(g_mtx);
    std::vector<uint32_t>& v = g_seen[place];
    if (std::find(v.begin(), v.end(), hash) != v.end()) return;
    v.push_back(hash);
    g_dirty = true;
    g_new_at = GetTickCount64();
}

void TexCrawlFlushIfQuiet(unsigned quiet_ms)
{
    {
        std::lock_guard l(g_mtx);
        const uint64_t now = GetTickCount64();
        if (!g_dirty || (now - g_new_at < quiet_ms && now - g_saved_at < 60000)) return;
        g_dirty = false;
        g_saved_at = now;
    }
    save();
}
