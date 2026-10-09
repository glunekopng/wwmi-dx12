#pragma once
// Shared by dx12_pipeline.cpp and dx12_skcap.cpp; nothing else includes it.
#include <d3d12.h>
#include <atomic>
#include <cstdint>

namespace MigotoPort {

// Compute UAV registers bound on the current dispatch: set by BindDescriptors, taken by the capture.
extern std::atomic<uint32_t> g_cs_uav_bound_mask;
// IssueDispatch's no-list/no-device exit, recorded for the capture's manifest (dx12_skcap.cpp).
void SkCapRecordBail(const void* cs, UINT x, UINT y, UINT z, bool had_device);

} // namespace MigotoPort
