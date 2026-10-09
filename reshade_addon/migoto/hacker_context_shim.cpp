#include "globals.h"
#include "HackerContext.h"
#include "HackerDevice.h"
#include "../../src/log.h"

void HackerContext::Bind3DMigotoResources()
{
    if (!mHackerDevice || !mHackerDevice->mIniResourceView || G->IniParamsReg < 0)
        return;
    ID3D11DeviceContext1* ctx = GetPassThroughOrigContext1();
    if (!ctx) return;
    ID3D11ShaderResourceView* srv = mHackerDevice->mIniResourceView;
    const UINT                slot = (UINT)G->IniParamsReg;
    ctx->VSSetShaderResources(slot, 1, &srv);
    ctx->HSSetShaderResources(slot, 1, &srv);
    ctx->DSSetShaderResources(slot, 1, &srv);
    ctx->GSSetShaderResources(slot, 1, &srv);
    ctx->PSSetShaderResources(slot, 1, &srv);
    ctx->CSSetShaderResources(slot, 1, &srv);
    logger::write("PORT: IniParams bound at t%u on all six stages (view %p)", slot, (void*)srv);
}

void HackerContext::InitIniParams()
{
    if (!G->iniParams.empty())
        memset(G->iniParams.data(), 0, sizeof(DirectX::XMFLOAT4) * G->iniParams.size());
    Bind3DMigotoResources();
}
