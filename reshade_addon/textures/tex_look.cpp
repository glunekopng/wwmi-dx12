// ===========================================================================
//  tex_look.cpp -- see tex_look.h. A texture recognised by how it looks: five patches of its mip0
//  (centre, four corners inset 1/8, side 1/16) box-averaged to one density, a histogram shortlist, then
//  SSIM per patch against every reference (a mod target, or a proven lower mip of one). A match becomes a
//  look-alike link in tex_lod. References and one verdict per content hash persist in tex_look.bin under
//  the algorithm version: a hash is decoded once, again only when the references changed since.
// ===========================================================================
#define BCDEC_IMPLEMENTATION
#include "../../third_party/bcdec/bcdec.h"
#include "tex_look.h"
#include "tex_lod.h"
#include "../../src/log.h"
#include "../../src/dxgi_bc.h"
#include "../config.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <share.h>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {

constexpr uint32_t kAlgo = 1;           // what a signature and a verdict mean; another value discards the file
constexpr uint32_t kMagic = 0x4b4c5854;       // "TXLK"
constexpr uint32_t kMagicPlaces = 0x324c5854; // "TXL2": the same, then the places
constexpr uint32_t kMagicMesh = 0x334c5854;   // "TXL3": the same, then the mesh places
constexpr uint32_t kMixed = 0xffffffffu;      // a place that showed two targets
constexpr int      kPatches = 5;
constexpr uint32_t kRefSide = 512; // a patch holds at most kRefSide/16 cells a side
constexpr uint32_t kMaxCells = kRefSide / 16;
constexpr float    kHistMax = 0.08f;
constexpr float    kSsimMin = 0.80f;
constexpr float    kLiveStd = 4.0f;
constexpr int      kLiveMin = 3;
constexpr double   kC1 = (0.01 * 255) * (0.01 * 255);
constexpr double   kC2 = (0.03 * 255) * (0.03 * 255);

struct Codec {
    uint8_t id = 0;
    uint8_t nch = 0;
};
Codec codec_of(uint32_t fmt)
{
    if (fmt >= 70 && fmt <= 72) return { 1, 3 };
    if (fmt >= 76 && fmt <= 78) return { 3, 4 };
    if (fmt == 79 || fmt == 80) return { 4, 1 };
    if (fmt == 82 || fmt == 83) return { 5, 2 };
    if (fmt >= 97 && fmt <= 99) return { 7, 4 };
    return {};
}
bool pow2(uint32_t v)
{
    return v && !(v & (v - 1));
}
bool eligible(uint32_t w, uint32_t h, uint32_t fmt)
{
    return pow2(w) && pow2(h) && (std::min)(w, h) >= 128 && codec_of(fmt).id;
}
uint32_t cells_of(uint32_t w, uint32_t h)
{
    return ((std::min)(w, kRefSide) / 16) * ((std::min)(h, kRefSide) / 16);
}

struct Corner {
    uint32_t x = 0;
    uint32_t y = 0;
};
struct Geometry {
    uint32_t sw = 0; // patch side in mip0 texels
    uint32_t sh = 0;
    uint32_t fx = 0; // texels per cell
    uint32_t fy = 0;
    uint32_t pw = 0; // cells per patch side
    uint32_t ph = 0;
    Corner   at[kPatches];
};
Geometry geometry_of(uint32_t w, uint32_t h)
{
    Geometry g;
    g.sw = w / 16;
    g.sh = h / 16;
    g.fx = w / (std::min)(w, kRefSide);
    g.fy = h / (std::min)(h, kRefSide);
    g.pw = g.sw / g.fx;
    g.ph = g.sh / g.fy;
    const uint32_t iw = w / 8;
    const uint32_t ih = h / 8;
    g.at[0] = { (w - g.sw) / 2, (h - g.sh) / 2 };
    g.at[1] = { iw, ih };
    g.at[2] = { w - iw - g.sw, ih };
    g.at[3] = { iw, h - ih - g.sh };
    g.at[4] = { w - iw - g.sw, h - ih - g.sh };
    return g;
}

struct LookSig {
    uint8_t              codec = 0;
    uint8_t              nch = 0;
    uint32_t             w = 0; // the mip it was taken from
    uint32_t             h = 0;
    uint32_t             pw = 0;
    uint32_t             ph = 0;
    std::vector<uint8_t> px; // kPatches x ph x pw x nch
};

struct LookStats {
    float hist[4][32] = {};
    float ch_std[4] = {};
    float patch_std[kPatches][4] = {};
};

struct LookView {
    uint32_t           pw = 0;
    uint32_t           ph = 0;
    uint32_t           nch = 0;
    std::vector<float> v; // kPatches x ph x pw x nch
    LookStats          st;
};

struct Ref {
    LookSig   sig;
    LookStats st; // at the signature's own density
};

enum class Kind : uint8_t {
    NoMatch = 0,
    Match = 1,
    Ref = 2, // offered as a reference: a target, or a proven lower mip of one
};
struct Verdict {
    uint32_t gen = 0; // the reference generation it was decided against
    Kind     kind = Kind::NoMatch;
};

struct Score {
    float hist = 1.f;
    float ssim = -1.f; // below 0: not measured, or too flat to tell
    bool  match() const { return hist <= kHistMax && ssim >= kSsimMin; }
};

void decode_block(uint8_t codec, const uint8_t* blk, uint8_t rgba[64])
{
    memset(rgba, 0, 64);
    if (codec == 1)
        bcdec_bc1(blk, rgba, 16);
    else if (codec == 3)
        bcdec_bc3(blk, rgba, 16);
    else if (codec == 7)
        bcdec_bc7(blk, rgba, 16);
    else if (codec == 4) {
        uint8_t r[16];
        bcdec_bc4(blk, r, 4);
        for (int i = 0; i < 16; ++i) rgba[i * 4] = r[i];
    } else if (codec == 5) {
        uint8_t rg[32];
        bcdec_bc5(blk, rg, 8);
        for (int i = 0; i < 16; ++i) {
            rgba[i * 4] = rg[i * 2];
            rgba[i * 4 + 1] = rg[i * 2 + 1];
        }
    }
}

bool sig_of(const TexLookCut& cut, LookSig& s)
{
    if (!eligible(cut.w, cut.h, cut.fmt)) return false;
    const Codec    c = codec_of(cut.fmt);
    const Geometry g = geometry_of(cut.w, cut.h);
    const uint32_t bb = DxgiBcBlockBytes(cut.fmt);
    const uint32_t nbx = g.sw / 4;
    const uint32_t nby = g.sh / 4;
    if (cut.blocks.size() != (size_t)kPatches * nbx * nby * bb) return false;
    s.codec = c.id;
    s.nch = c.nch;
    s.w = cut.w;
    s.h = cut.h;
    s.pw = g.pw;
    s.ph = g.ph;
    const size_t cells = (size_t)g.pw * g.ph * c.nch;
    s.px.assign(kPatches * cells, 0);
    std::vector<uint32_t> sum(cells);
    const uint8_t*        blk = cut.blocks.data();
    const double          per = (double)g.fx * g.fy;
    for (int k = 0; k < kPatches; ++k) {
        std::fill(sum.begin(), sum.end(), 0u);
        for (uint32_t by = 0; by < nby; ++by)
            for (uint32_t bx = 0; bx < nbx; ++bx, blk += bb) {
                uint8_t t[64];
                decode_block(c.id, blk, t);
                for (uint32_t i = 0; i < 16; ++i) {
                    const uint32_t tx = bx * 4 + (i & 3);
                    const uint32_t ty = by * 4 + (i >> 2);
                    uint32_t*      cell = &sum[((size_t)(ty / g.fy) * g.pw + tx / g.fx) * c.nch];
                    for (uint32_t ch = 0; ch < c.nch; ++ch) cell[ch] += t[i * 4 + ch];
                }
            }
        uint8_t* out = &s.px[k * cells];
        for (size_t i = 0; i < cells; ++i) out[i] = (uint8_t)std::nearbyint(sum[i] / per);
    }
    return true;
}

void stats_of(LookView& o)
{
    const size_t cells = (size_t)o.pw * o.ph;
    const size_t all = kPatches * cells;
    for (uint32_t c = 0; c < o.nch; ++c) {
        double s = 0;
        double ss = 0;
        for (int k = 0; k < kPatches; ++k) {
            double ps = 0;
            double pss = 0;
            for (size_t i = 0; i < cells; ++i) {
                const float v = o.v[(k * cells + i) * o.nch + c];
                ++o.st.hist[c][(std::min)(31, (int)v >> 3)];
                ps += v;
                pss += (double)v * v;
            }
            const double pm = ps / cells;
            o.st.patch_std[k][c] = (float)std::sqrt((std::max)(0.0, pss / cells - pm * pm));
            s += ps;
            ss += pss;
        }
        const double m = s / all;
        o.st.ch_std[c] = (float)std::sqrt((std::max)(0.0, ss / all - m * m));
        for (float& b : o.st.hist[c]) b /= (float)all;
    }
}

// The signature at a coarser (or its own) cell density: each cell the mean of the cells it covers.
LookView view_of(const LookSig& s, uint32_t pw, uint32_t ph)
{
    LookView o;
    o.pw = pw;
    o.ph = ph;
    o.nch = s.nch;
    const uint32_t fx = s.pw / pw;
    const uint32_t fy = s.ph / ph;
    o.v.assign((size_t)kPatches * pw * ph * s.nch, 0.f);
    for (int k = 0; k < kPatches; ++k)
        for (uint32_t y = 0; y < s.ph; ++y)
            for (uint32_t x = 0; x < s.pw; ++x)
                for (uint32_t c = 0; c < s.nch; ++c)
                    o.v[(((size_t)k * ph + y / fy) * pw + x / fx) * s.nch + c] +=
                        s.px[(((size_t)k * s.ph + y) * s.pw + x) * s.nch + c];
    const float per = (float)(fx * fy);
    for (float& v : o.v) v /= per;
    stats_of(o);
    return o;
}

float hist_dist(const LookStats& a, const LookStats& b, uint32_t nch)
{
    double sum = 0;
    int    n = 0;
    for (uint32_t c = 0; c < nch; ++c) {
        if (a.ch_std[c] < 1.f && b.ch_std[c] < 1.f) continue;
        double in = 0;
        for (int i = 0; i < 32; ++i) in += (std::min)(a.hist[c][i], b.hist[c][i]);
        sum += in;
        ++n;
    }
    return n ? (float)(1.0 - sum / n) : 1.f;
}

double ssim_plane(const LookView& a, const LookView& b, int k, uint32_t c, uint32_t win)
{
    const size_t base = (size_t)k * a.ph * a.pw;
    const double n = (double)win * win;
    double       total = 0;
    uint32_t     windows = 0;
    for (uint32_t y0 = 0; y0 + win <= a.ph; ++y0)
        for (uint32_t x0 = 0; x0 + win <= a.pw; ++x0) {
            double sa = 0;
            double sb = 0;
            double saa = 0;
            double sbb = 0;
            double sab = 0;
            for (uint32_t y = y0; y < y0 + win; ++y)
                for (uint32_t x = x0; x < x0 + win; ++x) {
                    const size_t i = (base + (size_t)y * a.pw + x) * a.nch + c;
                    const double va = a.v[i];
                    const double vb = b.v[i];
                    sa += va;
                    sb += vb;
                    saa += va * va;
                    sbb += vb * vb;
                    sab += va * vb;
                }
            const double ma = sa / n;
            const double mb = sb / n;
            const double var_a = saa / n - ma * ma;
            const double var_b = sbb / n - mb * mb;
            const double cov = sab / n - ma * mb;
            total += ((2 * ma * mb + kC1) * (2 * cov + kC2)) / ((ma * ma + mb * mb + kC1) * (var_a + var_b + kC2));
            ++windows;
        }
    return windows ? total / windows : 0;
}

// Lowest SSIM over the (patch, channel) planes with structure on either side; below 0 if fewer than
// kLiveMin patches have any.
float ssim_min(const LookView& a, const LookView& b)
{
    const uint32_t win = (std::min)({ 7u, a.pw, a.ph });
    int            live = 0;
    double         lo = 2;
    for (int k = 0; k < kPatches; ++k) {
        bool any = false;
        for (uint32_t c = 0; c < a.nch; ++c) {
            if ((std::max)(a.st.patch_std[k][c], b.st.patch_std[k][c]) < kLiveStd) continue;
            lo = (std::min)(lo, ssim_plane(a, b, k, c, win));
            any = true;
        }
        live += any;
    }
    return live >= kLiveMin ? (float)lo : -1.f;
}

// `cv` is `cand` at its own density.
Score compare(const LookSig& cand, const LookView& cv, const Ref& r)
{
    Score s;
    if (cand.codec != r.sig.codec || (uint64_t)cand.w * r.sig.h != (uint64_t)cand.h * r.sig.w) return s;
    const uint32_t pw = (std::min)(cand.pw, r.sig.pw);
    const uint32_t ph = (std::min)(cand.ph, r.sig.ph);
    const bool     ref_native = pw == r.sig.pw && ph == r.sig.ph;
    LookView       down;
    if (pw != cand.pw || ph != cand.ph) down = view_of(cand, pw, ph);
    const LookView& a = down.v.empty() ? cv : down;
    if (ref_native) {
        s.hist = hist_dist(a.st, r.st, cand.nch);
        if (s.hist > kHistMax) return s;
    }
    const LookView b = view_of(r.sig, pw, ph);
    if (!ref_native) {
        s.hist = hist_dist(a.st, b.st, cand.nch);
        if (s.hist > kHistMax) return s;
    }
    s.ssim = ssim_min(a, b);
    return s;
}

int live_patches(const LookStats& st, uint32_t nch)
{
    int live = 0;
    for (int k = 0; k < kPatches; ++k)
        for (uint32_t c = 0; c < nch; ++c)
            if (st.patch_std[k][c] >= kLiveStd) {
                ++live;
                break;
            }
    return live;
}

template <typename T>
bool put(FILE* f, const T& v)
{
    return fwrite(&v, sizeof v, 1, f) == 1;
}
template <typename T>
bool get(FILE* f, T& v)
{
    return fread(&v, sizeof v, 1, f) == 1;
}

using PlaceMap = std::unordered_map<uint64_t, uint32_t>;

class LookIndex {
public:
    void Init(const std::wstring& cache_dir);
    bool Wants(uint32_t hash, uint32_t w, uint32_t h, uint32_t fmt, bool is_target);
    void OnCut(uint32_t hash, const TexLookCut& cut, bool is_target);
    void FlushIfQuiet(unsigned quiet_ms);
    void OnExit();
    void AtPlace(uint64_t place, uint32_t hash, uint32_t target);
    void AtMeshPlace(uint64_t place, uint32_t hash, uint32_t target);
    void AddMeshPlace(uint64_t place, uint32_t target);
    uint32_t MeshMask(uint64_t pass, unsigned regs) const;
    bool HasMeshPlaces() const { return has_mesh_.load(std::memory_order_relaxed); }

private:
    std::wstring path() const { return dir_.empty() ? std::wstring() : dir_ + L"\\tex_look.bin"; }
    bool         weaker(uint32_t target, uint32_t w, uint32_t h) const;
    uint32_t     target_of(uint32_t hash, bool is_target) const;
    bool         offered(uint32_t hash) const;
    bool         keep_ref(uint32_t target, Ref&& r);
    void         offer(uint32_t source, uint32_t target, LookSig&& sig);
    void         match(uint32_t hash, const LookSig& sig);
    void         mark_dirty();
    bool         keep_place(PlaceMap& m, uint64_t place, uint32_t target);
    void         at_place(PlaceMap& m, uint64_t place, uint32_t hash, uint32_t target);
    size_t       load(const std::wstring& file, bool seed);
    void         write();

    mutable std::shared_mutex                          mtx_;
    std::unordered_map<uint32_t, Ref>                  refs_;     // target -> its reference
    std::unordered_map<uint32_t, Verdict>              verdicts_; // content hash -> what its decode decided
    PlaceMap                                           places_;      // a pass's slot -> the target HD draws there, or kMixed
    PlaceMap                                           mesh_places_; // the same for a game character draw's slot
    std::atomic<bool>                                  has_mesh_{ false };
    std::atomic<uint32_t>                              mesh_gen_{ 1 }; // bumped by every change to mesh_places_
    std::unordered_set<uint64_t>                       placed_;   // (hash, target) a place linked this session
    uint32_t                                           gen_ = 1;  // bumped by every reference added or replaced
    bool                                               loaded_ = false;
    bool                                               dirty_ = false;
    uint64_t                                           dirty_at_ = 0;
    std::wstring                                       dir_;
    std::vector<std::pair<std::wstring, std::wstring>> imported_; // seed taken -> its DISABLED_ name, after the next write
    std::atomic<uint32_t>                              decoded_{ 0 };
    std::atomic<uint32_t>                              skipped_{ 0 };
    std::atomic<uint32_t>                              matched_{ 0 };
    std::atomic<uint32_t>                              refs_new_{ 0 };
    std::atomic<uint32_t>                              places_new_{ 0 };
    std::atomic<uint32_t>                              by_place_{ 0 };
};

void LookIndex::mark_dirty()
{ // caller holds mtx_
    dirty_ = true;
    dirty_at_ = GetTickCount64();
}

bool LookIndex::keep_place(PlaceMap& m, uint64_t place, uint32_t target)
{ // caller holds mtx_
    auto it = m.find(place);
    if (it == m.end()) {
        m.emplace(place, target);
    } else {
        if (it->second == target || it->second == kMixed) return false;
        it->second = kMixed;
    }
    if (&m == &mesh_places_) mesh_gen_.fetch_add(1, std::memory_order_release);
    mark_dirty();
    return true;
}

// `target`: what `hash` is to the mods (a target, or exactly one), 0 for a texture they do not know. A known one
// records its place; an unknown one at a place recorded with one target becomes that target's look-alike.
void LookIndex::at_place(PlaceMap& m, uint64_t place, uint32_t hash, uint32_t target)
{
    if (!g_texlod_cache || !hash) return;
    if (target) {
        {
            std::shared_lock l(mtx_);
            auto             it = m.find(place);
            if (it != m.end() && (it->second == target || it->second == kMixed)) return;
        }
        std::unique_lock l(mtx_);
        if (keep_place(m, place, target)) places_new_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (TexLodDegenerate(hash)) return;
    uint32_t to = 0;
    {
        std::shared_lock l(mtx_);
        auto             it = m.find(place);
        if (it == m.end() || it->second == kMixed) return;
        to = it->second;
        if (placed_.count(((uint64_t)hash << 32) | to)) return;
    }
    {
        std::unique_lock l(mtx_);
        if (!placed_.insert(((uint64_t)hash << 32) | to).second) return;
    }
    if (!TexLodLearnPlace(hash, to)) return;
    by_place_.fetch_add(1, std::memory_order_relaxed);
    logger::info("TexLook: %08x is drawn where HD draws %08x -- its look-alike by place", hash, to);
}

void LookIndex::AtPlace(uint64_t place, uint32_t hash, uint32_t target)
{
    at_place(places_, place, hash, target);
}

void LookIndex::AtMeshPlace(uint64_t place, uint32_t hash, uint32_t target)
{
    at_place(mesh_places_, place, hash, target);
}

void LookIndex::AddMeshPlace(uint64_t place, uint32_t target)
{
    std::unique_lock l(mtx_);
    if (keep_place(mesh_places_, place, target)) places_new_.fetch_add(1, std::memory_order_relaxed);
    has_mesh_.store(true, std::memory_order_relaxed);
}

// Remembered per thread and pass until mesh_places_ changes: a character draw asks every frame.
uint32_t LookIndex::MeshMask(uint64_t pass, unsigned regs) const
{
    struct Memo {
        uint32_t mask = 0;
        uint32_t gen = 0;
    };
    thread_local std::unordered_map<uint64_t, Memo> t_memo;
    const uint32_t                                  gen = mesh_gen_.load(std::memory_order_acquire);
    auto                                            it = t_memo.find(pass);
    if (it != t_memo.end() && it->second.gen == gen) return it->second.mask;
    uint32_t mask = 0;
    {
        std::shared_lock l(mtx_);
        for (unsigned r = 0; r < regs && r < 32; ++r) {
            auto p = mesh_places_.find(TexLookSlotOf(pass, r));
            if (p != mesh_places_.end() && p->second != kMixed) mask |= 1u << r;
        }
    }
    t_memo[pass] = Memo{ mask, gen };
    return mask;
}

bool LookIndex::weaker(uint32_t target, uint32_t w, uint32_t h) const
{ // caller holds mtx_
    auto it = refs_.find(target);
    return it == refs_.end() || it->second.sig.pw * it->second.sig.ph < cells_of(w, h);
}

uint32_t LookIndex::target_of(uint32_t hash, bool is_target) const
{ // caller holds mtx_
    return is_target || refs_.count(hash) ? hash : TexLodKnown(hash);
}

bool LookIndex::offered(uint32_t hash) const
{ // caller holds mtx_
    auto it = verdicts_.find(hash);
    return it != verdicts_.end() && it->second.kind == Kind::Ref;
}

bool LookIndex::Wants(uint32_t hash, uint32_t w, uint32_t h, uint32_t fmt, bool is_target)
{
    if (!g_texlod_cache || !hash || !eligible(w, h, fmt)) return false;
    bool want = false;
    if (!TexLodDegenerate(hash)) {
        std::shared_lock l(mtx_);
        const uint32_t   high = target_of(hash, is_target);
        if (high)
            want = !offered(hash) && weaker(high, w, h);
        else {
            auto v = verdicts_.find(hash);
            want = v == verdicts_.end() || v->second.kind == Kind::Ref ||
                (v->second.kind == Kind::Match ? !TexLodLinked(hash) : v->second.gen != gen_);
        }
    }
    if (!want) skipped_.fetch_add(1, std::memory_order_relaxed);
    return want;
}

bool LookIndex::keep_ref(uint32_t target, Ref&& r)
{ // caller holds mtx_
    auto it = refs_.find(target);
    if (it != refs_.end() && it->second.sig.pw * it->second.sig.ph >= r.sig.pw * r.sig.ph) return false;
    refs_[target] = std::move(r);
    ++gen_;
    mark_dirty();
    return true;
}

void LookIndex::offer(uint32_t source, uint32_t target, LookSig&& sig)
{
    Ref r;
    r.sig = std::move(sig);
    r.st = view_of(r.sig, r.sig.pw, r.sig.ph).st;
    const bool     flat = live_patches(r.st, r.sig.nch) < kLiveMin;
    const uint32_t w = r.sig.w;
    const uint32_t h = r.sig.h;
    bool           kept = false;
    {
        std::unique_lock l(mtx_);
        verdicts_[source] = Verdict{ gen_, Kind::Ref };
        mark_dirty();
        if (!flat) kept = keep_ref(target, std::move(r));
    }
    if (kept) refs_new_.fetch_add(1, std::memory_order_relaxed);
    if (g_debug && flat)
        logger::trace("TexLook: %08x %ux%u too flat to be the reference of %08x", source, w, h, target);
    else if (g_debug && kept)
        logger::trace("TexLook: %08x %ux%u is now the reference of %08x%s", source, w, h, target,
            source == target ? " (itself)" : ", a proven lower mip of it");
}

void LookIndex::match(uint32_t hash, const LookSig& sig)
{
    const LookView                         cv = view_of(sig, sig.pw, sig.ph);
    std::vector<std::pair<uint32_t, Score>> hits;
    uint32_t                               gen = 0;
    uint32_t                               closest_t = 0;
    Score                                  closest;
    {
        std::shared_lock l(mtx_);
        gen = gen_;
        for (const auto& kv : refs_) {
            if (kv.first == hash) continue;
            const Score s = compare(sig, cv, kv.second);
            if (s.match())
                hits.emplace_back(kv.first, s);
            else if (s.hist < closest.hist || (s.hist == closest.hist && s.ssim > closest.ssim)) {
                closest = s;
                closest_t = kv.first;
            }
        }
    }
    {
        std::unique_lock l(mtx_);
        verdicts_[hash] = Verdict{ gen, hits.empty() ? Kind::NoMatch : Kind::Match };
        mark_dirty();
    }
    for (const auto& h : hits) {
        if (TexLodLearnVisual(hash, h.first, h.second.ssim) && g_debug)
            logger::trace("TexLook: %08x %ux%u looks like %08x (hist %.3f, ssim-min %.3f)", hash, sig.w, sig.h, h.first,
                h.second.hist, h.second.ssim);
    }
    if (hits.size() > 1)
        logger::info("TexLook: %08x %ux%u looks like %zu targets, e.g. %08x (ssim-min %.3f) and %08x (%.3f)", hash, sig.w,
            sig.h, hits.size(), hits[0].first, hits[0].second.ssim, hits[1].first, hits[1].second.ssim);
    if (!hits.empty())
        matched_.fetch_add(1, std::memory_order_relaxed);
    else if (closest_t && DiagOnce("TEXLOOK-NOMATCH", (int)hash))
        logger::trace("TexLook: NO-MATCH %08x %ux%u | closest %08x hist %.3f ssim-min %.3f (need hist <= %.2f, ssim >= %.2f)",
            hash, sig.w, sig.h, closest_t, closest.hist, closest.ssim, kHistMax, kSsimMin);
}

void LookIndex::OnCut(uint32_t hash, const TexLookCut& cut, bool is_target)
{
    LookSig sig;
    if (!g_texlod_cache || !sig_of(cut, sig)) return;
    decoded_.fetch_add(1, std::memory_order_relaxed);
    uint32_t high = 0;
    {
        std::shared_lock l(mtx_);
        high = target_of(hash, is_target);
    }
    if (high)
        offer(hash, high, std::move(sig));
    else
        match(hash, sig);
}

// The references (all a seed gives) and, from our own file, the verdicts. Caller holds mtx_.
size_t LookIndex::load(const std::wstring& file, bool seed)
{
    FILE* f = _wfsopen(file.c_str(), L"rb", _SH_DENYNO);
    if (!f) return 0;
    uint32_t magic = 0;
    uint32_t algo = 0;
    uint32_t gen = 0;
    uint32_t nrefs = 0;
    uint32_t nverdicts = 0;
    if (!get(f, magic) || !get(f, algo) || !get(f, gen) || !get(f, nrefs) || !get(f, nverdicts) ||
        (magic != kMagic && magic != kMagicPlaces && magic != kMagicMesh) || algo != kAlgo) {
        fclose(f);
        logger::write("TexLook: %S is not algorithm v%u (it says v%u) -- %s", file.c_str(), kAlgo, algo,
            seed ? "seed not taken" : "discarded, every texture is looked at again");
        return 0;
    }
    size_t taken = 0;
    bool   ok = true;
    for (uint32_t i = 0; ok && i < nrefs; ++i) {
        uint32_t target = 0;
        Ref      r;
        ok = get(f, target) && get(f, r.sig.codec) && get(f, r.sig.nch) && get(f, r.sig.w) && get(f, r.sig.h) &&
            get(f, r.sig.pw) && get(f, r.sig.ph);
        ok = ok && r.sig.nch && r.sig.nch <= 4 && r.sig.pw && r.sig.pw <= kMaxCells && r.sig.ph && r.sig.ph <= kMaxCells;
        if (!ok) break;
        r.sig.px.resize((size_t)kPatches * r.sig.pw * r.sig.ph * r.sig.nch);
        ok = fread(r.sig.px.data(), 1, r.sig.px.size(), f) == r.sig.px.size();
        if (!ok) break;
        r.st = view_of(r.sig, r.sig.pw, r.sig.ph).st;
        if (seed)
            taken += keep_ref(target, std::move(r));
        else {
            refs_[target] = std::move(r);
            ++taken;
        }
    }
    for (uint32_t i = 0; ok && i < nverdicts; ++i) {
        uint32_t hash = 0;
        Verdict  v;
        uint8_t  kind = 0;
        ok = get(f, hash) && get(f, v.gen) && get(f, kind) && kind <= (uint8_t)Kind::Ref;
        v.kind = (Kind)kind;
        if (ok && !seed) verdicts_[hash] = v;
    }
    uint32_t nplaces = 0;
    if (ok && magic != kMagic) ok = get(f, nplaces);
    for (uint32_t i = 0; ok && i < nplaces; ++i) {
        uint64_t place = 0;
        uint32_t target = 0;
        ok = get(f, place) && get(f, target);
        if (ok && target) keep_place(places_, place, target);
    }
    uint32_t nmesh = 0;
    if (ok && magic == kMagicMesh) ok = get(f, nmesh);
    for (uint32_t i = 0; ok && i < nmesh; ++i) {
        uint64_t place = 0;
        uint32_t target = 0;
        ok = get(f, place) && get(f, target);
        if (ok && target) keep_place(mesh_places_, place, target);
    }
    if (!mesh_places_.empty()) has_mesh_.store(true, std::memory_order_relaxed);
    fclose(f);
    if (!seed) gen_ = (std::max)(gen_, gen);
    if (!ok) logger::write("TexLook: %S ends early -- what came before is kept", file.c_str());
    return taken;
}

void LookIndex::write()
{ // caller holds mtx_
    const std::wstring p = path();
    if (p.empty()) return;
    const std::wstring tmp = p + L".tmp";
    FILE*              f = _wfsopen(tmp.c_str(), L"wb", _SH_DENYNO);
    if (!f) {
        logger::write("TexLook: cannot write %S", tmp.c_str());
        return;
    }
    bool ok = put(f, kMagicMesh) && put(f, kAlgo) && put(f, gen_) && put(f, (uint32_t)refs_.size()) &&
        put(f, (uint32_t)verdicts_.size());
    for (const auto& kv : refs_) {
        const LookSig& s = kv.second.sig;
        ok = ok && put(f, kv.first) && put(f, s.codec) && put(f, s.nch) && put(f, s.w) && put(f, s.h) && put(f, s.pw) &&
            put(f, s.ph) && fwrite(s.px.data(), 1, s.px.size(), f) == s.px.size();
    }
    for (const auto& kv : verdicts_) ok = ok && put(f, kv.first) && put(f, kv.second.gen) && put(f, (uint8_t)kv.second.kind);
    ok = ok && put(f, (uint32_t)places_.size());
    for (const auto& kv : places_) ok = ok && put(f, kv.first) && put(f, kv.second);
    ok = ok && put(f, (uint32_t)mesh_places_.size());
    for (const auto& kv : mesh_places_) ok = ok && put(f, kv.first) && put(f, kv.second);
    ok = fclose(f) == 0 && ok;
    if (!ok || !MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        logger::write("TexLook: cannot replace %S (err %lu)", p.c_str(), GetLastError());
        return;
    }
    dirty_ = false;
    logger::trace("TexLook: saved %zu reference(s), %zu verdict(s), %zu place(s), %zu mesh place(s); this session %u "
                  "decoded, %u skipped by their hash, %u matched, %u new reference(s), %u new place(s), %u look-alike(s) by "
                  "place",
        refs_.size(), verdicts_.size(), places_.size(), mesh_places_.size(), decoded_.load(), skipped_.load(),
        matched_.load(), refs_new_.load(), places_new_.load(), by_place_.load());
    for (const auto& seed : imported_) {
        if (MoveFileW(seed.first.c_str(), seed.second.c_str()))
            logger::write("TexLook: seed %S is in tex_look.bin now -- renamed %S", seed.first.c_str(), seed.second.c_str());
        else
            logger::write("TexLook: seed %S is in tex_look.bin now, but cannot be renamed (err %lu) -- it is taken again "
                          "next start",
                seed.first.c_str(), GetLastError());
    }
    imported_.clear();
}

void LookIndex::Init(const std::wstring& cache_dir)
{
    std::unique_lock l(mtx_);
    if (!cache_dir.empty()) dir_ = cache_dir;
    if (dir_.empty()) return;
    if (!loaded_) {
        loaded_ = true;
        const size_t n = load(path(), false);
        logger::write("TexLook: %zu reference(s), %zu verdict(s), %zu place(s), %zu mesh place(s) from %S (algorithm v%u)",
            n, verdicts_.size(), places_.size(), mesh_places_.size(), path().c_str(), kAlgo);
    }
    WIN32_FIND_DATAW sf;
    HANDLE           hs = FindFirstFileW((dir_ + L"\\*.texlook").c_str(), &sf);
    if (hs == INVALID_HANDLE_VALUE) return;
    do {
        if (sf.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const size_t n = wcslen(sf.cFileName);
        if (n < 8 || _wcsicmp(sf.cFileName + n - 8, L".texlook") != 0 || _wcsnicmp(sf.cFileName, L"DISABLED_", 9) == 0)
            continue;
        const std::wstring from = dir_ + L"\\" + sf.cFileName;
        if (std::any_of(imported_.begin(), imported_.end(), [&](const auto& s) { return s.first == from; })) continue;
        const size_t taken = load(from, true);
        SYSTEMTIME   st;
        GetLocalTime(&st);
        wchar_t when[32];
        swprintf_s(when, L"%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        imported_.emplace_back(from, dir_ + L"\\DISABLED_" + when + L"_" + sf.cFileName);
        mark_dirty();
        logger::write("TexLook: seed %S -- %zu reference(s) new or denser than ours", from.c_str(), taken);
    } while (FindNextFileW(hs, &sf));
    FindClose(hs);
}

void LookIndex::FlushIfQuiet(unsigned quiet_ms)
{
    std::unique_lock l(mtx_);
    if (dirty_ && GetTickCount64() - dirty_at_ >= quiet_ms) write();
}

void LookIndex::OnExit()
{
    std::unique_lock l(mtx_);
    if (dirty_) write();
    logger::write("TexLook: exit -- %zu reference(s), %zu verdict(s), %zu place(s), %zu mesh place(s); this session %u "
                  "decoded, %u skipped by their hash, %u matched, %u new reference(s), %u new place(s), %u look-alike(s) by "
                  "place",
        refs_.size(), verdicts_.size(), places_.size(), mesh_places_.size(), decoded_.load(), skipped_.load(),
        matched_.load(), refs_new_.load(), places_new_.load(), by_place_.load());
}

LookIndex g_look;

} // namespace

void TexLookInit(const std::wstring& cache_dir)
{
    g_look.Init(cache_dir);
}
void TexLookFlushIfQuiet(unsigned quiet_ms)
{
    g_look.FlushIfQuiet(quiet_ms);
}
void TexLookOnExit()
{
    g_look.OnExit();
}
bool TexLookWants(uint32_t hash, uint32_t w, uint32_t h, uint32_t fmt, bool is_target)
{
    return g_look.Wants(hash, w, h, fmt, is_target);
}
void TexLookOnCut(uint32_t hash, const TexLookCut& cut, bool is_target)
{
    g_look.OnCut(hash, cut, is_target);
}
uint64_t TexLookPlaceOf(const wchar_t* ns, int comp, uint64_t ps)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (const wchar_t* c = ns; c && *c; ++c) h = (h ^ (uint64_t)towlower(*c)) * 0x100000001b3ull;
    h = (h ^ (uint64_t)(uint32_t)comp) * 0x100000001b3ull;
    return (h ^ ps) * 0x100000001b3ull;
}
void TexLookAtPlace(uint64_t pass, uint32_t slot, uint32_t hash, uint32_t target)
{
    g_look.AtPlace(TexLookSlotOf(pass, slot), hash, target);
}
uint64_t TexLookMeshPassOf(uint32_t ib_hash, uint32_t first_index, uint32_t index_count, uint64_t ps)
{
    uint64_t h = 0x84222325cbf29ce4ull;
    for (const uint32_t v : { ib_hash, first_index, index_count }) h = (h ^ v) * 0x100000001b3ull;
    return (h ^ ps) * 0x100000001b3ull;
}
uint64_t TexLookSlotOf(uint64_t pass, uint32_t slot)
{
    return (pass ^ slot) * 0x100000001b3ull;
}
bool TexLookHasMeshPlaces()
{
    return g_look.HasMeshPlaces();
}
uint32_t TexLookMeshMask(uint64_t pass, unsigned regs)
{
    return g_look.MeshMask(pass, regs);
}
void TexLookAtMeshPlace(uint64_t place, uint32_t hash, uint32_t target)
{
    g_look.AtMeshPlace(place, hash, target);
}
void TexLookAddMeshPlace(uint64_t place, uint32_t target)
{
    g_look.AddMeshPlace(place, target);
}

TexLookCut TexLookCutOf(const uint8_t* bc, size_t bc_size, uint32_t w, uint32_t h, uint32_t fmt)
{
    TexLookCut cut;
    if (!bc || !eligible(w, h, fmt)) return cut;
    const uint32_t bb = DxgiBcBlockBytes(fmt);
    const size_t   row = (size_t)(w / 4) * bb;
    if (bc_size < row * (h / 4)) return cut;
    const Geometry g = geometry_of(w, h);
    const size_t   run = (size_t)(g.sw / 4) * bb;
    cut.blocks.resize(kPatches * (size_t)(g.sh / 4) * run);
    uint8_t* d = cut.blocks.data();
    for (const Corner& p : g.at)
        for (uint32_t r = 0; r < g.sh / 4; ++r, d += run) memcpy(d, bc + (size_t)(p.y / 4 + r) * row + (size_t)(p.x / 4) * bb, run);
    cut.fmt = fmt;
    cut.w = w;
    cut.h = h;
    return cut;
}
