#pragma once

#include "tool.hpp"

namespace csystem
{
struct LineBreakingField
{
    size_t string_index;
    const char* name;
    const char* prefix;
};

inline constexpr LineBreakingField line_breaking_fields[] = {
    {1, "line_start_prohibited_characters", "AH:"},
    {2, "line_end_prohibited_characters", "AL:"},
};

struct LineBreakingValue
{
    std::string characters;
    std::string suffix;
};

std::optional<LineBreakingValue> decode_line_breaking_value(const Json& string,
                                                            const LineBreakingField& field);
Json export_script_documents(const fs::path& project, const std::vector<Entry>& entries);
std::string indent_javascript(const std::string& source);
PayloadMap compile_script_documents(const fs::path& project, const Json& manifest);
Json readable_config(const Json& decoded, const ResourcePathLookup& paths = {});
Json merge_config(const Json& readable,
                  const Json& original,
                  const ResourceResolver& resolver = {});
}
