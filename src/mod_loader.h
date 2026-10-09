#pragma once
#include <d3d12.h>
#include <reshade.hpp>
#include <string>
#include <vector>

namespace ModLoader {

unsigned SrvHeapSize();
unsigned SrvHeapCap();
void     SetDir(const wchar_t* mods_dir);
void     Init(reshade::api::device* device);
void     Reload();
void     Tick(); // runs a pending Reload
void     GetLastStats(int& out_mods);

// A mod texture the host has loaded: SRV + resource + the file it came from.
struct TexResource {
    D3D12_CPU_DESCRIPTOR_HANDLE srv{};
    bool                        valid = false;
    ID3D12Resource*             res = nullptr;
    std::wstring                path;
};

void                            LoadTextureOverrides();
bool                            SubstantiateWclResources();
const std::vector<TexResource>& TexResources();
struct TexEntry {
    D3D12_CPU_DESCRIPTOR_HANDLE srv{};
    ID3D12Resource*             res = nullptr;
};
int             TexIndexOf(ID3D12Resource* res);
bool            TexEntryAt(int index, TexEntry* out);
unsigned        TexStoreGen();
ID3D12Resource* TexResOfSrv(D3D12_CPU_DESCRIPTOR_HANDLE srv);

bool IsExcludedEntry(const wchar_t* name);

// One discovered mod: a top-level folder under the mods folder (d3dx.ini's include_recursive) and every .ini under it.
struct DiscoveredMod {
    std::wstring              dir;  // absolute mod folder
    std::vector<std::wstring> inis; // all .ini under dir (recursive), DISABLED*-filtered, sorted
};
std::vector<DiscoveredMod> EnumerateMods();
}
