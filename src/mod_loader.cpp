#include "mod_loader.h"
#include "log.h"
#include "../reshade_addon/textures/tex_lod.h"         // TexLodInit: the LOD cache read again on F5
#include "../reshade_addon/textures/tex_look.h"        // TexLookInit: a seed dropped in since taken on F5
#include "../reshade_addon/migoto/port_api.h"          // their [Resource*] registry
#include "../reshade_addon/instrumentation/profiler.h" // ET:TexStore
#include <windows.h>
#include <atomic>
#include <vector>
#include <algorithm>
#include <string>
#include <filesystem>
#include <map>
#include <unordered_set>
#include <cwctype> // towlower -- EnumerateMods' folder match
#include <d3d12.h>
#include <wrl.h>

using Microsoft::WRL::ComPtr;
using namespace reshade::api;
namespace fs = std::filesystem;

static device* g_device = nullptr;
static bool    g_reload_pending = false;


static ComPtr<ID3D12DescriptorHeap>              g_srv_heap;
static std::vector<ComPtr<ID3D12DescriptorHeap>> g_srv_more; // further blocks of g_srv_heap_cap, as the library grows
static uint32_t                                  g_srv_heap_size = 0;
static uint32_t                                  g_srv_heap_cap = 0; // descriptors in g_srv_heap (and in each further block); 0 until it exists
namespace ModLoader {
unsigned SrvHeapSize()
{
    return g_srv_heap_size;
}
unsigned SrvHeapCap()
{
    return g_srv_heap_cap * (unsigned)(1 + g_srv_more.size());
}
}
static uint32_t g_srv_descriptor_size = 0;
// The next mod SRV slot; a further block is added when the ones there are used up. {0} if one could not be made.
static D3D12_CPU_DESCRIPTOR_HANDLE NextModSrv()
{
    if (!g_srv_heap || !g_srv_heap_cap) return { 0 };
    const uint32_t        blk = g_srv_heap_size / g_srv_heap_cap;
    const uint32_t        off = g_srv_heap_size % g_srv_heap_cap;
    ID3D12DescriptorHeap* h = g_srv_heap.Get();
    if (blk) {
        while (g_srv_more.size() < blk) {
            ComPtr<ID3D12Device>             dev;
            ComPtr<ID3D12DescriptorHeap>     nb;
            const D3D12_DESCRIPTOR_HEAP_DESC d = g_srv_heap->GetDesc();
            if (FAILED(g_srv_heap->GetDevice(IID_PPV_ARGS(&dev))) || FAILED(dev->CreateDescriptorHeap(&d, IID_PPV_ARGS(&nb)))) return { 0 };
            g_srv_more.push_back(nb);
            logger::write("ModLoader: mod SRV heap grown to %u descriptors (the library outgrew its first sizing)",
                g_srv_heap_cap * (unsigned)(1 + g_srv_more.size()));
        }
        h = g_srv_more[blk - 1].Get();
    }
    D3D12_CPU_DESCRIPTOR_HANDLE c = h->GetCPUDescriptorHandleForHeapStart();
    c.ptr += (SIZE_T)off * g_srv_descriptor_size;
    ++g_srv_heap_size;
    return c;
}
static uint32_t SrvHeapCapacity(unsigned* dds_out)
{
    unsigned dds = 0;
    PortApi::ForEachCustomResource(
        [](const wchar_t*, const wchar_t* filename, int, bool, ID3D12Resource*, void* user) {
            const size_t n = filename ? wcslen(filename) : 0;
            if (n >= 4 && _wcsicmp(filename + n - 4, L".dds") == 0) ++*static_cast<unsigned*>(user);
        },
        &dds);
    if (dds_out) *dds_out = dds;
    const uint32_t need = 2u * dds + 256u;
    const uint32_t cap = (need < 1024u) ? 1024u : ((need + 63u) & ~63u);
    return cap;
}
static void TexStoreAlloc(uint32_t cap); // below, with the store

void ModLoader::Init(device* device)
{
    g_device = device;
}

void ModLoader::GetLastStats(int& mods)
{
    static unsigned s_gen = ~0u;
    static int      s_mods = 0;
    const unsigned  gen = PortApi::LoadGeneration();
    if (gen != s_gen) {
        s_gen = gen;
        s_mods = (int)EnumerateMods().size();
    }
    mods = s_mods;
}

static void DoReload()
{
    if (!g_device) return;
    if (g_device->get_api() != reshade::api::device_api::d3d12) {
        logger::write("ModLoader: skipping reload -- device is not D3D12 (api=%u)", (unsigned)g_device->get_api());
        return;
    }
    TexLodInit(); // the LOD cache read again, and any seed dropped in since taken
    TexLookInit();
    g_srv_heap_size = 0;
}

static std::vector<ModLoader::TexResource> g_tex_resources;
struct TexStore {
    uint32_t               cap = 0;
    uint32_t               icap = 0;
    unsigned               shift = 64;
    ModLoader::TexEntry*   entries = nullptr;
    std::atomic<uint64_t>* key = nullptr;
    std::atomic<uint32_t>* val = nullptr;
    uint32_t               slot(uint64_t k) const { return (uint32_t)((k * 0x9E3779B97F4A7C15ull) >> shift) & (icap - 1); }
    void                   put_key(uint64_t k, uint32_t index)
    {
        for (uint32_t i = slot(k), n = 0; n < icap; ++n, i = (i + 1) & (icap - 1)) {
            const uint64_t was = key[i].load(std::memory_order_acquire);
            if (was == k || was == 0) {
                val[i].store(index, std::memory_order_release);
                key[i].store(k, std::memory_order_release); // publish the key LAST
                return;
            }
        }
    }
};
static std::atomic<TexStore*> g_tex_store{ nullptr };
static std::atomic<uint32_t>  g_tex_count{ 0 };
static std::atomic<unsigned>  g_tex_gen{ 1 };
static TexStore*              TexStoreMake(uint32_t cap)
{
    auto*    s = new TexStore();
    uint32_t icap = 4096u;
    unsigned bits = 12;
    while (icap < 4u * cap) {
        icap <<= 1;
        ++bits;
    }
    s->entries = new ModLoader::TexEntry[cap]();
    s->key = new std::atomic<uint64_t>[icap];
    s->val = new std::atomic<uint32_t>[icap];
    for (uint32_t i = 0; i < icap; ++i) {
        s->key[i].store(0);
        s->val[i].store(0);
    }
    s->shift = 64u - bits;
    s->icap = icap;
    s->cap = cap;
    return s;
}
static void TexStoreAlloc(uint32_t cap)
{
    if (g_tex_store.load(std::memory_order_acquire) || !cap) return;
    g_tex_store.store(TexStoreMake(cap), std::memory_order_release);
}
static void TexStoreClear()
{
    g_tex_gen.fetch_add(1, std::memory_order_acq_rel);
    g_tex_count.store(0, std::memory_order_release);
    if (TexStore* s = g_tex_store.load(std::memory_order_acquire))
        for (uint32_t i = 0; i < s->icap; ++i) s->key[i].store(0, std::memory_order_release);
}
static void TexStorePublish(uint32_t index, const ModLoader::TexEntry& e)
{
    TexStore* s = g_tex_store.load(std::memory_order_acquire);
    if (!s || !e.res) return;
    if (index >= s->cap) {
        uint32_t cap = s->cap * 2;
        while (cap <= index) cap *= 2;
        TexStore*      g = TexStoreMake(cap);
        const uint32_t n = g_tex_count.load(std::memory_order_acquire);
        for (uint32_t i = 0; i < n && i < s->cap; ++i) {
            g->entries[i] = s->entries[i];
            if (s->entries[i].res) g->put_key((uint64_t)s->entries[i].res, i);
        }
        g_tex_store.store(g, std::memory_order_release);
        logger::write("ModLoader: texture store grown to %u entries", cap);
        s = g;
    }
    s->entries[index] = e;
    s->put_key((uint64_t)e.res, index);
    g_tex_count.store(index + 1, std::memory_order_release);
    g_tex_gen.fetch_add(1, std::memory_order_acq_rel);
}
unsigned ModLoader::TexStoreGen()
{
    return g_tex_gen.load(std::memory_order_acquire);
}
int ModLoader::TexIndexOf(ID3D12Resource* res)
{
    const TexStore* s = g_tex_store.load(std::memory_order_acquire);
    if (!res || !s) return -1;
    const uint64_t key = (uint64_t)res;
    for (uint32_t i = s->slot(key), n = 0; n < s->icap; ++n, i = (i + 1) & (s->icap - 1)) {
        const uint64_t k = s->key[i].load(std::memory_order_acquire);
        if (k == key) {
            const uint32_t v = s->val[i].load(std::memory_order_acquire);
            return (v < g_tex_count.load(std::memory_order_acquire)) ? (int)v : -1;
        }
        if (k == 0) return -1;
    }
    return -1;
}
bool ModLoader::TexEntryAt(int index, TexEntry* out)
{
    const TexStore* s = g_tex_store.load(std::memory_order_acquire);
    if (!out || !s || index < 0 || (uint32_t)index >= g_tex_count.load(std::memory_order_acquire) || (uint32_t)index >= s->cap) return false;
    *out = s->entries[index];
    return out->res != nullptr && out->srv.ptr != 0;
}
ID3D12Resource* ModLoader::TexResOfSrv(D3D12_CPU_DESCRIPTOR_HANDLE srv)
{
    const TexStore* s = g_tex_store.load(std::memory_order_acquire);
    if (!srv.ptr || !s) return nullptr;
    const uint32_t n = (std::min)(g_tex_count.load(std::memory_order_acquire), s->cap);
    for (uint32_t i = 0; i < n; ++i)
        if (s->entries[i].srv.ptr == srv.ptr) return s->entries[i].res;
    return nullptr;
}
static bool g_tex_ovr_loaded = false;

// Ini parsing is owned by the ported engine; runs once it has loaded: the SRV heap, sized from what it declared.
void ModLoader::LoadTextureOverrides()
{
    if (g_tex_ovr_loaded) return;
    ID3D12Device* dev = g_device ? reinterpret_cast<ID3D12Device*>(g_device->get_native()) : nullptr;
    if (!dev) return;
    g_tex_ovr_loaded = true;
    if (!g_srv_heap) {
        unsigned                   n_dds = 0;
        const uint32_t             cap = SrvHeapCapacity(&n_dds);
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, cap, D3D12_DESCRIPTOR_HEAP_FLAG_NONE };
        HRESULT                    hr = dev->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&g_srv_heap));
        if (FAILED(hr)) {
            logger::write("ModLoader Error: Failed to create SRV heap hr=%08x", hr);
            return;
        }
        g_srv_descriptor_size = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        g_srv_heap_cap = cap;
        TexStoreAlloc(cap);
        logger::write("ModLoader: mod SRV heap sized %u for the library (%u .dds declared)", cap, n_dds);
    }
    logger::write("ModLoader: %zu mods (from the inis their engine parsed)", EnumerateMods().size());
}
bool ModLoader::SubstantiateWclResources()
{
    PROF(PROF_ET_TexStore);
    ID3D12Device* dev = g_device ? reinterpret_cast<ID3D12Device*>(g_device->get_native()) : nullptr;
    if (!dev || !g_srv_heap) return false;
    {
        static unsigned s_store_gen = 0;
        static long     s_born = -1;
        static unsigned s_calls = 0;
        const unsigned  gen = PortApi::LoadGeneration();
        if (s_store_gen != gen) {
            s_store_gen = gen;
            TexStoreClear(); // the lock-free read side first, so no reader resolves into the old table
            g_tex_resources.clear();
            s_born = -1;
        }
        const long born = PortApi::WrapResourcesCreated();
        if (born == s_born && (++s_calls & 7u)) return true;
        s_born = born;
        s_calls = 0;
    }
    auto srv_format_for = [](DXGI_FORMAT f) -> DXGI_FORMAT {
        switch (f) {
        case DXGI_FORMAT_BC1_TYPELESS: return DXGI_FORMAT_BC1_UNORM;
        case DXGI_FORMAT_BC2_TYPELESS: return DXGI_FORMAT_BC2_UNORM;
        case DXGI_FORMAT_BC3_TYPELESS: return DXGI_FORMAT_BC3_UNORM;
        case DXGI_FORMAT_BC4_TYPELESS: return DXGI_FORMAT_BC4_UNORM;
        case DXGI_FORMAT_BC5_TYPELESS: return DXGI_FORMAT_BC5_UNORM;
        case DXGI_FORMAT_BC6H_TYPELESS: return DXGI_FORMAT_BC6H_UF16;
        case DXGI_FORMAT_BC7_TYPELESS: return DXGI_FORMAT_BC7_UNORM;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
        case DXGI_FORMAT_R8G8_TYPELESS: return DXGI_FORMAT_R8G8_UNORM;
        case DXGI_FORMAT_R8_TYPELESS: return DXGI_FORMAT_R8_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
        case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_FLOAT;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case DXGI_FORMAT_R32G32B32_TYPELESS: return DXGI_FORMAT_R32G32B32_FLOAT;
        case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
        case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        default: return f; // already fully qualified
        }
    };

    int registered = 0;
    struct Ctx {
        int*          registered;
        ID3D12Device* dev;
        DXGI_FORMAT (*srv_fmt)(DXGI_FORMAT);
    } ctx{ &registered, dev, srv_format_for };
    PortApi::ForEachCustomResource(
        [](const wchar_t* fq_name, const wchar_t* filename, int type, bool substantiated,
            ID3D12Resource* dx12, void* user) {
            Ctx* c = (Ctx*)user;
            (void)type;
            (void)substantiated;
            if (!dx12) return; // not substantiated yet, or nothing to create
            const D3D12_RESOURCE_DESC rd = dx12->GetDesc();
            if (rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return; // buffers are not swapped

            if (ModLoader::TexIndexOf(dx12) >= 0) return;

            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = c->srv_fmt(rd.Format);
            if (srv.Format != rd.Format)
                logger::write("PortRes: typeless resource format %d described as %d for the SRV",
                    (int)rd.Format, (int)srv.Format);
            if (srv.Format == DXGI_FORMAT_UNKNOWN) {
                logger::write("PortRes: SKIPPING SRV -- resource format %d has no view-legal form",
                    (int)rd.Format);
                return;
            }
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = rd.MipLevels ? rd.MipLevels : 1;

            const D3D12_CPU_DESCRIPTOR_HANDLE h = NextModSrv();
            if (!h.ptr) return;
            c->dev->CreateShaderResourceView(dx12, &srv, h);

            g_tex_resources.push_back({ h, true, dx12, filename && *filename ? filename : fq_name });
            TexStorePublish((uint32_t)g_tex_resources.size() - 1, ModLoader::TexEntry{ h, dx12 });
            ++*c->registered;
        },
        &ctx,
        [](ID3D12Resource* dx12, void*) { return dx12 && ModLoader::TexIndexOf(dx12) < 0; });

    if (registered > 0)
        logger::write("PortRes: described %d newly substantiated texture(s); %zu total, %u SRV slots used",
            registered, g_tex_resources.size(), g_srv_heap_size);
    return true;
}

const std::vector<ModLoader::TexResource>& ModLoader::TexResources()
{
    return g_tex_resources;
}
std::vector<ModLoader::DiscoveredMod> ModLoader::EnumerateMods()
{
    struct Found {
        fs::path root;
        ModIni   ini;
    };
    std::vector<Found> found;
    PortApi::ForEachModIni(
        [](const wchar_t* ini, const wchar_t* ns, const wchar_t* root, void* user) {
            static_cast<std::vector<Found>*>(user)->push_back({ root, { ini, ns } });
        },
        &found);
    auto lower = [](std::wstring s) {
        for (auto& c : s) c = (wchar_t)towlower(c);
        return s;
    };
    std::unordered_set<std::wstring> ini_dirs;
    for (const auto& f : found) ini_dirs.insert(lower(fs::path(f.ini.path).parent_path().wstring()));
    std::map<fs::path, DiscoveredMod> mods;
    for (auto& f : found) {
        const fs::path folder = fs::path(f.ini.path).parent_path();
        fs::path       key = f.ini.path;
        fs::path       dir = f.root;
        fs::path       walk = f.root;
        for (const auto& part : folder.lexically_relative(f.root)) {
            if (part == L".") break;
            walk /= part;
            if (ini_dirs.count(lower(walk.wstring()))) {
                key = walk;
                dir = walk;
                break;
            }
        }
        DiscoveredMod& m = mods[key];
        m.dir = dir.wstring();
        m.inis.push_back(std::move(f.ini));
    }
    std::vector<DiscoveredMod> out;
    for (auto& kv : mods) {
        std::sort(kv.second.inis.begin(), kv.second.inis.end(),
            [](const ModIni& a, const ModIni& b) { return a.path < b.path; });
        out.push_back(std::move(kv.second));
    }
    return out;
}

void ModLoader::Tick()
{
    if (g_reload_pending) {
        DoReload();
        g_reload_pending = false;
    }
}
void ModLoader::Reload()
{
    g_reload_pending = true;
}
