#pragma once

#include <d3d11_1.h>
#include <nvapi.h>

DEFINE_GUID(IID_HackerDevice,
    0x83ffd841, 0xa5c9, 0x46f4, 0x81, 0x9, 0xbc, 0x25, 0x95, 0x58, 0xfe, 0xf4);
#include "globals.h"
#include "Overlay.h"
#include "dx12_device.h" // MigotoPort::Device(): the device-side DX11->DX12 transitions
#include "dx12_wrap.h"   // MigotoPort::BackBufferAsD3D11(): `bb` as a resource their engine can bind

class Overlay; // theirs is an ImGui overlay; we have our own, so this stays an opaque null

class BackBufferSwapChain1 : public IDXGISwapChain1 {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** pp) override
    {
        if (pp) *pp = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE   AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE   Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID, void** pp) override
    {
        if (pp) *pp = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void** pp) override
    {
        if (pp) *pp = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE Present(UINT, UINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT, REFIID, void** ppSurface) override
    {
        if (!ppSurface) return E_POINTER;
        *ppSurface = MigotoPort::BackBufferAsD3D11();
        return *ppSurface ? S_OK : E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL, IDXGIOutput*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL*, IDXGIOutput**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT, UINT, UINT, DXGI_FORMAT, UINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** pp) override
    {
        if (pp) *pp = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID, void** pp) override
    {
        if (pp) *pp = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE Present1(UINT, UINT, const DXGI_PRESENT_PARAMETERS*) override { return E_NOTIMPL; }
    BOOL STDMETHODCALLTYPE    IsTemporaryMonoSupported() override { return FALSE; }
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** pp) override
    {
        if (pp) *pp = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION*) override { return E_NOTIMPL; }
};

class HackerSwapChain {
public:
    IDXGISwapChain1* mOrigSwapChain1 = nullptr;
    Overlay*         mOverlay = nullptr; // CommandList.cpp:2033-2034 reads it

    IDXGISwapChain1* GetOrigSwapChain1()
    {
        static BackBufferSwapChain1 s;
        return &s;
    }
    HRESULT GetBuffer(UINT /*Buffer*/, REFIID /*riid*/, void** ppSurface)
    {
        if (!ppSurface) return E_POINTER;
        *ppSurface = MigotoPort::BackBufferAsD3D11(); // AddRef'd; their code Releases it
        return *ppSurface ? S_OK : E_NOINTERFACE;     // no back buffer published yet == not available
    }
};

class HackerDevice {
public:
    HRESULT STDMETHODCALLTYPE CreateCustomInputLayout(
        const D3D11_INPUT_ELEMENT_DESC* pInputElementDescs, UINT NumElements,
        const void* pShaderBytecodeWithInputSignature, SIZE_T BytecodeLength,
        ID3D11InputLayout** ppInputLayout);

    ID3D11Texture1D*          mIniTexture = nullptr;
    ID3D11ShaderResourceView* mIniResourceView = nullptr;

    ID3D11Device1*        GetPassThroughOrigDevice1() { return (ID3D11Device1*)MigotoPort::Device(); }
    ID3D11Device1*        GetPossiblyHookedOrigDevice1() { return (ID3D11Device1*)MigotoPort::Device(); }
    ID3D11DeviceContext1* GetPassThroughOrigContext1() { return nullptr; }
    HackerSwapChain*      GetHackerSwapChain() { return &mSwapChainShim; }
    // theirs: HackerDevice.h:131. Override.cpp runs command lists through it.
    class HackerContext* GetHackerContext() { return mHackerContext; }

    void CreateIniParamResources()
    {
        if (G->iniParams.size() < (size_t)G->iniParamsReserved)
            G->iniParams.resize(G->iniParamsReserved);

        // Resizing IniParams must release the old versions (theirs :265-275).
        if (mIniResourceView) {
            mIniResourceView->Release();
            mIniResourceView = nullptr;
        }
        if (mIniTexture) {
            mIniTexture->Release();
            mIniTexture = nullptr;
        }

        if (G->iniParams.empty())
            return; // theirs :283 -- nothing to describe

        ID3D11Device1* dev = GetPassThroughOrigDevice1();
        if (!dev) return;

        D3D11_SUBRESOURCE_DATA initialData{};
        initialData.pSysMem = G->iniParams.data();
        initialData.SysMemPitch = (UINT)(sizeof(DirectX::XMFLOAT4) * G->iniParams.size());

        D3D11_TEXTURE1D_DESC desc{};
        desc.Width = (UINT)G->iniParams.size(); // n texels, .rgba as a float4
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        desc.Usage = D3D11_USAGE_DYNAMIC;            // rewritten before every dispatch
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE; // accessed as t120
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        desc.MiscFlags = 0;
        if (FAILED(dev->CreateTexture1D(&desc, &initialData, &mIniTexture)) || !mIniTexture) {
            LogInfo("    IniParams CreateTexture1D FAILED\n");
            return;
        }
        // NULL desc: use the texture's own format (theirs :318).
        if (FAILED(dev->CreateShaderResourceView(mIniTexture, NULL, &mIniResourceView))) {
            LogInfo("    IniParams CreateShaderResourceView FAILED\n");
            return;
        }
        LogInfo("    IniParams texture %p + view %p created for %Iu params\n",
            mIniTexture, mIniResourceView, G->iniParams.size());
    }

    class HackerContext* mHackerContext = nullptr; // set by PortApi::Load, which pairs g_dev with g_ctx

private:
    HackerSwapChain mSwapChainShim; // so GetHackerSwapChain()->... does not dereference null
};
