#pragma once
#include "tool.hpp"

namespace csystem
{
enum class DataFormat
{
    Utf16,
    Cp932
};

std::string data_format_name(DataFormat);
DataFormat parse_data_format(const std::string&);
std::u16string read_sjis_tunnel_table(const fs::path&);
std::u16string project_sjis_tunnel_table(const Json&);
std::string decode_cp932(std::span<const uint8_t>, std::u16string_view tunnel = {});
Bytes encode_cp932(const std::string&, std::u16string_view tunnel = {});
std::string decode_legacy_text(std::span<const uint8_t>, std::u16string_view tunnel = {});
Bytes encode_legacy_text(const std::string&, std::u16string_view tunnel = {});
std::string decode_utf16_text(std::u16string_view);
std::u16string encode_utf16_text(const std::string&);
Bytes canonical_scenario(std::span<const uint8_t>,
                         uint32_t id,
                         DataFormat,
                         std::u16string_view tunnel = {},
                         uint32_t archive_version = 23);
Bytes native_scenario(std::span<const uint8_t>,
                      DataFormat,
                      std::u16string_view tunnel = {},
                      uint32_t archive_version = 23);
Json decode_compact_settings(std::span<const uint8_t>, DataFormat);
Bytes encode_compact_settings(const Json&, DataFormat, bool terminated = true);
Json decode_legacy_config(std::span<const uint8_t> raw, std::span<const uint8_t> trailing);
Bytes encode_legacy_config(const Json&);
std::optional<std::array<uint32_t, 20>> compact_image_attributes(std::span<const uint8_t>);
std::optional<std::array<uint32_t, 20>> legacy_image_attributes(std::span<const uint8_t>);
std::optional<std::array<uint32_t, 20>> masked_image_attributes(std::span<const uint8_t>);
std::optional<std::array<uint32_t, 20>> harami_image_attributes(std::span<const uint8_t>);
std::optional<std::array<uint32_t, 20>> oshioki_image_attributes(std::span<const uint8_t>);
std::optional<std::array<uint32_t, 20>> zettai_image_attributes(std::span<const uint8_t>);
std::optional<std::array<uint32_t, 20>> fuka_image_attributes(std::span<const uint8_t>);
std::optional<std::array<uint32_t, 8>> legacy22_image_attributes(std::span<const uint8_t>);
std::optional<std::array<uint32_t, 8>> legacy8_image_attributes(std::span<const uint8_t>);
std::optional<std::array<uint32_t, 8>> ani8_image_attributes(std::span<const uint8_t>);
std::optional<std::array<uint32_t, 8>> natsu8_image_attributes(std::span<const uint8_t>);
std::optional<std::array<uint32_t, 8>> gyk8_image_attributes(std::span<const uint8_t>);
Bytes gyk8_image(const Image&, std::array<uint32_t, 8>, const Image* channel_alpha = nullptr,
                 bool top_down = true);
Bytes natsu8_image(const Image&, std::array<uint32_t, 8>, const Image* channel_alpha = nullptr,
                   bool top_down = true);
Bytes ani8_image(const Image&, std::array<uint32_t, 8>, const Image* channel_alpha = nullptr,
                 bool top_down = true);
std::optional<std::array<uint32_t, 8>> shuffled22_image_attributes(std::span<const uint8_t>);
std::optional<std::array<uint32_t, 8>> iji22_image_attributes(std::span<const uint8_t>);
std::optional<std::array<uint32_t, 8>> lesson22_image_attributes(std::span<const uint8_t>);
Bytes shuffled22_image(const Image&, std::array<uint32_t, 8>, const Image* channel_alpha = nullptr);
Bytes iji22_image(const Image&, std::array<uint32_t, 8>, const Image* channel_alpha = nullptr,
                  bool top_down = true);
Bytes lesson22_image(const Image&, std::array<uint32_t, 8>, const Image* channel_alpha = nullptr,
                     bool top_down = true);
Bytes legacy22_image(const Image&, std::array<uint32_t, 8>, const Image* channel_alpha = nullptr,
                     bool top_down = true);
Bytes legacy8_image(const Image&, std::array<uint32_t, 8>, const Image* channel_alpha = nullptr,
                    bool top_down = true);
Bytes masked_image(const Image&, std::array<uint32_t, 20>, const Image* channel_alpha = nullptr,
                   bool top_down = true);
Bytes harami_image(const Image&, std::array<uint32_t, 20>, const Image* channel_alpha = nullptr,
                   bool top_down = true);
Bytes oshioki_image(const Image&, std::array<uint32_t, 20>, const Image* channel_alpha = nullptr,
                    bool top_down = true);
Bytes zettai_image(const Image&, std::array<uint32_t, 20>, const Image* channel_alpha = nullptr,
                   bool top_down = true);
Bytes fuka_image(const Image&, std::array<uint32_t, 20>, const Image* channel_alpha = nullptr,
                 bool top_down = true);
std::optional<Image> legacy22_channel_alpha(std::span<const uint8_t>);
struct ImageProfileContext;
Bytes canonical_image(std::span<const uint8_t>,
                      DataFormat,
                      std::span<const uint32_t> bitmap_ids = {},
                      const ImageProfileContext* profile = nullptr, uint32_t entry_id = 0);
Bytes compact_grayscale(const Image&, std::array<uint32_t, 20> attributes, DataFormat);
Bytes compact_bitmap(const Image&, std::array<uint32_t, 20>, DataFormat,
                     const Image* channel_alpha = nullptr);
Bytes legacy_bitmap(const Image&, std::array<uint32_t, 20>,
                    const Image* channel_alpha = nullptr);
std::optional<Image> compact_channel_alpha(std::span<const uint8_t>, DataFormat);
Bytes legacy_grayscale(const Image&);
}
