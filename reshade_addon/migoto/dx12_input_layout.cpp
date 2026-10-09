#include "dx12_input_layout.h"
#include "dx12_wrap.h"
#include <wrl/client.h> // Microsoft::WRL::ComPtr, for the layout blob

EXTERN_C const GUID InputLayoutDescGuid;
#include <d3dcompiler.h>
#include <mutex>
#include <unordered_map>
#include <string>
#include <vector>
#include <cstdio> // _snprintf_s for DescribeInputLayout (X20)

namespace MigotoPort {

namespace {

class LayoutStandIn final : public ::ID3D11InputLayout {
public:
    Microsoft::WRL::ComPtr<ID3DBlob>      blob;
    std::vector<D3D11_INPUT_ELEMENT_DESC> elems;
    std::vector<std::string>              names; // SemanticName storage; elems point into it

    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&rc_); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG n = InterlockedDecrement(&rc_);
        if (n <= 0) {
            delete this;
            return 0;
        }
        return (ULONG)n;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(::ID3D11InputLayout) || riid == __uuidof(ID3D11DeviceChild) ||
            riid == __uuidof(IUnknown)) {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    void STDMETHODCALLTYPE GetDevice(ID3D11Device** pp) override
    {
        if (!pp) return;
        *pp = PortDeviceAsD3D11();
        (*pp)->AddRef();
    }

    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override
    {
        if (!pDataSize) return E_INVALIDARG;
        if (guid == InputLayoutDescGuid && blob) {
            if (!pData || *pDataSize < sizeof(ID3DBlob*)) {
                *pDataSize = sizeof(ID3DBlob*);
                return S_OK;
            }
            ID3DBlob* b = blob.Get();
            b->AddRef();
            *(ID3DBlob**)pData = b;
            *pDataSize = sizeof(ID3DBlob*);
            return S_OK;
        }
        *pDataSize = 0;
        return DXGI_ERROR_NOT_FOUND;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return S_OK; }

private:
    LONG rc_ = 1;
};

std::unordered_map<unsigned long long, LayoutStandIn*> g_layouts;
std::mutex                                             g_layout_mtx;
LayoutStandIn*                                         g_current = nullptr;

} // namespace

void RecordInputLayout(unsigned long long pso, const InputElement* in, unsigned count)
{
    if (!pso || !in || !count) return;

    auto* L = new LayoutStandIn();
    L->names.reserve(count);
    L->elems.reserve(count);
    for (unsigned i = 0; i < count; ++i)
        L->names.emplace_back(in[i].semantic ? in[i].semantic : "");
    for (unsigned i = 0; i < count; ++i) {
        D3D11_INPUT_ELEMENT_DESC d{};
        d.SemanticName = L->names[i].c_str();
        d.SemanticIndex = in[i].semantic_index;
        d.Format = (DXGI_FORMAT)in[i].format;
        d.InputSlot = in[i].buffer_binding;
        d.AlignedByteOffset = in[i].offset;
        d.InputSlotClass = in[i].instance_step_rate ? D3D11_INPUT_PER_INSTANCE_DATA
                                                    : D3D11_INPUT_PER_VERTEX_DATA;
        d.InstanceDataStepRate = in[i].instance_step_rate;
        L->elems.push_back(d);
    }
    const SIZE_T bytes = L->elems.size() * sizeof(D3D11_INPUT_ELEMENT_DESC);
    if (SUCCEEDED(D3DCreateBlob(bytes, &L->blob)) && L->blob)
        memcpy(L->blob->GetBufferPointer(), L->elems.data(), bytes);

    std::lock_guard<std::mutex> l(g_layout_mtx);
    auto                        it = g_layouts.find(pso);
    if (it != g_layouts.end()) { // PSO handles are reused after destruction
        if (g_current == it->second) g_current = nullptr;
        it->second->Release();
    }
    g_layouts[pso] = L;
}

void SetCurrentInputLayout(unsigned long long pso)
{
    std::lock_guard<std::mutex> l(g_layout_mtx);
    auto                        it = g_layouts.find(pso);
    g_current = (it != g_layouts.end()) ? it->second : nullptr;
}

::ID3D11InputLayout* CurrentInputLayout()
{
    std::lock_guard<std::mutex> l(g_layout_mtx);
    if (!g_current) return nullptr;
    g_current->AddRef(); // IAGetInputLayout AddRefs; their DumpVBs Releases it at the end
    return g_current;
}

unsigned DescribeInputLayout(unsigned long long pso, char* buf, unsigned n)
{
    if (!buf || !n) return 0;
    buf[0] = 0;
    std::lock_guard<std::mutex> l(g_layout_mtx);
    auto                        it = g_layouts.find(pso);
    if (it == g_layouts.end() || !it->second) return 0;
    const auto& el = it->second->elems;
    unsigned    off = 0;
    for (size_t i = 0; i < el.size() && off + 64 < n; ++i) {
        const int w = _snprintf_s(buf + off, n - off, _TRUNCATE, "%s%s%u@s%u+%u:f%u%s",
            i ? " " : "", el[i].SemanticName ? el[i].SemanticName : "?",
            el[i].SemanticIndex, el[i].InputSlot, el[i].AlignedByteOffset,
            (unsigned)el[i].Format,
            el[i].InputSlotClass == D3D11_INPUT_PER_INSTANCE_DATA ? "/inst" : "");
        if (w < 0) break;
        off += (unsigned)w;
    }
    return (unsigned)el.size();
}

} // namespace MigotoPort
