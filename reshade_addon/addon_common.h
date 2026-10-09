#pragma once
#ifndef IMGUI_DISABLE_INCLUDE_IMCONFIG_H
#define IMGUI_DISABLE_INCLUDE_IMCONFIG_H
#endif
#include <imgui.h>
#include <reshade.hpp>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <deque>
#include <d3d12.h>
#include <dxgi1_4.h> // IDXGIAdapter3::QueryVideoMemoryInfo -- the LOCAL/NON_LOCAL budget split
#include <psapi.h>   // GetProcessMemoryInfo (PrivateUsage) -- the CPU-heap half of the memory row
#include <wrl.h>
#include <string>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <ctime>
#include <atomic>
#include <set>
#include <regex>
#include <cstdio>
#include <share.h>
#include <sstream>
#include <memory>
#include <functional>
#include <thread>

#include "../src/log.h"
#include "build_stamp.h" // WWMI_BUILD_SHA -- so every log says which commit produced it
#include "../src/mod_loader.h"
#include "../src/fnv.h"
#include "addon_state.h"
#include "config.h"
#include "instrumentation/profiler.h"
#include "instrumentation/diagnostics.h"
#include "mods/ib_hash.h"
#include "instrumentation/crash_handler.h"
#include "migoto/frame_analysis_driver.h"
#include "migoto/dx12_bridge.h"       // SetResourceStateQuery
#include "migoto/dx12_input_layout.h" // RecordInputLayout / SetCurrentInputLayout / DescribeInputLayout
#include "DrawCallInfo.h"
#include "textures/texture_module.h"
#include "shapekeys/shapekey_chain.h" // the chain lives in its own file
#include "textures/tex_lod.h"
#include "mods/skeleton.h"
#include "ui/overlay.h"
#include "mods/wwmi_runtime.h"
#include "instrumentation/morph_dump.h"
#include "mods/wwmi_mod.h"
#include "migoto/port_api.h"      // the ported 3dmigoto engine
#include "migoto/dx12_pipeline.h" // MigotoPort::DispatchesIssued -- proof their compute reached the GPU
#include "migoto/dx12_wrap.h"     // AsWrapResource: the DX11 handle -> DX12 resource the bridge binds
#include "migoto/port_keygate.h"  // ShimUnmetTakeCount -- the heartbeat's unmet= field
#include "rabbitfx/pso_blend.h"

using namespace reshade::api;
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
