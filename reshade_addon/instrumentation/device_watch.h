#pragma once
// Device-removal watch: a poller thread that writes the removal (DRED, the debug layer) into our log.
#include <reshade.hpp>

void StartRemovalWatchdog(reshade::api::swapchain* sc); // once, from the first present with a device
void DredWatchdogStop();                                // destroy_device: the poller ends

void StallWatchdogTick(bool focused);
void StallWatchdogStop();
