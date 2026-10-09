// port_api: what their parser built, indexed once per parse and answered without the engine lock.
#include "port_api_internal.h"
#include "globals.h"
#include "IniHandler.h"
#include "CommandList.h"
#include "ResourceHash.h"
#include "HackerDevice.h"
#include "port_keygate.h"
#include "../../src/log.h"

#include <atomic>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace PortApi {

static std::atomic<const std::unordered_set<uint64_t>*> g_override_hashes{ nullptr };
static void                                             RebuildOverrideHashes()
{
    auto* s = new std::unordered_set<uint64_t>();
    for (const auto& kv : G->mShaderOverrideMap) s->insert(kv.first);
    g_override_hashes.store(s, std::memory_order_release);
    logger::write("PORT: shader-override hash set rebuilt: %zu entries", s->size());
}
bool OverrideHashKnown(uint64_t h)
{
    const auto* s = g_override_hashes.load(std::memory_order_acquire);
    return s && s->find(h) != s->end();
}

enum : uint8_t { kSkOther = 0,
    kSkLoaderCb,
    kSkMultiplierCb,
    kSkOffsets,
    kSkScale };

struct SkNsLess { // THEIR relation (IniHandler.cpp WStringInsensitiveLess), not a towlower of our own
    bool operator()(const std::wstring& a, const std::wstring& b) const
    {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    }
};

struct SkIndex {
    static const uint32_t kNoNs = 0xFFFFFFFFu;
    struct Row {
        unsigned     hash;
        uint32_t     ns_id; // index into `ns`; kNoNs for a section with no namespace
        uint8_t      kind;
        std::wstring leaf; // verbatim; only the generic `which` query looks inside it
    };
    std::vector<std::wstring>                           ns;
    std::map<std::wstring, uint32_t, SkNsLess>          ns_by_name;
    std::vector<Row>                                    rows;
    std::unordered_map<unsigned, std::vector<uint32_t>> by_hash; // hash -> row indices
    std::unordered_set<unsigned>                        callback_hashes;
    std::unordered_map<uint32_t, unsigned>              cb_hash_by_ns;   // ns_id -> its callback hash
    std::unordered_map<uint64_t, unsigned>              hash_by_ns_kind; // (ns_id<<8)|kind -> hash

    uint32_t FindNs(const wchar_t* want) const
    {
        auto it = ns_by_name.find(want);
        return (it == ns_by_name.end()) ? kNoNs : it->second;
    }
    uint32_t InternNs(const std::wstring& n)
    {
        auto it = ns_by_name.find(n);
        if (it != ns_by_name.end()) return it->second;
        ns.push_back(n);
        const uint32_t id = (uint32_t)ns.size() - 1;
        ns_by_name.emplace(n, id);
        return id;
    }
};
static std::atomic<const SkIndex*> g_sk_index{ nullptr };

static uint8_t SkClassifyLeaf(const wchar_t* leaf)
{
    if (!_wcsicmp(leaf, L"ShapeKeyLoaderCallback")) return kSkLoaderCb;
    if (!_wcsicmp(leaf, L"ShapeKeyMultiplierCallback")) return kSkMultiplierCb;
    if (!_wcsicmp(leaf, L"ShapeKeyOffsets")) return kSkOffsets;
    if (!_wcsicmp(leaf, L"ShapeKeyScale")) return kSkScale;
    return kSkOther;
}

static void RebuildShapeKeyIndex()
{
    auto* ix = new SkIndex();
    for (auto& kv : G->mTextureOverrideMap) {
        for (TextureOverride& to : kv.second) {
            SkIndex::Row r;
            r.hash = kv.first;
            r.ns_id = SkIndex::kNoNs;
            std::wstring nsw;
            if (get_section_namespace(to.ini_section.c_str(), &nsw) && !nsw.empty())
                r.ns_id = ix->InternNs(nsw);
            const size_t bs = to.ini_section.find_last_of(L'\\');
            r.leaf = (bs == std::wstring::npos) ? to.ini_section : to.ini_section.substr(bs + 1);
            r.kind = SkClassifyLeaf(r.leaf.c_str());

            ix->by_hash[r.hash].push_back((uint32_t)ix->rows.size());
            if (r.kind == kSkLoaderCb || r.kind == kSkMultiplierCb) {
                ix->callback_hashes.insert(r.hash);
                // Both callbacks carry the same hash, so whichever lands first is the same answer.
                if (r.ns_id != SkIndex::kNoNs) ix->cb_hash_by_ns.emplace(r.ns_id, r.hash);
            }
            if (r.ns_id != SkIndex::kNoNs && r.kind != kSkOther)
                ix->hash_by_ns_kind.emplace(((uint64_t)r.ns_id << 8) | r.kind, r.hash);
            ix->rows.push_back(std::move(r));
        }
    }
    g_sk_index.store(ix, std::memory_order_release);
    logger::write("PORT: shapekey section index rebuilt: %zu sections, %zu namespaces, %zu hashes, "
                  "%zu callback hashes",
        ix->rows.size(), ix->ns.size(), ix->by_hash.size(),
        ix->callback_hashes.size());
}

bool TextureOverrideHashKnown(uint32_t h)
{
    const SkIndex* ix = g_sk_index.load(std::memory_order_acquire);
    return !ix || ix->by_hash.find(h) != ix->by_hash.end();
}

void RebuildParseSnapshots()
{
    RebuildOverrideHashes();
    RebuildShapeKeyIndex();
}

std::unordered_map<std::wstring, TextureOverride*>& SectionIndex()
{
    static auto*    m = new std::unordered_map<std::wstring, TextureOverride*>();
    static unsigned built_for = 0;
    if (built_for != g_generation) {
        built_for = g_generation;
        m->clear();
        for (auto& kv : G->mTextureOverrideMap)
            for (TextureOverride& to : kv.second)
                m->emplace(to.ini_section, &to);
    }
    return *m;
}

std::unordered_map<std::wstring, ExplicitCommandListSection*>& ExplicitIndex()
{
    static auto*    m = new std::unordered_map<std::wstring, ExplicitCommandListSection*>();
    static unsigned built_for = 0;
    if (built_for != g_generation) {
        built_for = g_generation;
        m->clear();
        for (auto& kv : explicitCommandListSections)
            m->emplace(kv.first, &kv.second);
    }
    return *m;
}

std::unordered_map<const void*, std::wstring>& ExplicitNameIndex()
{
    static auto*    m = new std::unordered_map<const void*, std::wstring>();
    static unsigned built_for = 0;
    if (built_for != g_generation) {
        built_for = g_generation;
        m->clear();
        for (auto& kv : explicitCommandListSections)
            m->emplace((const void*)&kv.second, kv.first);
    }
    return *m;
}

std::unordered_map<const void*, std::wstring>& CustomResourceNameIndex()
{
    static auto*    m = new std::unordered_map<const void*, std::wstring>();
    static unsigned built_for = 0;
    if (built_for != g_generation) {
        built_for = g_generation;
        m->clear();
        for (auto& kv : customResources)
            m->emplace((const void*)&kv.second, kv.first);
    }
    return *m;
}

bool HasSection(const wchar_t* fq_section)
{
    if (!g_loaded || !fq_section) return false;
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    const std::wstring                    w = fq_section;
    return SectionIndex().count(w) != 0 || ExplicitIndex().count(w) != 0;
}

bool GetSectionValue(const wchar_t* fq_section, const wchar_t* key, wchar_t* out, size_t out_chars)
{
    if (!out || !out_chars) return false;
    out[0] = 0;
    if (!g_loaded || !fq_section || !key) return false;

    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    IniSectionVector*                     sv = nullptr;
    GetIniSection(&sv, fq_section); // theirs; empty vector if the section is unknown
    if (!sv) return false;
    for (const IniLine& l : *sv) {
        if (_wcsicmp(l.first.c_str(), key) != 0) continue;
        wcsncpy_s(out, out_chars, l.second.c_str(), _TRUNCATE);
        return true; // FIRST wins, as their own lookups do
    }
    return false;
}

bool TextureOverrideDeclaresHash(unsigned hash)
{
    if (!g_loaded || !hash) return false;
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    return G->mTextureOverrideMap.find(hash) != G->mTextureOverrideMap.end();
}

bool ShapeKeyCallbackDeclaresHash(unsigned hash)
{
    if (!g_loaded || !hash) return false;
    const SkIndex* ix = g_sk_index.load(std::memory_order_acquire);
    return ix && ix->callback_hashes.find(hash) != ix->callback_hashes.end();
}

unsigned ModDeclaredShapeKeyHash(const wchar_t* ns)
{
    if (!g_loaded || !ns || !*ns) return 0;
    const SkIndex* ix = g_sk_index.load(std::memory_order_acquire);
    if (!ix) return 0;
    const uint32_t id = ix->FindNs(ns);
    if (id == SkIndex::kNoNs) return 0;
    auto it = ix->cb_hash_by_ns.find(id);
    return (it == ix->cb_hash_by_ns.end()) ? 0u : it->second;
}

unsigned ModDeclaredShapeKeySectionHash(const wchar_t* ns, const wchar_t* which)
{
    if (!g_loaded || !ns || !*ns || !which || !*which) return 0;
    const SkIndex* ix = g_sk_index.load(std::memory_order_acquire);
    if (!ix) return 0;
    const uint32_t id = ix->FindNs(ns);
    if (id == SkIndex::kNoNs) return 0;

    // The two names the library actually uses resolve to a leaf kind, so this is one map lookup.
    uint8_t kind = kSkOther;
    if (!_wcsicmp(which, L"scale"))
        kind = kSkScale;
    else if (!_wcsicmp(which, L"offsets"))
        kind = kSkOffsets;
    if (kind != kSkOther) {
        auto it = ix->hash_by_ns_kind.find(((uint64_t)id << 8) | kind);
        return (it == ix->hash_by_ns_kind.end()) ? 0u : it->second;
    }

    std::wstring sub = which;
    for (wchar_t& c : sub) c = towlower(c);
    for (const SkIndex::Row& r : ix->rows) {
        if (r.ns_id != id) continue;
        if (r.kind == kSkLoaderCb || r.kind == kSkMultiplierCb) continue;
        std::wstring leaf = r.leaf;
        for (wchar_t& c : leaf) c = towlower(c);
        if (leaf.find(L"shapekey") != std::wstring::npos && leaf.find(sub) != std::wstring::npos)
            return r.hash;
    }
    return 0;
}

void ForEachBufferResize(void (*cb)(unsigned hash, unsigned stride, unsigned count, const wchar_t* section,
                             void* user),
    void* user)
{
    if (!g_loaded || !cb) return;
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());

    for (auto& kv : G->mTextureOverrideMap) {
        for (TextureOverride& to : kv.second) {
            if (to.override_byte_width <= 0) continue; // -1 = this section declares no resize
            cb(kv.first, 1u, (unsigned)to.override_byte_width, to.ini_section.c_str(), user);
        }
    }
}

void ForEachTextureOverrideConfig(void (*cb)(unsigned hash, int byte_width, int num_elements,
                                      int match_priority, const wchar_t* section, void* user),
    void* user)
{
    if (!g_loaded || !cb) return;
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    for (auto& kv : G->mTextureOverrideMap) {
        for (TextureOverride& to : kv.second) {
            cb(kv.first, to.override_byte_width, to.override_num_elements, to.priority,
                to.ini_section.c_str(), user);
        }
    }
}

bool AllowBufferResize()
{
    return g_loaded && G->allow_buffer_resize;
}

static bool ListReaches(const CommandList* cl, const void* inner, int depth)
{
    if (!cl || depth > 8) return false;
    for (const auto& c : cl->commands) {
        const CommandListCommand* cmd = MigotoPort::UnwrapObserved(c.get());
        if (const RunExplicitCommandList* e = dynamic_cast<const RunExplicitCommandList*>(cmd)) {
            if ((const void*)e->command_list_section == inner) return true;
            if (e->command_list_section &&
                (ListReaches(&e->command_list_section->command_list, inner, depth + 1) ||
                    ListReaches(&e->command_list_section->post_command_list, inner, depth + 1)))
                return true;
        } else if (const RunLinkedCommandList* r = dynamic_cast<const RunLinkedCommandList*>(cmd)) {
            if (ListReaches(r->link, inner, depth + 1)) return true;
        }
    }
    return false;
}

bool ListNests(const void* outer_section, const void* inner_section)
{
    if (!outer_section || !inner_section || outer_section == inner_section) return false;
    const auto* o = static_cast<const ExplicitCommandListSection*>(outer_section);
    return ListReaches(&o->command_list, inner_section, 0) ||
        ListReaches(&o->post_command_list, inner_section, 0);
}

bool ModDeclaresHash(const wchar_t* ns, unsigned hash)
{
    if (!g_loaded || !hash || !ns || !*ns) return false;
    const SkIndex* ix = g_sk_index.load(std::memory_order_acquire);
    if (!ix) return false;
    auto bh = ix->by_hash.find(hash);
    if (bh == ix->by_hash.end()) return false;
    const uint32_t id = ix->FindNs(ns);
    if (id == SkIndex::kNoNs) return false;
    for (uint32_t ri : bh->second)
        if (ix->rows[ri].ns_id == id) return true;
    return false;
}

void GetIniParams(float* out_rgba, int texels)
{
    if (!out_rgba || texels <= 0) return;
    for (int i = 0; i < texels * 4; ++i) out_rgba[i] = 0.f;
    if (!g_loaded) return;
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    const int                             n = (int)G->iniParams.size();
    for (int i = 0; i < texels && i < n; ++i) {
        out_rgba[i * 4 + 0] = G->iniParams[i].x;
        out_rgba[i * 4 + 1] = G->iniParams[i].y;
        out_rgba[i * 4 + 2] = G->iniParams[i].z;
        out_rgba[i * 4 + 3] = G->iniParams[i].w;
    }
}

bool SectionFolder(const wchar_t* fq_section, wchar_t* out, size_t out_chars)
{
    if (!out || !out_chars) return false;
    out[0] = 0;
    if (!g_loaded || !fq_section) return false;
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    std::wstring                          folder;
    if (!PortSectionFolder(fq_section, &folder) || folder.empty()) return false;
    wcsncpy_s(out, out_chars, folder.c_str(), _TRUNCATE);
    return true;
}

void ForEachSectionKey(const wchar_t* fq_section,
    void (*cb)(const wchar_t* key, const wchar_t* val, void* user), void* user)
{
    if (!g_loaded || !fq_section || !cb) return;
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    IniSectionVector*                     sv = nullptr;
    GetIniSection(&sv, fq_section);
    if (!sv) return;
    for (const IniLine& l : *sv) // file order: a command list depends on it
        cb(l.first.c_str(), l.second.c_str(), user);
}

static void ForEachSectionName(const std::function<void(const std::wstring&)>& fn)
{
    for (auto& kv : SectionIndex()) fn(kv.first);
    for (auto& kv : ExplicitIndex()) fn(kv.first);
}

bool ResolveSection(const wchar_t* ns, const wchar_t* ns_hint2, const wchar_t* section,
    wchar_t* out, size_t out_chars)
{
    if (!out || !out_chars) return false;
    out[0] = 0;
    if (!g_loaded || !section || !*section) return false;

    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    const std::wstring                    sec = section;

    auto ends_with = [](const std::wstring& hay, const std::wstring& needle) {
        if (hay.size() < needle.size()) return false;
        return _wcsnicmp(hay.c_str() + (hay.size() - needle.size()), needle.c_str(),
                   needle.size()) == 0;
    };

    if (ns && *ns) {
        std::wstring want = std::wstring(1, L'\\') + ns + std::wstring(1, L'\\') + sec;
        std::wstring hit_name;
        int          matches = 0;
        ForEachSectionName([&](const std::wstring& name) {
            if (!ends_with(name, want)) return;
            ++matches;
            hit_name = name;
        });
        if (matches == 1) {
            wcsncpy_s(out, out_chars, hit_name.c_str(), _TRUNCATE);
            return true;
        }
        if (matches > 1) {
            logger::write("PORT: section '%ls' is AMBIGUOUS under namespace '%ls' (%d matches) -- refusing "
                          "to guess; pass the full namespace",
                section, ns, matches);
            return false;
        }
    }
    if (ns_hint2 && *ns_hint2) {
        std::wstring hint = ns_hint2;
        for (auto& ch : hint)
            if (ch == L'/') ch = L'\\'; // their names use backslash separators
        const std::wstring tail = std::wstring(1, L'\\') + sec;
        std::wstring       hit_name;
        int                matches = 0;
        ForEachSectionName([&](const std::wstring& name) {
            if (!ends_with(name, tail)) return;
            std::wstring lower = name;
            for (auto& ch : lower) ch = (wchar_t)towlower(ch);
            if (lower.find(hint) == std::wstring::npos) return;
            ++matches;
            hit_name = name;
        });
        if (matches == 1) {
            wcsncpy_s(out, out_chars, hit_name.c_str(), _TRUNCATE);
            return true;
        }
        if (matches > 1)
            logger::write("PORT: section '%ls' is AMBIGUOUS under path hint '%ls' (%d matches) -- refusing "
                          "to guess",
                section, ns_hint2, matches);
    }
    return false;
}

bool NamespaceForIni(const wchar_t* abs_ini_path, wchar_t* out, size_t out_chars)
{
    if (!out || !out_chars) return false;
    out[0] = 0;
    if (!g_loaded || !abs_ini_path || !*abs_ini_path) return false;
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    std::wstring                          ns;
    if (!PortIniNamespace(abs_ini_path, &ns)) return false;
    wcsncpy_s(out, out_chars, ns.c_str(), _TRUNCATE);
    return true;
}

void ForEachTextureOverride(void (*cb)(uint32_t, const wchar_t*, bool, const wchar_t*, void*), void* user)
{
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    if (!g_loaded || !cb) return;
    for (auto& kv : G->mTextureOverrideMap) {
        for (TextureOverride& to : kv.second) {
            const bool   ranged = (to.match_first_index.op != FuzzyMatchOp::ALWAYS);
            std::wstring folder;
            PortSectionFolder(to.ini_section.c_str(), &folder); // empty is fine: a global section
            cb(kv.first, to.ini_section.c_str(), ranged, folder.c_str(), user);
        }
    }
}

bool TextureHashHasSections(uint32_t hash)
{
    return TextureOverrideHashKnown(hash);
}

} // namespace PortApi
// Is every command of `cl` one a skip section may hold (see ForEachSkipTextureOverride)? Counts the skips.
static bool SkipSectionBodyIsSafe(const CommandList* cl, int* skips)
{
    if (!cl) return true;
    for (const auto& c : cl->commands) {
        CommandListCommand* cmd = MigotoPort::UnwrapObserved(c.get());
        if (dynamic_cast<SkipCommand*>(cmd)) {
            ++*skips;
            continue;
        }
        if (dynamic_cast<AssignmentCommand*>(cmd) || dynamic_cast<CommandPlaceholder*>(cmd)) continue;
        if (auto* ic = dynamic_cast<IfCommand*>(cmd)) {
            if (SkipSectionBodyIsSafe(ic->true_commands_pre.get(), skips) && SkipSectionBodyIsSafe(ic->true_commands_post.get(), skips) &&
                SkipSectionBodyIsSafe(ic->false_commands_pre.get(), skips) && SkipSectionBodyIsSafe(ic->false_commands_post.get(), skips)) continue;
            return false;
        }
        return false;
    }
    return true;
}
namespace PortApi {
void ForEachSkipTextureOverride(void (*cb)(uint32_t, const wchar_t*, void*), void* user)
{
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    if (!g_loaded || !cb) return;
    for (auto& kv : G->mTextureOverrideMap) {
        for (TextureOverride& to : kv.second) {
            if (to.match_first_index.op != FuzzyMatchOp::ALWAYS) continue; // ranged: a component's own section
            int skips = 0;
            int post_skips = 0;
            if (!SkipSectionBodyIsSafe(&to.command_list, &skips) || !skips) continue;
            if (!SkipSectionBodyIsSafe(&to.post_command_list, &post_skips)) continue;
            cb(kv.first, to.ini_section.c_str(), user);
        }
    }
}
} // namespace PortApi
