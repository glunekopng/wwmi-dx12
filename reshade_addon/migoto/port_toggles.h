#pragma once
// Behaviour toggles of the ported engine's DX12 layer, each an A/B kept for a reason.

namespace MigotoPort {

inline bool g_inherit_game_viewport = true;

inline bool g_uav_barrier = true;

inline bool g_blend_write_all_rt = false;

inline bool g_port_post_split = true;

} // namespace MigotoPort
