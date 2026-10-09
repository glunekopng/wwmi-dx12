// ===========================================================================
//  tex_lod.cpp -- see tex_lod.h. BC decode (bcdec.h) -> 32x32 RGB signature ->
//  NCC content match -> lower->target links -> one line per target in our cache
//  file, each low under it with its flags (mip, exact, chain), and one line of degenerate lows.
// ===========================================================================
#define BCDEC_IMPLEMENTATION
#include "../../third_party/bcdec/bcdec.h"
#include "tex_lod.h"
#include "../../src/log.h"
#include "../../src/dxgi_bc.h"
#include "../config.h"
#include <windows.h>
#include <atomic>
#include <unordered_map>
#include <mutex>
#include <vector>
#include <cmath>
#include <cstdio>
#include <share.h>
#include <set>
#include <shared_mutex>
#include <string>
#include <algorithm>
#include <map>
#include <cstring>
#include <cwchar>

namespace {

constexpr int   SIG = 32;                // 32x32 grid signature (3072 samples)
constexpr float NCC_MIN = 0.97f;         // sanity correlation (validated diffuse pair ~0.9997)
constexpr float RMSE_MAX = 8.0f;         // absolute per-texel RMSE (0-255); true LOD is ~pixel-identical
constexpr float RMSE_UNMEASURED = 255.f; // a look-alike whose content was never compared
constexpr float MIN_STD = 18.0f;         // require spatial structure; flat normal/mask maps can't disambiguate

struct LodSig {
    uint8_t  v[SIG * SIG * 3];
    uint32_t w;
    uint32_t h;
    uint32_t fmt;
    bool     is_target;
};

std::mutex                           g_mtx;  // WRITERS only (sigs, records, table writes, dirs); see g_tab
std::unordered_map<uint32_t, LodSig> g_sigs; // hash -> signature

struct Entry {
    bool                  exact = false;
    std::vector<uint32_t> highs;
};
struct Slot {
    std::atomic<uint32_t>     low{ 0 };
    std::atomic<const Entry*> e{ nullptr };
};
struct EntryTab {
    uint32_t bits;
    Slot*    s;
};
EntryTab* tab_new(uint32_t bits)
{
    return new EntryTab{ bits, new Slot[size_t(1) << bits]() };
}
std::atomic<EntryTab*> g_tab{ tab_new(17) };
uint32_t               g_tab_used = 0; // slots taken, tombstones included -- under g_mtx
uint32_t               tab_slot(uint32_t low, uint32_t bits)
{
    return (low * 2654435761u) >> (32 - bits);
}
const Entry* entry_of(uint32_t low)
{
    if (!low) return nullptr;
    const EntryTab* tb = g_tab.load(std::memory_order_acquire);
    const uint32_t  mask = (1u << tb->bits) - 1;
    for (uint32_t i = tab_slot(low, tb->bits), n = 0; n <= mask; ++n, i = (i + 1) & mask) {
        const uint32_t k = tb->s[i].low.load(std::memory_order_acquire);
        if (!k) return nullptr;
        if (k == low) return tb->s[i].e.load(std::memory_order_acquire);
    }
    return nullptr;
}
void tab_grow()
{ // caller holds g_mtx
    const EntryTab* old = g_tab.load(std::memory_order_relaxed);
    EntryTab*       tb = tab_new(old->bits + 1);
    const uint32_t  mask = (1u << tb->bits) - 1;
    uint32_t        used = 0;
    for (uint32_t k = 0; k < (1u << old->bits); ++k) {
        const uint32_t low = old->s[k].low.load(std::memory_order_relaxed);
        const Entry*   e = old->s[k].e.load(std::memory_order_relaxed);
        if (!low || !e) continue; // empty or a tombstone: not carried over
        uint32_t i = tab_slot(low, tb->bits);
        while (tb->s[i].low.load(std::memory_order_relaxed)) i = (i + 1) & mask;
        tb->s[i].e.store(e, std::memory_order_relaxed);
        tb->s[i].low.store(low, std::memory_order_relaxed);
        ++used;
    }
    g_tab_used = used;
    g_tab.store(tb, std::memory_order_release);
    if (g_debug) logger::write("TexLod: table grown to %u slots (%u lows)", mask + 1, used);
}
// Caller holds g_mtx. A null entry takes the low out; its slot keeps the key as a tombstone.
void tab_set(uint32_t low, const Entry* e)
{
    if (!low) return;
    if (e && (uint64_t)(g_tab_used + 1) * 2 > (1ull << g_tab.load(std::memory_order_relaxed)->bits)) tab_grow();
    const EntryTab* tb = g_tab.load(std::memory_order_relaxed);
    const uint32_t  mask = (1u << tb->bits) - 1;
    for (uint32_t i = tab_slot(low, tb->bits), n = 0; n <= mask; ++n, i = (i + 1) & mask) {
        const uint32_t k = tb->s[i].low.load(std::memory_order_relaxed);
        if (k && k != low) continue;
        if (!k && !e) return;
        tb->s[i].e.store(e, std::memory_order_release);
        if (!k) {
            tb->s[i].low.store(low, std::memory_order_release);
            ++g_tab_used;
        }
        return;
    }
}
std::wstring                                       g_cache_dir; // OURS, outside the mod tree -- the only place we read or write
std::vector<std::pair<std::wstring, std::wstring>> g_imported;  // seed taken -> its DISABLED_ name, after the next write

struct Link {
    uint32_t high = 0;
    uint32_t mip = 0;
    bool     exact = false;
    bool     chain = false;
    float    rmse = RMSE_UNMEASURED;
};
struct Rec {
    std::vector<Link> links;
};
std::unordered_map<uint32_t, Rec>         g_records; // low -> every target it belongs to
std::unordered_map<uint32_t, uint32_t>    g_refuted;
std::set<uint32_t>                        g_degenerate;            // identical to the mips of two targets or more: never swapped
std::atomic<const std::vector<uint32_t>*> g_degen_snap{ nullptr }; // g_degenerate for lock-free readers; old ones kept

void publish_degenerate()
{ // caller holds g_mtx
    g_degen_snap.store(new std::vector<uint32_t>(g_degenerate.begin(), g_degenerate.end()), std::memory_order_release);
}

bool exact(const Rec& r)
{
    return !r.links.empty() && r.links.front().exact;
}
Link* link_to(Rec& r, uint32_t high)
{
    for (Link& k : r.links)
        if (k.high == high) return &k;
    return nullptr;
}
bool exact_record(uint32_t low)
{
    auto it = g_records.find(low);
    return it != g_records.end() && exact(it->second);
}
uint32_t single_high(uint32_t low)
{
    auto it = g_records.find(low);
    return it == g_records.end() || it->second.links.size() != 1 ? 0 : it->second.links.front().high;
}
// Of two hashes that are one texture at two resolutions (one an exact low of the other), the higher; else 0.
uint32_t one_texture(uint32_t a, uint32_t b)
{
    if (exact_record(b) && single_high(b) == a) return a;
    if (exact_record(a) && single_high(a) == b) return b;
    return 0;
}

// Decode the single texel (px,py) of a BC image into rgb[3]. Decodes only the block that contains it.
void sample_px(uint32_t fmt, const uint8_t* bc, size_t bc_size, uint32_t blocksW,
    uint32_t px, uint32_t py, uint8_t rgb[3])
{
    uint32_t bx = px >> 2;
    uint32_t by = py >> 2;
    uint32_t inx = px & 3;
    uint32_t iny = py & 3;
    uint32_t bb = DxgiBcBlockBytes(fmt);
    size_t   off = ((size_t)by * blocksW + bx) * bb;
    if (off + bb > bc_size) {
        rgb[0] = rgb[1] = rgb[2] = 0;
        return;
    }
    const uint8_t* blk = bc + off;
    if (fmt >= 94 && fmt <= 99) {
        uint8_t b[64];
        bcdec_bc7(blk, b, 16);
        const uint8_t* p = b + (iny * 4 + inx) * 4;
        rgb[0] = p[0];
        rgb[1] = p[1];
        rgb[2] = p[2];
    } else if (fmt >= 70 && fmt <= 72) {
        uint8_t b[64];
        bcdec_bc1(blk, b, 16);
        const uint8_t* p = b + (iny * 4 + inx) * 4;
        rgb[0] = p[0];
        rgb[1] = p[1];
        rgb[2] = p[2];
    } else if (fmt >= 76 && fmt <= 78) {
        uint8_t b[64];
        bcdec_bc3(blk, b, 16);
        const uint8_t* p = b + (iny * 4 + inx) * 4;
        rgb[0] = p[0];
        rgb[1] = p[1];
        rgb[2] = p[2];
    } else if (fmt >= 82 && fmt <= 84) {
        uint8_t b[32];
        bcdec_bc5(blk, b, 8);
        const uint8_t* p = b + (iny * 4 + inx) * 2;
        rgb[0] = p[0];
        rgb[1] = p[1];
        rgb[2] = 0;
    } // BC5 = RG
    else {
        rgb[0] = rgb[1] = rgb[2] = 0;
    }
}

LodSig make_sig(uint32_t fmt, const uint8_t* bc, size_t bc_size, uint32_t w, uint32_t h, bool tgt)
{
    LodSig s;
    s.w = w;
    s.h = h;
    s.fmt = fmt;
    s.is_target = tgt;
    uint32_t blocksW = (w + 3) / 4;
    for (int gy = 0; gy < SIG; ++gy)
        for (int gx = 0; gx < SIG; ++gx) {
            uint32_t px = (uint32_t)(((2 * gx + 1) * (uint64_t)w) / (2 * SIG));
            if (px >= w) px = w - 1;
            uint32_t py = (uint32_t)(((2 * gy + 1) * (uint64_t)h) / (2 * SIG));
            if (py >= h) py = h - 1;
            sample_px(fmt, bc, bc_size, blocksW, px, py, &s.v[(gy * SIG + gx) * 3]);
        }
    return s;
}

float ncc(const LodSig& a, const LodSig& b)
{
    double ma = 0;
    double mb = 0;
    for (int i = 0; i < SIG * SIG * 3; ++i) {
        ma += a.v[i];
        mb += b.v[i];
    }
    ma /= SIG * SIG * 3;
    mb /= SIG * SIG * 3;
    double num = 0;
    double da = 0;
    double db = 0;
    for (int i = 0; i < SIG * SIG * 3; ++i) {
        double fa = a.v[i] - ma;
        double fb = b.v[i] - mb;
        num += fa * fb;
        da += fa * fa;
        db += fb * fb;
    }
    return (float)(num / (std::sqrt(da * db) + 1e-9));
}

float rmse(const LodSig& a, const LodSig& b)
{
    double s = 0;
    for (int i = 0; i < SIG * SIG * 3; ++i) {
        double d = (double)a.v[i] - b.v[i];
        s += d * d;
    }
    return (float)std::sqrt(s / (SIG * SIG * 3));
}

// Spatial std of a signature. Near-flat textures (normal/mask maps) can't be disambiguated by content.
float sig_std(const LodSig& a)
{
    double m = 0;
    for (int i = 0; i < SIG * SIG * 3; ++i) m += a.v[i];
    m /= SIG * SIG * 3;
    double v = 0;
    for (int i = 0; i < SIG * SIG * 3; ++i) {
        double d = a.v[i] - m;
        v += d * d;
    }
    v /= SIG * SIG * 3;
    return (float)std::sqrt(v);
}

// True LOD pair test: enough structure on both, near-identical content (low RMSE), correlated.
bool is_lod_match(const LodSig& big, const LodSig& sm, float& out_rmse)
{
    if (sig_std(big) < MIN_STD || sig_std(sm) < MIN_STD) return false;
    float r = rmse(big, sm);
    if (r > RMSE_MAX) {
        out_rmse = r;
        return false;
    }
    if (ncc(big, sm) < NCC_MIN) {
        out_rmse = r;
        return false;
    }
    out_rmse = r;
    return true;
}

bool same_aspect(uint32_t w1, uint32_t h1, uint32_t w2, uint32_t h2)
{
    return (uint64_t)w1 * h2 == (uint64_t)w2 * h1;
}

bool is_lod_parent(const LodSig& big, const LodSig& sm)
{
    if (!same_aspect(big.w, big.h, sm.w, sm.h)) return false; // proportions first, before content
    if (big.w <= sm.w || big.h <= sm.h) return false;         // big strictly larger in both dims
    if (big.w % sm.w || big.h % sm.h) return false;
    uint32_t rx = big.w / sm.w;
    uint32_t ry = big.h / sm.h;
    return rx == ry && (rx & (rx - 1)) == 0; // uniform power-of-two downscale
}

std::wstring cache_path()
{
    return g_cache_dir.empty() ? std::wstring() : (g_cache_dir + L"\\lod_tex_cache.lodcache");
}

const char* const kHeader =
    "; wwmi-dx12 LOD texture matches. One line per target:  <target> = <low> <flags> | <low> <flags> ...\n"
    "; A low is a lower-resolution texture the game streams in; it is swapped as its target's mod texture.\n"
    ";   mip=K         the low is the target's mip K (0: the level is not known)\n"
    ";   exact=1       proven from the game's own data: the target's own mip, or a stream copy\n"
    ";   exact=0       only looks alike, dropped once a low is proven to be that mip of the target: used with\n"
    ";                 lod_resemblance=1, and only in draws of a mod that declares the target\n"
    ";   chain=1       learned through a chain of stream copies; a look-alike learned so has the rmse of the one it came through\n"
    ";   rmse=R        a look-alike's difference from identical, 0-255 per texel (none: never measured); it needs R <= %.0f and correlation >= %.2f\n"
    "; degenerate = <low> <low> ...  identical to the mips of more than one target: never swapped, never learned again\n";

void write_entry(FILE* f, uint32_t low, const Link& k)
{
    fprintf(f, "%08x mip=%u exact=%d chain=%d", low, k.mip, (int)k.exact, (int)k.chain);
    if (!k.exact && k.rmse != RMSE_UNMEASURED) fprintf(f, " rmse=%.2f", k.rmse);
}

bool     g_dirty = false; // learned since the file was last written -- under g_mtx
uint64_t g_dirty_at = 0;  // GetTickCount64 of the latest change

void mark_dirty()
{ // caller holds g_mtx
    g_dirty = true;
    g_dirty_at = GetTickCount64();
}

// The whole cache from g_records, one line per target, then g_degenerate, through a temp file. Caller holds g_mtx.
void rewrite_cache()
{
    const std::wstring path = cache_path();
    if (path.empty() || !g_texlod_cache) return;
    std::map<uint32_t, std::vector<std::pair<uint32_t, const Link*>>> by_target;
    for (auto& kv : g_records)
        for (const Link& k : kv.second.links) by_target[k.high].emplace_back(kv.first, &k);
    const std::wstring tmp = path + L".tmp";
    FILE*              f = _wfsopen(tmp.c_str(), L"w", _SH_DENYNO);
    if (!f) {
        logger::write("TexLod: cannot write %S", tmp.c_str());
        return;
    }
    fprintf(f, kHeader, RMSE_MAX, NCC_MIN);
    for (auto& t : by_target) {
        std::sort(t.second.begin(), t.second.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        fprintf(f, "%08x = ", t.first);
        for (size_t i = 0; i < t.second.size(); ++i) {
            if (i) fputs(" | ", f);
            write_entry(f, t.second[i].first, *t.second[i].second);
        }
        fputc('\n', f);
    }
    if (!g_degenerate.empty()) {
        fputs("degenerate =", f);
        for (uint32_t low : g_degenerate) fprintf(f, " %08x", low);
        fputc('\n', f);
    }
    fclose(f);
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        logger::write("TexLod: cannot replace %S (err %lu)", path.c_str(), GetLastError());
        return;
    }
    g_dirty = false;
    for (const auto& seed : g_imported) {
        if (MoveFileW(seed.first.c_str(), seed.second.c_str()))
            logger::write("TexLod: seed %S is in the cache now -- renamed %S", seed.first.c_str(), seed.second.c_str());
        else
            logger::write("TexLod: seed %S is in the cache now, but cannot be renamed (err %lu) -- it is taken again next "
                          "start",
                seed.first.c_str(), GetLastError());
    }
    g_imported.clear();
}

// A low's entry from its record, for the lock-free readers; the lookups decide what it swaps. Caller holds g_mtx.
void publish(uint32_t low)
{
    auto it = g_records.find(low);
    if (it == g_records.end() || it->second.links.empty()) {
        if (entry_of(low)) tab_set(low, nullptr);
        return;
    }
    const Rec& r = it->second;
    auto*      e = new Entry;
    e->exact = exact(r);
    for (const Link& k : r.links) e->highs.push_back(k.high);
    const Entry* was = entry_of(low);
    if (was && was->exact == e->exact && was->highs == e->highs) {
        delete e;
        return;
    }
    tab_set(low, e);
}

std::string trim(const char* s, const char* e)
{
    while (s < e && (*s == ' ' || *s == '\t')) ++s;
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) --e;
    return std::string(s, e);
}

using Records = std::unordered_map<uint32_t, Rec>;

void merge_link(Records& into, uint32_t low, const Link& k)
{
    if (!low || !k.high || low == k.high) return;
    Rec& r = into[low];
    if (Link* e = link_to(r, k.high))
        *e = k;
    else
        r.links.push_back(k);
}

bool parse_line(const char* line, Records& into, std::set<uint32_t>& degen)
{
    const char* d = line;
    while (*d == ' ' || *d == '\t') ++d;
    if (strncmp(d, "degenerate", 10) == 0) {
        const char* p = strchr(d, '=');
        if (!p) return false;
        for (++p;;) {
            char*               end = nullptr;
            const unsigned long low = strtoul(p, &end, 16);
            if (end == p) break;
            if (low) degen.insert((uint32_t)low);
            p = end;
        }
        return true;
    }
    unsigned t = 0;
    int      n = 0;
    if (sscanf_s(line, " %x =%n", &t, &n) != 1 || !t || !n || !strstr(line, "exact=")) return false;
    for (const char* p = line + n; *p;) {
        while (*p == ' ' || *p == '\t' || *p == '|') ++p;
        char*               end = nullptr;
        const unsigned long low = strtoul(p, &end, 16);
        if (end == p) break;
        p = end;
        Link k;
        k.high = t;
        bool flagged = false;
        for (;;) {
            while (*p == ' ' || *p == '\t') ++p;
            if (!*p || *p == '|' || *p == '\r' || *p == '\n') break;
            const char* s = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '|' && *p != '\r' && *p != '\n') ++p;
            const std::string tok(s, p);
            const size_t      eq = tok.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = tok.substr(0, eq);
            const char*       val = tok.c_str() + eq + 1;
            if (key == "mip")
                k.mip = (uint32_t)strtoul(val, nullptr, 10);
            else if (key == "exact")
                k.exact = atoi(val) != 0;
            else if (key == "chain")
                k.chain = atoi(val) != 0;
            else if (key == "degenerate")
                flagged = atoi(val) != 0;
            else if (key == "rmse")
                k.rmse = (float)atof(val);
        }
        if (!k.exact && k.chain && !k.mip && k.rmse == 0.f) k.rmse = RMSE_UNMEASURED;
        if (flagged && k.exact)
            degen.insert((uint32_t)low);
        else
            merge_link(into, (uint32_t)low, k);
        if (*p == '\r' || *p == '\n') break;
    }
    return true;
}

bool parse_legacy_line(const char* line, Records& into, std::set<uint32_t>& degen)
{
    unsigned lo = 0;
    unsigned hi = 0;
    if (sscanf_s(line, " %x = %x", &lo, &hi) != 2 || !lo) return false;
    std::vector<uint32_t> rivals;
    std::string           note;
    if (const char* c = strchr(line, ';')) {
        ++c;
        while (*c == ' ' || *c == '\t') ++c;
        if (strncmp(c, "ini=", 4) == 0 || strncmp(c, "vs=", 3) == 0) {
            const char* e = strstr(c, " ;");
            const char* stop = e ? e : c + strlen(c);
            if (c[0] == 'v')
                for (const char* p = c + 3; p < stop;) {
                    char*               end = nullptr;
                    const unsigned long h = strtoul(p, &end, 16);
                    if (end == p) break;
                    if (h) rivals.push_back((uint32_t)h);
                    p = end;
                    if (*p != ',') break;
                    ++p;
                }
            c = e ? e + 2 : stop;
        }
        note = trim(c, c + strlen(c));
    }
    auto starts = [&](const char* s) { return note.rfind(s, 0) == 0; };
    if (!hi) {
        const size_t p = note.find("exact mip of");
        if (!starts("degenerate") || p == std::string::npos) return false;
        into.erase(lo);
        degen.insert(lo);
        return true;
    }
    into.erase(lo);
    degen.erase(lo);
    Link k;
    k.high = hi;
    k.exact = starts("mip-exact") || starts("lineage");
    k.chain = starts("lineage-chain") || starts("content-chain");
    if (starts("mip-exact")) k.mip = (uint32_t)strtoul(note.c_str() + 9, nullptr, 10);
    if (k.exact || rivals.size() < 2) {
        merge_link(into, lo, k);
        return true;
    }
    for (uint32_t h : rivals) {
        Link x = k;
        x.high = h;
        merge_link(into, lo, x);
    }
    return true;
}

void load_file(const std::wstring& path, int& files, Records& into, std::set<uint32_t>& degen)
{
    if (path.empty()) return;
    FILE* f = _wfsopen(path.c_str(), L"r", _SH_DENYNO);
    if (!f) return;
    ++files;
    std::string line;
    char        chunk[1024];
    while (fgets(chunk, sizeof chunk, f)) {
        line += chunk;
        if (line.back() != '\n' && !feof(f)) continue;
        if (!parse_line(line.c_str(), into, degen)) parse_legacy_line(line.c_str(), into, degen);
        line.clear();
    }
    fclose(f);
}

void normalize(Records& rs, std::set<uint32_t>& degen)
{
    for (auto it = rs.begin(); it != rs.end();) {
        auto& links = it->second.links;
        if (std::any_of(links.begin(), links.end(), [](const Link& k) { return k.exact; }))
            links.erase(std::remove_if(links.begin(), links.end(), [](const Link& k) { return !k.exact; }), links.end());
        if (exact(it->second) && links.size() > 1) degen.insert(it->first);
        if (links.empty() || degen.count(it->first))
            it = rs.erase(it);
        else
            ++it;
    }
}

uint64_t at_mip(uint32_t high, uint32_t mip)
{
    return ((uint64_t)high << 32) | mip;
}

bool mip_proven(uint32_t high, uint32_t mip)
{ // caller holds g_mtx: some low is the exact mip `mip` of `high`, its level known
    if (!mip) return false;
    for (auto& kv : g_records) {
        if (!exact(kv.second)) continue;
        const Link& k = kv.second.links.front();
        if (k.high == high && k.mip == mip) return true;
    }
    return false;
}

size_t drop_lookalikes_of(const std::set<uint64_t>& proven)
{
    size_t                dropped = 0;
    std::vector<uint32_t> touched;
    for (auto it = g_records.begin(); it != g_records.end();) {
        auto& links = it->second.links;
        if (exact(it->second)) {
            ++it;
            continue;
        }
        const size_t was = links.size();
        links.erase(
            std::remove_if(links.begin(), links.end(), [&](const Link& k) { return proven.count(at_mip(k.high, k.mip)) != 0; }),
            links.end());
        if (links.size() != was) {
            dropped += was - links.size();
            touched.push_back(it->first);
        }
        if (links.empty())
            it = g_records.erase(it);
        else
            ++it;
    }
    for (uint32_t low : touched) publish(low);
    if (dropped) mark_dirty();
    return dropped;
}

void import_seed(const std::wstring& path, const wchar_t* name)
{
    Records            seed;
    std::set<uint32_t> seed_degen;
    int                files = 0;
    load_file(path, files, seed, seed_degen);
    normalize(seed, seed_degen);
    size_t proven = 0;
    size_t added = 0;
    size_t proved_guess = 0;
    size_t gave_mip = 0;
    size_t raised = 0;
    size_t kept = 0;
    size_t listed = 0;
    size_t disagree = 0;
    for (uint32_t low : seed_degen)
        if (g_degenerate.insert(low).second) {
            g_records.erase(low);
            ++listed;
        }
    for (auto& kv : seed) {
        auto& links = kv.second.links;
        links.erase(std::remove_if(links.begin(), links.end(), [](const Link& k) { return !k.exact; }), links.end());
        if (links.empty()) continue;
        ++proven;
        const uint32_t low = kv.first;
        if (g_degenerate.count(low)) {
            ++kept;
            continue;
        }
        auto it = g_records.find(low);
        if (it == g_records.end()) {
            g_records.emplace(low, std::move(kv.second));
            ++added;
            continue;
        }
        if (!exact(it->second)) {
            it->second = std::move(kv.second);
            ++proved_guess;
            continue;
        }
        const Link&    s = links.front();
        Link&          e = it->second.links.front();
        const uint32_t one = e.high == s.high ? e.high : one_texture(e.high, s.high);
        if (one == e.high) {
            if (e.high == s.high && !e.mip && s.mip) {
                e.mip = s.mip;
                ++gave_mip;
            } else {
                ++kept;
            }
            continue;
        }
        if (one == s.high) {
            e = s;
            ++raised;
            continue;
        }
        g_records.erase(it);
        g_degenerate.insert(low);
        ++disagree;
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t when[32];
    swprintf_s(when, L"%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    g_imported.erase(std::remove_if(g_imported.begin(), g_imported.end(), [&](const auto& f) { return f.first == path; }),
        g_imported.end());
    g_imported.emplace_back(path, g_cache_dir + L"\\DISABLED_" + when + L"_" + name);
    mark_dirty();
    logger::write("TexLod: seed %S -- %zu proven low(s): %zu new, %zu proved a look-alike, %zu gave a mip level, "
                  "%zu moved to the higher of one texture, %zu kept as we had them; degenerate: %zu from its list, "
                  "%zu where its proof and ours name different targets",
        path.c_str(), proven, added, proved_guess, gave_mip, raised, kept, listed, disagree);
}

// `low` was just made the exact mip `mip` of `high` (0: the level is not known). Caller holds g_mtx.
void proven(uint32_t low, uint32_t high, uint32_t mip)
{
    mark_dirty();
    publish(low);
    if (!mip) return;
    if (const size_t n = drop_lookalikes_of({ at_mip(high, mip) }))
        logger::write("TexLod: %08x mip %u proven by %08x -- %zu look-alike link(s) claiming that mip dropped", high, mip,
            low, n);
}

// drop_lookalikes_of every mip a low is proven to be. Caller holds g_mtx.
size_t drop_lookalikes_of_proven()
{
    std::set<uint64_t> mips;
    for (auto& kv : g_records) {
        if (!exact(kv.second)) continue;
        const Link& k = kv.second.links.front();
        if (k.mip) mips.insert(at_mip(k.high, k.mip));
    }
    return drop_lookalikes_of(mips);
}

bool learn_exact(uint32_t low, uint32_t high, uint32_t mip, bool chain, bool identity, const char* how)
{
    if (!low || !high || low == high || g_degenerate.count(low)) return false;
    {
        auto rf = g_refuted.find(low);
        if (rf != g_refuted.end() && rf->second == high) {
            if (DiagOnce("TEXLOD-REFUSED", (int)low))
                logger::write("TexLod: REFUSED %08x -> %08x (%s): this pair was refuted by the texture's own mip0 this session",
                    low, high, how);
            return false;
        }
    }
    Rec& r = g_records[low];
    if (!r.links.empty() && !exact(r)) {
        if ((r.links.size() > 1 || !link_to(r, high)) && DiagOnce("TEXLOD-PROVEN", (int)low))
            logger::write("TexLod: %08x proven -> %08x (%s); %zu look-alike target(s) dropped", low, high, how, r.links.size());
        r.links.clear();
    }
    if (!r.links.empty()) {
        Link&          e = r.links.front();
        const uint32_t one = one_texture(e.high, high);
        if (e.high == high || one == e.high) {
            if (e.high != high || !mip || e.mip) return true; // on file already
            e.mip = mip;
            proven(low, high, mip);
            return true;
        }
        if (one == high) {
            logger::write("TexLod: %08x -> %08x (%s): its target %08x is a lower mip of that one", low, high, how, e.high);
            e.high = high;
            e.mip = mip;
            e.chain = chain;
            proven(low, high, mip);
            return true;
        }
        if (!identity) return false;
        logger::write("TexLod: DEGENERATE %08x -- it was the exact mip of %08x, now also mip %u of %08x: identical data, "
                      "never swapped from here on (persisted)",
            low, e.high, mip, high);
        g_records.erase(low);
        g_degenerate.insert(low);
        publish_degenerate();
        mark_dirty();
        publish(low);
        return true;
    }
    Link k;
    k.high = high;
    k.mip = mip;
    k.exact = true;
    k.chain = chain;
    r.links.push_back(k);
    proven(low, high, mip);
    return true;
}

bool learn_lookalike(uint32_t low, uint32_t high, uint32_t mip, float rmse_v, bool chain)
{
    if (!low || !high || low == high || g_degenerate.count(low)) return false;
    auto rf = g_refuted.find(low);
    if (rf != g_refuted.end() && rf->second == high) return false;
    if (exact_record(low) || mip_proven(high, mip)) return false;
    Rec& r = g_records[low];
    if (link_to(r, high)) return false;
    Link k;
    k.high = high;
    k.mip = mip;
    k.chain = chain;
    k.rmse = rmse_v;
    r.links.push_back(k);
    mark_dirty();
    publish(low);
    return true;
}

struct Feeder {
    uint32_t low = 0;
    uint32_t steps = 0;
};
std::unordered_map<uint32_t, std::vector<Feeder>> g_feeders;

uint32_t below(uint32_t mip, uint32_t steps)
{
    return mip && steps ? mip + steps : 0;
}

void chain_alias(uint32_t id, uint32_t rule, int depth, bool look, uint32_t mip, float rmse_v)
{
    if (!id || !rule || id == rule || depth > 16) return;
    const bool degen = g_degenerate.count(id) != 0;
    auto       rec = g_records.find(id);
    if (degen || (rec != g_records.end() && exact(rec->second))) {
        if (degen || !link_to(rec->second, rule)) {
            if (g_debug && DiagOnce("LINEAGE-STOP", (int)id)) {
                auto fe = g_feeders.find(id);
                logger::write("LINEAGE-STOP %08x -> %08x (depth %d): its exact record points to %08x%s; %zu feeder(s) below not walked",
                    id, rule, depth, single_high(id), degen ? " (degenerate)" : "",
                    fe == g_feeders.end() ? (size_t)0 : fe->second.size());
            }
            return;
        }
    } else {
        const bool added = look ? learn_lookalike(id, rule, mip, rmse_v, true) : learn_exact(id, rule, mip, true, false, "lineage-chain");
        if (g_debug && added)
            logger::write("LINEAGE-CHAIN %08x -> %08x mip %u (%s: fed a mip of a texture that is that target)", id, rule, mip,
                look ? "content-chain" : "lineage-chain");
    }
    auto it = g_feeders.find(id);
    if (it == g_feeders.end()) return;
    for (const Feeder& f : it->second) chain_alias(f.low, rule, depth + 1, look, below(mip, f.steps), rmse_v);
}

uint32_t mip_of(const LodSig& big, const LodSig& sm)
{
    uint32_t k = 0;
    for (uint32_t w = big.w; w > sm.w; w >>= 1) ++k;
    return k;
}

void decide(uint32_t hash, const LodSig& stored)
{
    unsigned passed = 0;
    for (auto& kv : g_sigs) {
        if (!kv.second.is_target || !is_lod_parent(kv.second, stored)) continue;
        float r;
        if (!is_lod_match(kv.second, stored, r)) continue;
        ++passed;
        if (learn_lookalike(hash, kv.first, mip_of(kv.second, stored), r, false) && g_debug)
            logger::trace("TexLod: %08x looks like %08x (%ux%u -> %ux%u, rmse=%.2f)", hash, kv.first, stored.w, stored.h,
                kv.second.w, kv.second.h, r);
    }
    if (passed && exact_record(hash)) {
        if (g_debug) logger::trace("TexLod: %08x matched %u target(s) by content, kept its exact record", hash, passed);
        return;
    }
    if (passed || !g_debug) return;
    int      ntgt = 0;
    int      nparent = 0;
    uint32_t cbest = 0;
    float    crm = 1e9f;
    float    cnc = 0;
    float    cstdB = 0;
    float    cstdS = sig_std(stored);
    for (auto& kv : g_sigs) {
        if (!kv.second.is_target) continue;
        ++ntgt;
        if (!is_lod_parent(kv.second, stored)) continue;
        ++nparent;
        float r = rmse(kv.second, stored);
        if (r < crm) {
            crm = r;
            cnc = ncc(kv.second, stored);
            cbest = kv.first;
            cstdB = sig_std(kv.second);
        }
    }
    if (DiagOnce("TEXLOD-NOMATCH", (int)hash)) logger::trace("TexLod: NO-MATCH %08x %ux%u | targets=%d parents=%d closest=%08x rmse=%.2f ncc=%.3f stdBig=%.1f stdSm=%.1f (need rmse<=%.0f ncc>=%.2f std>=%.0f)",
        hash, stored.w, stored.h, ntgt, nparent, cbest, crm, cnc, cstdB, cstdS, RMSE_MAX, NCC_MIN, MIN_STD);
}

} // namespace

void TexLodFeeds(uint32_t lower, uint32_t big, uint32_t big_rule, uint32_t steps)
{
    if (!lower || !big || lower == big) return;
    std::lock_guard l(g_mtx);
    if (!g_texlod_cache) return;
    auto& v = g_feeders[big];
    if (std::none_of(v.begin(), v.end(), [&](const Feeder& f) { return f.low == lower; })) v.push_back({ lower, steps });
    uint32_t rule = big_rule;
    if (!rule) rule = single_high(big);
    if (!rule) return;
    const bool look = !big_rule && !exact_record(big);
    uint32_t   mip = steps;
    float      rmse_v = RMSE_UNMEASURED;
    if (!big_rule) {
        const Link& k = g_records.find(big)->second.links.front();
        mip = below(k.mip, steps);
        rmse_v = k.rmse;
    }
    if (g_debug && DiagOnce("LINEAGE-WALK", (int)lower))
        logger::write("LINEAGE-WALK %08x fed %08x (%s) -> rule %08x", lower, big,
            big_rule ? "a target" : look ? "look-alike record"
                                         : "exact record",
            rule);
    chain_alias(lower, rule, 0, look, mip, rmse_v);
}

static void load_all()
{
    if (g_dirty) rewrite_cache(); // a reload reads the file back: what was learned since the last write goes in first
    std::vector<uint32_t> held;
    held.reserve(g_records.size());
    for (auto& kv : g_records) held.push_back(kv.first);
    g_records.clear();
    g_degenerate.clear();
    if (!g_texlod_cache) {
        g_sigs.clear();
        g_feeders.clear();
        publish_degenerate();
        for (uint32_t low : held) publish(low);
        logger::write("TexLod: texture match cache off (lod_cache=0) -- nothing read, learned or saved");
        return;
    }
    int files = 0;
    load_file(cache_path(), files, g_records, g_degenerate);
    normalize(g_records, g_degenerate);
    if (!g_cache_dir.empty()) {
        WIN32_FIND_DATAW sf;
        HANDLE           hs = FindFirstFileW((g_cache_dir + L"\\*.lodcache").c_str(), &sf);
        if (hs != INVALID_HANDLE_VALUE) {
            do {
                if (sf.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                const size_t n = wcslen(sf.cFileName);
                if (n < 9 || _wcsicmp(sf.cFileName + n - 9, L".lodcache") != 0) continue; // not a short-name match
                if (_wcsicmp(sf.cFileName, L"lod_tex_cache.lodcache") == 0 || _wcsnicmp(sf.cFileName, L"DISABLED_", 9) == 0)
                    continue;
                import_seed(g_cache_dir + L"\\" + sf.cFileName, sf.cFileName);
            } while (FindNextFileW(hs, &sf));
            FindClose(hs);
        }
    }
    const size_t unproven = drop_lookalikes_of_proven();
    publish_degenerate();
    for (uint32_t low : held) publish(low); // one no file holds now leaves the table
    size_t everywhere = 0;
    for (auto& kv : g_records) {
        publish(kv.first);
        everywhere += exact(kv.second);
    }
    logger::trace("TexLod: loaded %zu LOD lows from %S%s: %zu swapped everywhere, %zu look-alike (%s), "
                  "%zu degenerate (never swapped); %zu look-alike link(s) at a proven mip dropped; "
                  "checked at engine load",
        g_records.size() + g_degenerate.size(), cache_path().c_str(), files ? "" : " (none yet)", everywhere,
        g_records.size() - everywhere, g_texlod_resemblance ? "used" : "learned, not used", g_degenerate.size(),
        unproven);
}

void TexLodInit(const std::wstring& cache_dir)
{
    std::lock_guard l(g_mtx);
    if (!cache_dir.empty()) g_cache_dir = cache_dir;
    load_all();
}

void TexLodSetCache(bool on)
{
    std::lock_guard l(g_mtx);
    if (g_texlod_cache == on) return;
    if (!on && g_dirty) rewrite_cache(); // what was learned while it was on is kept for when it is on again
    g_texlod_cache = on;
    load_all();
    if (on) logger::write("TexLod: texture match cache on -- read back; learning and saving again");
}

void TexLodTargetsReady(bool (*is_target)(uint32_t hash, void* user), void* user)
{
    std::lock_guard l(g_mtx);
    if (!g_texlod_cache) return;
    unsigned              folded = 0;
    unsigned              narrowed = 0;
    std::vector<uint32_t> dropped;
    for (auto& kv : g_records) {
        if (!is_target || !exact(kv.second)) continue;
        Link& k = kv.second.links.front();
        if (is_target(k.high, user)) continue;
        auto up = g_records.find(k.high);
        if (up == g_records.end() || !exact(up->second)) continue;
        const Link& via = up->second.links.front();
        if (via.high == kv.first) continue;
        k.high = via.high;
        k.mip = k.mip && via.mip ? k.mip + via.mip : 0;
        ++folded;
    }
    for (auto it = g_records.begin(); it != g_records.end();) {
        auto& links = it->second.links;
        if (!is_target || exact(it->second)) {
            ++it;
            continue;
        }
        const size_t was = links.size();
        links.erase(std::remove_if(links.begin(), links.end(), [&](const Link& k) { return !is_target(k.high, user); }),
            links.end());
        if (links.empty()) {
            g_sigs.erase(it->first); // signatures are once-per-texture; forget it so its next upload matches again
            dropped.push_back(it->first);
            it = g_records.erase(it);
            continue;
        }
        if (links.size() != was) ++narrowed;
        ++it;
    }
    if (folded) drop_lookalikes_of_proven();
    for (auto& kv : g_records) publish(kv.first);
    for (uint32_t low : dropped) publish(low);
    g_feeders.clear();
    rewrite_cache();
    logger::write("TexLod: sweep -- %zu lows kept; %u folded onto the target their undeclared one is a lower mip of; "
                  "look-alike matches: %u lost a target no ini declares now, %zu dropped: none of their targets is declared",
        g_records.size(), folded, narrowed, dropped.size());
}

void TexLodSetResemblance(bool on)
{
    std::lock_guard l(g_mtx);
    if (g_texlod_resemblance == on) return;
    g_texlod_resemblance = on;
    size_t lookalike = 0;
    for (auto& kv : g_records) lookalike += !exact(kv.second);
    logger::write("TexLod: look-alike matches %s -- %zu look-alike low(s) %s", on ? "ON" : "OFF", lookalike,
        on ? "now used in draws of a mod that declares their target" : "learned, not used");
}

void TexLodFlushIfQuiet(unsigned quiet_ms)
{
    std::lock_guard l(g_mtx);
    if (g_dirty && GetTickCount64() - g_dirty_at >= quiet_ms) rewrite_cache();
}

void TexLodOnExit()
{
    std::lock_guard l(g_mtx);
    if (!g_dirty) return;
    rewrite_cache();
    logger::write("TexLod: exit -- what was learned since the last write is in the cache");
}

void TexLodRefute(uint32_t low, uint32_t high, const char* why)
{
    if (!low || !high) return;
    std::lock_guard<std::mutex> l(g_mtx);
    if (!g_texlod_cache) return;
    g_refuted[low] = high;
    auto it = g_records.find(low);
    if (it == g_records.end()) return;
    auto& links = it->second.links;
    auto  k = std::find_if(links.begin(), links.end(), [&](const Link& x) { return x.high == high; });
    if (k == links.end()) return;
    const bool was_exact = k->exact;
    links.erase(k);
    if (links.empty()) g_records.erase(it);
    publish(low);
    rewrite_cache();
    logger::write("TexLod: REFUTED %08x -> %08x (%s; was %s) -- dropped from memory and from the cache, refused if re-learned",
        low, high, why ? why : "", was_exact ? "exact" : "a look-alike");
}

namespace {
struct Pick {
    const Entry* e = nullptr;
    uint32_t     high = 0;
};
std::shared_mutex                  g_pick_mtx;
std::unordered_map<uint64_t, Pick> g_pick; // (low, draw's namespace) -> what it picked from which entry
uint32_t                           everywhere(const Entry* e)
{
    return e && e->exact ? e->highs.front() : 0;
}
} // namespace

uint32_t TexLodLookup(uint32_t hash)
{
    return g_texlod_alias_off ? 0 : everywhere(entry_of(hash)); // no lock: see g_tab
}
uint32_t TexLodLookupOwned(uint32_t hash, const wchar_t* ns, bool (*owns)(uint32_t high, const wchar_t* ns))
{
    if (g_texlod_alias_off || !hash || !ns || !*ns || !owns) return 0;
    const Entry* e = entry_of(hash);
    if (!e || e->exact || !g_texlod_resemblance) return 0;
    uint32_t fnv = 2166136261u;
    for (const wchar_t* c = ns; *c; ++c) fnv = (fnv ^ (uint32_t)towlower(*c)) * 16777619u;
    const uint64_t key = ((uint64_t)hash << 32) | fnv;
    {
        std::shared_lock l(g_pick_mtx);
        auto             p = g_pick.find(key);
        if (p != g_pick.end() && p->second.e == e) return p->second.high;
    }
    uint32_t pick = 0;
    int      owned = 0;
    for (uint32_t h : e->highs)
        if (owns(h, ns)) {
            pick = h;
            ++owned;
        }
    if (owned != 1) pick = 0;
    {
        std::unique_lock l(g_pick_mtx);
        g_pick[key] = Pick{ e, pick };
    }
    logger::write("TexLod: LOOK-ALIKE %08x (%zu targets) for '%ls' -> %s %08x", hash, e->highs.size(), ns,
        owned == 1 ? "its own target" : owned ? "REFUSED, it owns several, e.g."
                                              : "REFUSED, it owns none",
        pick);
    return pick;
}
uint32_t TexLodKnown(uint32_t hash)
{
    return everywhere(entry_of(hash)); // ungated by the A/B: membership and dedupe, never a swap
}
bool TexLodDegenerate(uint32_t hash)
{
    const std::vector<uint32_t>* v = g_degen_snap.load(std::memory_order_acquire); // no lock: see g_degen_snap
    return v && std::binary_search(v->begin(), v->end(), hash);
}

uint32_t TexLodAddMipExact(uint32_t low, uint32_t high, unsigned mip, bool* now_degenerate)
{
    if (now_degenerate) *now_degenerate = false;
    if (!low || !high || low == high) return 0;
    std::lock_guard l(g_mtx);
    if (!g_texlod_cache || g_degenerate.count(low)) return 0;
    const uint32_t prev = single_high(low);
    learn_exact(low, high, mip, false, true, "mip-exact"); // replaces a look-alike; an exact record stands
    if (g_degenerate.count(low)) {                         // the exact mip of TWO rules: of neither
        if (now_degenerate) *now_degenerate = true;
        return prev;
    }
    auto it = g_feeders.find(low); // what fed `low` is `high` at a lower resolution too
    if (it != g_feeders.end())
        for (const Feeder& f : it->second) chain_alias(f.low, high, 0, false, below(mip, f.steps), RMSE_UNMEASURED);
    return prev;
}

std::vector<uint8_t> TexLodSampleOf(const uint8_t* bc, size_t bc_size, uint32_t w, uint32_t h, uint32_t fmt)
{
    if (!bc || !DxgiIsBc(fmt) || (uint64_t)w * h < 256ull * 256ull) return {};
    const LodSig s = make_sig(fmt, bc, bc_size, w, h, false);
    return std::vector<uint8_t>(s.v, s.v + sizeof s.v);
}

void TexLodOnSample(uint32_t hash, const std::vector<uint8_t>& sample, uint32_t w, uint32_t h, uint32_t fmt, bool is_target)
{
    if (sample.size() != sizeof(LodSig::v) || !DxgiIsBc(fmt) || (uint64_t)w * h < 256ull * 256ull) return;
    std::lock_guard l(g_mtx);
    if (!g_texlod_cache) return;
    auto ex = g_sigs.find(hash);
    if (ex != g_sigs.end()) { // once per texture
        if (is_target) ex->second.is_target = true;
        return;
    }
    LodSig sig;
    memcpy(sig.v, sample.data(), sizeof sig.v);
    sig.w = w;
    sig.h = h;
    sig.fmt = fmt;
    sig.is_target = is_target;
    auto& stored = g_sigs.emplace(hash, sig).first->second;

    if (is_target) { // a new LOD0 target: every lower-res texture already seen that looks like it
        for (auto& kv : g_sigs) {
            if (kv.first == hash || kv.second.is_target || !is_lod_parent(stored, kv.second)) continue;
            float r;
            if (!is_lod_match(stored, kv.second, r)) continue;
            if (learn_lookalike(kv.first, hash, mip_of(stored, kv.second), r, false) && g_debug)
                logger::trace("TexLod: %08x looks like %08x (%ux%u -> %ux%u, rmse=%.2f, late)", kv.first, hash,
                    kv.second.w, kv.second.h, w, h, r);
        }
    } else
        decide(hash, stored);
}
