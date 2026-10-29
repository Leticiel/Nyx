#pragma once
#include "tool.hpp"

namespace csystem
{
Json decode_graphics_group(std::span<const uint8_t>, uint32_t archive_version = 23, unsigned tables = 0);
unsigned graphics_group_tables(const std::vector<Entry>&);
Bytes encode_graphics_group(const Json&, const ResourceResolver& = {});
Json readable_graphics_group(const Json&, const ResourcePathLookup&);
Json decode_graphics_timeline(std::span<const uint8_t>, uint32_t archive_version = 23);
Bytes encode_graphics_timeline(const Json&, const ResourceResolver& = {});
Json readable_graphics_timeline(const Json&, const ResourcePathLookup&);
Json decode_motion_preset(std::span<const uint8_t>);
Bytes encode_motion_preset(const Json&);
Json decode_native_page(std::span<const uint8_t>, uint32_t archive_version = 23,
                        const std::string& parameter_profile = {});
Bytes encode_native_page(const Json&, const ResourceResolver& = {});
Json readable_native_page(const Json&, const ResourcePathLookup&);
}
