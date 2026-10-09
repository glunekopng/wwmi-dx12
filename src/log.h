#pragma once
#include <windows.h>
#include <cstdio>
#include <ctime>
#include <locale.h>

namespace logger {

// Open log at an explicit path.
void init(const wchar_t* log_path);

extern int         g_level;    // 0 = off, and 0 is the default
extern const char* g_build_id; // set once at init; emitted whenever the log first opens up
void               set_level(int lvl);
inline bool        enabled(int lvl)
{
    return g_level >= lvl;
}
inline void set_build_id(const char* id)
{
    g_build_id = id;
}

_locale_t utf8_locale();

void write(_Printf_format_string_ const char* fmt, ...);  // level 1: errors / warnings / must-know events
void info(_Printf_format_string_ const char* fmt, ...);   // level 2
void trace(_Printf_format_string_ const char* fmt, ...);  // level 3
void report(_Printf_format_string_ const char* fmt, ...); // level 1, never rate-limited: a removal / stall / crash report, a line that paces itself

void set_unlimited(bool on);
bool unlimited();

} // namespace logger
