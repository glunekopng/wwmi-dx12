#include "mod_loader.h"
#include "log.h"
#include "../reshade_addon/textures/tex_lod.h"         // TexLodInit: the LOD cache read again on F5
#include "../reshade_addon/migoto/port_api.h"          // their [Resource*] registry
#include "../reshade_addon/instrumentation/profiler.h" // ET:TexStore
#include <windows.h>
#include <atomic>
#include <vector>
#include <algorithm>
#include <string>
#include <filesystem>
#include <cwctype> // towlower -- SrvHeapCapacityFor's extension match
#include <d3d12.h>
#include <wrl.h>

using Microsoft::WRL::ComPtr;
using namespace reshade::api;
namespace fs = std::filesystem;

static device*      g_device = nullptr;
static std::wstring g_mods_dir = L"";
static bool         g_reload_pending = false;

static int g_last_mods = 0;

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
static uint32_t SrvHeapCapacityFor(const std::wstring& mods_dir, unsigned* dds_out)
{
    unsigned        dds = 0;
    std::error_code ec;
    if (!mods_dir.empty() && fs::is_directory(mods_dir, ec)) {
        for (fs::recursive_directory_iterator it(mods_dir, fs::directory_options::skip_permission_denied, ec), end;
            !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            std::wstring ext = it->path().extension().wstring();
            for (auto& c : ext) c = (wchar_t)towlower(c);
            if (ext == L".dds") ++dds;
        }
    }
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

void ModLoader::SetDir(const wchar_t* path)
{
    g_mods_dir = path;
}

void ModLoader::GetLastStats(int& mods)
{
    mods = g_last_mods;
}

static void DoReload()
{
    if (!g_device) return;
    if (g_device->get_api() != reshade::api::device_api::d3d12) {
        logger::write("ModLoader: skipping reload -- device is not D3D12 (api=%u)", (unsigned)g_device->get_api());
        return;
    }
    ID3D12Device* d3d12_dev = reinterpret_cast<ID3D12Device*>(g_device->get_native());
    if (!d3d12_dev) return;

    logger::write("ModLoader: scanning directory %S", g_mods_dir.c_str());
    TexLodInit(); // the LOD cache read again, and any seed dropped in since taken

    g_last_mods = 0;
    g_srv_heap_size = 0;

    if (!g_srv_heap) {
        unsigned                   n_dds = 0;
        const uint32_t             cap = SrvHeapCapacityFor(g_mods_dir, &n_dds);
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, cap, D3D12_DESCRIPTOR_HEAP_FLAG_NONE };
        HRESULT                    hr = d3d12_dev->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&g_srv_heap));
        if (SUCCEEDED(hr)) {
            g_srv_descriptor_size = d3d12_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            g_srv_heap_cap = cap;
            TexStoreAlloc(cap);
            logger::write("ModLoader: mod SRV heap sized %u for the library (%u .dds under %S)",
                cap, n_dds, g_mods_dir.c_str());
        } else {
            logger::write("ModLoader Error: Failed to create SRV heap hr=%08x", hr);
            return;
        }
    }

    std::error_code ec;
    if (!fs::exists(g_mods_dir, ec)) return;
    g_last_mods = (int)ModLoader::EnumerateMods().size();
    logger::write("ModLoader: reload complete. %d mods", g_last_mods);
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

// Ini parsing is owned by the ported engine; this only reports the mod folder count once the heap exists.
void ModLoader::LoadTextureOverrides()
{
    if (g_tex_ovr_loaded) return;
    ID3D12Device* dev = g_device ? reinterpret_cast<ID3D12Device*>(g_device->get_native()) : nullptr;
    if (!dev || !g_srv_heap) return; // wait until heap exists (created in Reload)
    g_tex_ovr_loaded = true;

    logger::write("ModLoader: %zu mod folders discovered (ini parsing is owned by the Wcl engine)",
        EnumerateMods().size());
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
bool ModLoader::IsExcludedEntry(const wchar_t* name)
{
    if (!name) return false;
    // WWMI-shipped d3dx.ini [Include] exclude_recursive globs: "DISABLED*" and "desktop.ini".
    if (_wcsnicmp(name, L"DISABLED", 8) == 0) return true; // case-insensitive "DISABLED" prefix
    if (_wcsicmp(name, L"desktop.ini") == 0) return true;
    return false;
}

std::vector<ModLoader::DiscoveredMod> ModLoader::EnumerateMods()
{
    std::vector<DiscoveredMod> out;
    std::error_code            ec;
    if (g_mods_dir.empty() || !fs::exists(g_mods_dir, ec)) return out;

    // Top-level folders = mods. Sorted so mod_id assignment is stable across runs.
    std::vector<fs::path> folders;
    for (auto& e : fs::directory_iterator(g_mods_dir, ec)) {
        if (!e.is_directory(ec)) continue;
        if (IsExcludedEntry(e.path().filename().c_str())) continue; // DISABLED* mod folder = off
        folders.push_back(e.path());
    }
    std::sort(folders.begin(), folders.end());

    for (auto& f : folders) {
        DiscoveredMod m;
        m.dir = f.wstring();
        std::error_code ec2;
        for (auto& fe : fs::recursive_directory_iterator(f, ec2)) {
            if (ec2) break;
            if (!fe.is_regular_file(ec2)) continue;
            if (_wcsicmp(fe.path().extension().wstring().c_str(), L".ini") != 0) continue;
            // Skip .ini under any DISABLED* subfolder, or named DISABLED*/desktop.ini (per-component check).
            fs::path rel = fs::relative(fe.path(), f, ec2);
            bool     skip = false;
            for (const auto& part : rel)
                if (IsExcludedEntry(part.c_str())) {
                    skip = true;
                    break;
                }
            if (skip) continue;
            m.inis.push_back(fe.path().wstring());
        }
        std::sort(m.inis.begin(), m.inis.end()); // deterministic merge order (mod-fix < mod-Lod < mod-auto-generated)
        if (!m.inis.empty()) out.push_back(std::move(m));
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
