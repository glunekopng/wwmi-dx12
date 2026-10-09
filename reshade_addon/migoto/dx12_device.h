#pragma once
// ===========================================================================
//  dx12_device.h -- OURS. PORT-LEAF. ID3D11Device1 as a DX11->DX12 transition surface.
//
//  Their CustomResource::Substantiate* and CustomShader::compile issue Create* calls through
//  CommandListState::mOrigDevice1, so returning null there faults.
//
//  Signatures GENERATED from the SDK (tools/gen_dx12_device.py). Defaults are loud E_NOTIMPL so an
//  unimplemented call can never look like it succeeded.
// ===========================================================================

#include <d3d11_1.h>
#include <d3d12.h>

namespace MigotoPort {

class Dx12Device : public ID3D11Device1 {
public:
    Dx12Device() = default;
    virtual ~Dx12Device() = default;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG STDMETHODCALLTYPE   AddRef() override;
    ULONG STDMETHODCALLTYPE   Release() override;

    void STDMETHODCALLTYPE              GetImmediateContext1(ID3D11DeviceContext1** ppImmediateContext) override;
    HRESULT STDMETHODCALLTYPE           CreateDeferredContext1(UINT ContextFlags, _COM_Outptr_opt_ ID3D11DeviceContext1** ppDeferredContext) override;
    HRESULT STDMETHODCALLTYPE           CreateBlendState1(const D3D11_BLEND_DESC1* pBlendStateDesc, _COM_Outptr_opt_ ID3D11BlendState1** ppBlendState) override;
    HRESULT STDMETHODCALLTYPE           CreateRasterizerState1(const D3D11_RASTERIZER_DESC1* pRasterizerDesc, _COM_Outptr_opt_ ID3D11RasterizerState1** ppRasterizerState) override;
    HRESULT STDMETHODCALLTYPE           CreateDeviceContextState(UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels, UINT SDKVersion, REFIID EmulatedInterface, D3D_FEATURE_LEVEL* pChosenFeatureLevel, ID3DDeviceContextState** ppContextState) override;
    HRESULT STDMETHODCALLTYPE           OpenSharedResource1(HANDLE hResource, REFIID returnedInterface, _COM_Outptr_ void** ppResource) override;
    HRESULT STDMETHODCALLTYPE           OpenSharedResourceByName(LPCWSTR lpName, DWORD dwDesiredAccess, REFIID returnedInterface, _COM_Outptr_ void** ppResource) override;
    HRESULT STDMETHODCALLTYPE           CreateBuffer(const D3D11_BUFFER_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, _COM_Outptr_opt_ ID3D11Buffer** ppBuffer) override;
    HRESULT STDMETHODCALLTYPE           CreateTexture1D(const D3D11_TEXTURE1D_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, _COM_Outptr_opt_ ID3D11Texture1D** ppTexture1D) override;
    HRESULT STDMETHODCALLTYPE           CreateTexture2D(const D3D11_TEXTURE2D_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, _COM_Outptr_opt_ ID3D11Texture2D** ppTexture2D) override;
    HRESULT STDMETHODCALLTYPE           CreateTexture3D(const D3D11_TEXTURE3D_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, _COM_Outptr_opt_ ID3D11Texture3D** ppTexture3D) override;
    HRESULT STDMETHODCALLTYPE           CreateShaderResourceView(ID3D11Resource* pResource, const D3D11_SHADER_RESOURCE_VIEW_DESC* pDesc, _COM_Outptr_opt_ ID3D11ShaderResourceView** ppSRView) override;
    HRESULT STDMETHODCALLTYPE           CreateUnorderedAccessView(ID3D11Resource* pResource, const D3D11_UNORDERED_ACCESS_VIEW_DESC* pDesc, _COM_Outptr_opt_ ID3D11UnorderedAccessView** ppUAView) override;
    HRESULT STDMETHODCALLTYPE           CreateRenderTargetView(ID3D11Resource* pResource, const D3D11_RENDER_TARGET_VIEW_DESC* pDesc, _COM_Outptr_opt_ ID3D11RenderTargetView** ppRTView) override;
    HRESULT STDMETHODCALLTYPE           CreateDepthStencilView(ID3D11Resource* pResource, const D3D11_DEPTH_STENCIL_VIEW_DESC* pDesc, _COM_Outptr_opt_ ID3D11DepthStencilView** ppDepthStencilView) override;
    HRESULT STDMETHODCALLTYPE           CreateInputLayout(const D3D11_INPUT_ELEMENT_DESC* pInputElementDescs, UINT NumElements, const void* pShaderBytecodeWithInputSignature, SIZE_T BytecodeLength, _COM_Outptr_opt_ ID3D11InputLayout** ppInputLayout) override;
    HRESULT STDMETHODCALLTYPE           CreateVertexShader(const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage* pClassLinkage, _COM_Outptr_opt_ ID3D11VertexShader** ppVertexShader) override;
    HRESULT STDMETHODCALLTYPE           CreateGeometryShader(const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage* pClassLinkage, _COM_Outptr_opt_ ID3D11GeometryShader** ppGeometryShader) override;
    HRESULT STDMETHODCALLTYPE           CreateGeometryShaderWithStreamOutput(const void* pShaderBytecode, SIZE_T BytecodeLength, const D3D11_SO_DECLARATION_ENTRY* pSODeclaration, UINT NumEntries, const UINT* pBufferStrides, UINT NumStrides, UINT RasterizedStream, ID3D11ClassLinkage* pClassLinkage, _COM_Outptr_opt_ ID3D11GeometryShader** ppGeometryShader) override;
    HRESULT STDMETHODCALLTYPE           CreatePixelShader(const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage* pClassLinkage, _COM_Outptr_opt_ ID3D11PixelShader** ppPixelShader) override;
    HRESULT STDMETHODCALLTYPE           CreateHullShader(const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage* pClassLinkage, _COM_Outptr_opt_ ID3D11HullShader** ppHullShader) override;
    HRESULT STDMETHODCALLTYPE           CreateDomainShader(const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage* pClassLinkage, _COM_Outptr_opt_ ID3D11DomainShader** ppDomainShader) override;
    HRESULT STDMETHODCALLTYPE           CreateComputeShader(const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage* pClassLinkage, _COM_Outptr_opt_ ID3D11ComputeShader** ppComputeShader) override;
    HRESULT STDMETHODCALLTYPE           CreateClassLinkage(_COM_Outptr_ ID3D11ClassLinkage** ppLinkage) override;
    HRESULT STDMETHODCALLTYPE           CreateBlendState(const D3D11_BLEND_DESC* pBlendStateDesc, _COM_Outptr_opt_ ID3D11BlendState** ppBlendState) override;
    HRESULT STDMETHODCALLTYPE           CreateDepthStencilState(const D3D11_DEPTH_STENCIL_DESC* pDepthStencilDesc, _COM_Outptr_opt_ ID3D11DepthStencilState** ppDepthStencilState) override;
    HRESULT STDMETHODCALLTYPE           CreateRasterizerState(const D3D11_RASTERIZER_DESC* pRasterizerDesc, _COM_Outptr_opt_ ID3D11RasterizerState** ppRasterizerState) override;
    HRESULT STDMETHODCALLTYPE           CreateSamplerState(const D3D11_SAMPLER_DESC* pSamplerDesc, _COM_Outptr_opt_ ID3D11SamplerState** ppSamplerState) override;
    HRESULT STDMETHODCALLTYPE           CreateQuery(const D3D11_QUERY_DESC* pQueryDesc, _COM_Outptr_opt_ ID3D11Query** ppQuery) override;
    HRESULT STDMETHODCALLTYPE           CreatePredicate(const D3D11_QUERY_DESC* pPredicateDesc, _COM_Outptr_opt_ ID3D11Predicate** ppPredicate) override;
    HRESULT STDMETHODCALLTYPE           CreateCounter(const D3D11_COUNTER_DESC* pCounterDesc, _COM_Outptr_opt_ ID3D11Counter** ppCounter) override;
    HRESULT STDMETHODCALLTYPE           CreateDeferredContext(UINT ContextFlags, _COM_Outptr_opt_ ID3D11DeviceContext** ppDeferredContext) override;
    HRESULT STDMETHODCALLTYPE           OpenSharedResource(HANDLE hResource, REFIID ReturnedInterface, _COM_Outptr_opt_ void** ppResource) override;
    HRESULT STDMETHODCALLTYPE           CheckFormatSupport(DXGI_FORMAT Format, UINT* pFormatSupport) override;
    HRESULT STDMETHODCALLTYPE           CheckMultisampleQualityLevels(DXGI_FORMAT Format, UINT SampleCount, UINT* pNumQualityLevels) override;
    void STDMETHODCALLTYPE              CheckCounterInfo(D3D11_COUNTER_INFO* pCounterInfo) override;
    HRESULT STDMETHODCALLTYPE           CheckCounter(const D3D11_COUNTER_DESC* pDesc, D3D11_COUNTER_TYPE* pType, UINT* pActiveCounters, LPSTR szName, UINT* pNameLength, LPSTR szUnits, UINT* pUnitsLength, LPSTR szDescription, UINT* pDescriptionLength) override;
    HRESULT STDMETHODCALLTYPE           CheckFeatureSupport(D3D11_FEATURE Feature, void* pFeatureSupportData, UINT FeatureSupportDataSize) override;
    HRESULT STDMETHODCALLTYPE           GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override;
    HRESULT STDMETHODCALLTYPE           SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override;
    HRESULT STDMETHODCALLTYPE           SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override;
    D3D_FEATURE_LEVEL STDMETHODCALLTYPE GetFeatureLevel(void) override;
    UINT STDMETHODCALLTYPE              GetCreationFlags(void) override;
    HRESULT STDMETHODCALLTYPE           GetDeviceRemovedReason(void) override;
    void STDMETHODCALLTYPE              GetImmediateContext(ID3D11DeviceContext** ppImmediateContext) override;
    HRESULT STDMETHODCALLTYPE           SetExceptionMode(UINT RaiseFlags) override;
    UINT STDMETHODCALLTYPE              GetExceptionMode(void) override;

private:
    LONG m_ref = 1;
};

Dx12Device* Device();

} // namespace MigotoPort
