#pragma once
#include <cstdint>
#include <string>
#include <reshade.hpp>
#include "../migoto/port_api.h" // PortApi::DrawInfo

extern thread_local uint64_t t_cur_vs_hash;
extern thread_local uint64_t t_cur_ps_hash;

uint64_t AliasShaderHash(uint64_t h);
uint64_t AliasShaderHashAlways(uint64_t h);

// Does any [ShaderOverride*] target this hash? Optionally returns the first section name.
bool ShaderOverrideMatches(uint64_t shader_hash, std::wstring* first_section);
// Run them. True means a section cancelled the game's own draw or dispatch.
bool RunShaderOverrides(uint64_t shader_hash, const PortApi::DrawInfo* draw = nullptr,
    reshade::api::command_list* cmd_list = nullptr);
