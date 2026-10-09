#include "CommandList.h"
#include "port_keygate.h"

bool PortParseRunExplicitCommandList(const wchar_t* section, const wchar_t* key, wstring* val,
    CommandList* explicit_command_list, CommandList* pre_command_list, CommandList* post_command_list,
    const wstring* ini_namespace);

#define ParseRunExplicitCommandList PortParseRunExplicitCommandList

#include "Override.cpp"

#undef ParseRunExplicitCommandList

bool PortParseRunExplicitCommandList(const wchar_t* section, const wchar_t* key, wstring* val,
    CommandList* explicit_command_list, CommandList* pre_command_list, CommandList* post_command_list,
    const wstring* ini_namespace)
{
    const bool ok = ParseRunExplicitCommandList(section, key, val, explicit_command_list,
        pre_command_list, post_command_list, ini_namespace);
    MigotoPort::WrapObservedList(explicit_command_list);
    MigotoPort::WrapObservedList(pre_command_list);
    MigotoPort::WrapObservedList(post_command_list);
    return ok;
}
