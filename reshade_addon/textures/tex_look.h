#pragma once
#include <cstdint>
#include <string>
#include <vector>

// The BC blocks under a mip0's five patches, cut on the upload thread: all a signature reads.
struct TexLookCut {
    uint32_t             fmt = 0;
    uint32_t             w = 0;
    uint32_t             h = 0;
    std::vector<uint8_t> blocks; // patch by patch, block row by block row
};

void TexLookInit(const std::wstring& cache_dir = std::wstring());
void TexLookFlushIfQuiet(unsigned quiet_ms);
void TexLookOnExit();

bool       TexLookWants(uint32_t hash, uint32_t w, uint32_t h, uint32_t fmt, bool is_target);
TexLookCut TexLookCutOf(const uint8_t* bc, size_t bc_size, uint32_t w, uint32_t h, uint32_t fmt);
void       TexLookOnCut(uint32_t hash, const TexLookCut& cut, bool is_target);

uint64_t TexLookPlaceOf(const wchar_t* ns, int comp, uint64_t ps); // a component draw's pass: one key on every install
void     TexLookAtPlace(uint64_t pass, uint32_t slot, uint32_t hash, uint32_t target);

uint64_t TexLookMeshPassOf(uint32_t ib_hash, uint32_t first_index, uint32_t index_count, uint64_t ps); // a game character draw's
uint64_t TexLookSlotOf(uint64_t pass, uint32_t slot);
bool     TexLookHasMeshPlaces();
uint32_t TexLookMeshMask(uint64_t pass, unsigned regs); // the registers below `regs` with a mesh place of one texture
void     TexLookAtMeshPlace(uint64_t place, uint32_t hash, uint32_t target);
void     TexLookAddMeshPlace(uint64_t place, uint32_t target);
