#pragma once
#include <string>
#include <d3d12.h>
#include <d3d12sdklayers.h>

// The mods directory, set in ActivateD3D12.
extern std::wstring g_mods_dir;

extern ID3D12InfoQueue* g_infoq;

// Drain that queue into our log, tagged with where the drain happened.
void DrainDebugLayer(const char* when);
