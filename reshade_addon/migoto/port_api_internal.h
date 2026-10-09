#pragma once
#include "port_api.h"
#include <mutex>
#include <string>
#include <unordered_map>
#include <cstdint>

class HackerDevice;
struct TextureOverride;
class ExplicitCommandListSection;

namespace PortApi {

// The one device/context pair their engine runs against, and whether a config is loaded (port_api.cpp).
extern HackerDevice         g_dev;
extern FrameAnalysisContext g_ctx;
extern bool                 g_loaded;
extern unsigned             g_generation; // bumped on every (re)load

// Their engine assumes the immediate context's single thread; every call in holds this.
std::recursive_mutex& EngineLock();

// Parse-time snapshots and indexes (port_parse_index.cpp). Rebuilt after every parse, read lock-free.
void                                                           RebuildParseSnapshots();
bool                                                           OverrideHashKnown(uint64_t h);
bool                                                           TextureOverrideHashKnown(uint32_t h); // true before the first parse: unknown, ask the engine
std::unordered_map<std::wstring, TextureOverride*>&            SectionIndex();
std::unordered_map<std::wstring, ExplicitCommandListSection*>& ExplicitIndex();
std::unordered_map<const void*, std::wstring>&                 ExplicitNameIndex();
std::unordered_map<const void*, std::wstring>&                 CustomResourceNameIndex();

// Each extra copy drawn this frame gets its mod's [CommandListUpdateMergedSkeleton] run on its own state (port_skel_copies.cpp).
void RunSkelCopyUpdates();

} // namespace PortApi
