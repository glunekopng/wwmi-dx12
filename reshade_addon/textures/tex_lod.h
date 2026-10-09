#pragma once
#include <cstdint>
#include <string>
#include <vector>

void TexLodInit(const std::wstring& cache_dir = std::wstring());

void TexLodTargetsReady(bool (*is_target)(uint32_t hash, void* user), void* user);
void TexLodOnExit();
void TexLodFlushIfQuiet(unsigned quiet_ms);
void TexLodSetResemblance(bool on);
void TexLodSetCache(bool on);

std::vector<uint8_t> TexLodSampleOf(const uint8_t* bc, size_t bc_size, uint32_t w, uint32_t h, uint32_t fmt);
void                 TexLodOnSample(uint32_t hash, const std::vector<uint8_t>& sample,
    uint32_t w, uint32_t h, uint32_t fmt, bool is_target);

uint32_t TexLodLookup(uint32_t hash);
uint32_t TexLodLookupOwned(uint32_t hash, const wchar_t* ns, bool (*owns)(uint32_t high, const wchar_t* ns));
uint32_t TexLodKnown(uint32_t hash);
bool     TexLodDegenerate(uint32_t hash);

void     TexLodFeeds(uint32_t lower, uint32_t big, uint32_t big_rule, uint32_t steps); // (`small` is a windows.h macro)
uint32_t TexLodAddMipExact(uint32_t low, uint32_t high, unsigned mip, bool* now_degenerate);
void     TexLodRefute(uint32_t low, uint32_t high, const char* why);
