#pragma once
#include <reshade.hpp>
void DrawOverlay(reshade::api::effect_runtime* runtime);
void DrawRtBlockedNotice(reshade::api::effect_runtime* runtime); // ReShade's on-screen display, while g_rt_blocked is fresh

// Overlay controls that carry their id (overlay.cpp): "H20. label". Shared with the RT module's section.
bool NCheckbox(const char* id, const char* label, bool* v);
bool NButton(const char* id, const char* label);
