#pragma once
#include "legacy.hpp"

namespace csystem
{
// Imported layouts remain experimental until real game data and the engine have
// both been checked. Synthetic round trips alone must not promote this status.
const Json& experimental_image_profiles();
struct ImageProfileContext
{
    std::string id;
    std::map<uint32_t, uint32_t> baselines;
};
ImageProfileContext select_image_profile(const std::vector<Entry>&, const fs::path& content);
bool known_image_layout(std::span<const uint8_t>);
std::optional<Json> profile_image_metadata(std::span<const uint8_t>, const std::string& profile = {});
std::optional<Json> bgra_profile_metadata(std::span<const uint8_t>);
Bytes canonical_profile_image(std::span<const uint8_t>, const Json& metadata,
                              std::optional<uint32_t> baseline = {});
Bytes encode_profile_image(const Image&, Json metadata, const Image* channel_alpha = nullptr);
bool is_tb1(std::span<const uint8_t>);
Image decode_tb1(std::span<const uint8_t>);
Bytes encode_tb1(const Image&, std::span<const uint8_t> header);
Bytes encode_fixed_bgra(const Image&, uint32_t base, uint32_t parameter);
}
