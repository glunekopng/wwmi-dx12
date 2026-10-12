#pragma once
// Every D3D12 buffer the addon and the port create. The description is the same for all of them.
#include <d3d12.h>

inline D3D12_RESOURCE_DESC D3D12BufferDesc(UINT64 bytes, D3D12_RESOURCE_FLAGS flags)
{
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_UNKNOWN;
    d.SampleDesc = { 1, 0 };
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = flags;
    return d;
}

inline HRESULT CreateD3D12Buffer(ID3D12Device* dev, D3D12_HEAP_TYPE heap, UINT64 bytes, D3D12_RESOURCE_STATES state,
    ID3D12Resource** out, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = heap;
    const D3D12_RESOURCE_DESC d = D3D12BufferDesc(bytes, flags);
    return dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(out));
}

inline HRESULT CreatePlacedD3D12Buffer(ID3D12Device* dev, ID3D12Heap* heap, UINT64 offset, UINT64 bytes,
    D3D12_RESOURCE_STATES state, ID3D12Resource** out)
{
    const D3D12_RESOURCE_DESC d = D3D12BufferDesc(bytes, D3D12_RESOURCE_FLAG_NONE);
    return dev->CreatePlacedResource(heap, offset, &d, state, nullptr, IID_PPV_ARGS(out));
}
