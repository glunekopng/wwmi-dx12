#pragma once
#include <cstdint>

bool TexCrawlOn();
void TexCrawlSet(bool on);
void TexCrawlNote(uint64_t place, uint32_t hash); // a texture a game character draw binds at that mesh place
void TexCrawlFlushIfQuiet(unsigned quiet_ms);     // saved once no new texture was seen for quiet_ms, or a minute on
