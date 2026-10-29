#pragma once
#include "tool.hpp"

namespace csystem
{
struct PageParameterLayout
{
    std::string format;
    size_t option_count;
};
// Profiles describe verified native consumers, independently of page wire layouts.
PageParameterLayout page_parameter_layout(const std::string& profile);
Json detect_page_parameter_profile(const fs::path& game);
std::string page_parameter_profile_for_hash(const std::string& sha256);
std::string page_parameter_image_fingerprint(std::span<const uint8_t> image);
Json name_page_parameters(const Json& document, const std::string& profile);
Json neutral_page_parameters(const Json& document);
}
