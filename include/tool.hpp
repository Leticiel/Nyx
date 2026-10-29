#pragma once
#include "core.hpp"
#include "json.hpp"
#include <fstream>
#include <functional>
#include <array>
#include <memory>
#include <optional>
#include <set>

namespace csystem
{
namespace fs = std::filesystem;
using Json = nlohmann::ordered_json;
inline constexpr const char* project_format = "csystem-project-13";
inline constexpr const char* config_format = "csystem-config-5";

struct Cursor
{
    std::span<const uint8_t> data;
    size_t pos = 0;
    std::span<const uint8_t> take(size_t n);
    uint8_t u8();
    uint16_t u16();
    uint32_t u32();
    uint64_t u64();
    uint32_t count(size_t minimum = 1);

    bool done() const
    {
        return pos == data.size();
    }

    void finish() const;
};

void put(Bytes&, uint64_t, size_t n = 4);
void patch(Bytes&, size_t, uint64_t, size_t n = 4);
void append(Bytes&, std::span<const uint8_t>);
void write_file(const fs::path&, std::span<const uint8_t>);
void write_text(const fs::path&, const std::string&);
void validate_output_directory(const fs::path& source, const fs::path& output);
void remove_output_directory(const fs::path& source, const fs::path& output);
std::string read_text(const fs::path&);
std::string utf8(const std::u16string&);
std::u16string utf16(const std::string&);
std::string hex(std::span<const uint8_t>);
Bytes unhex(const std::string&);
std::string sha256(std::span<const uint8_t>);
std::string file_hash(const fs::path&);
std::string padded(uint64_t, int width = 4);
uint32_t checked32(uint64_t);
uint32_t read_decimal(Cursor&);
void write_decimal(Bytes&, uint32_t);
Bytes lzss_encode(std::span<const uint8_t>);
Bytes index_decode(std::span<const uint8_t>);
Bytes index_encode(std::span<const uint8_t>);
enum class ArchiveIndexLayout
{
    SizePackedOffset,
    SizeOffsetPacked
};

struct ArchiveIndex
{
    ArchiveIndexLayout layout = ArchiveIndexLayout::SizePackedOffset;
    std::vector<Entry> entries;
};

struct ArchivePair
{
    std::string key;
    std::string index;
    std::string content;
};

std::vector<ArchivePair> game_archives(const fs::path& game);
void transform_archive_video(Bytes& bytes);
std::string archive_video_extension(std::span<const uint8_t> bytes);

std::string archive_index_layout_name(ArchiveIndexLayout);
ArchiveIndexLayout parse_archive_index_layout(const std::string&);
ArchiveIndex decode_arc_index(std::span<const uint8_t>, std::optional<uint64_t> content_size = {});
ArchiveIndex decode_arc_index(std::span<const uint8_t>,
                              const std::map<uint8_t, uint64_t>& content_sizes);
fs::path archive_part_path(const fs::path& content, uint8_t part);
ArchiveIndex read_arc_index(const fs::path& index, const fs::path& content);
Bytes encode_arc_index(const std::vector<Entry>&, ArchiveIndexLayout);
std::vector<Entry> arc_index(std::span<const uint8_t>);
Bytes entry_payload(const Entry&, const fs::path&);
size_t archive_entry_prefix_size(const Entry&);
bool opaque_archive_entry(const Entry&);
bool index_only_archive_entry(const Entry&);
std::string archive_entry_compression(const Entry&, std::span<const uint8_t> stored = {});
Bytes read_range(const fs::path&, uint64_t, size_t);
Json decode_config(std::span<const uint8_t>);
Bytes encode_config(const Json&);

struct ResourceEntry
{
    uint32_t id, size, packed;
    uint64_t offset;
    size_t index_position;
};

struct ResourcePart
{
    unsigned number;
    std::vector<ResourceEntry> entries;
};

struct Resources
{
    bool present = false;
    fs::path directory;
    Bytes index;
    std::vector<ResourcePart> parts;
    explicit Resources(const fs::path&);
    const ResourceEntry& find(uint32_t) const;
    Bytes payload(uint32_t) const;
};

using PayloadMap = std::map<uint32_t, Bytes>;
using ResourceResolver = std::function<int32_t(const std::string&, int32_t, const std::string&)>;
using ResourcePathLookup = std::function<std::string(const std::string&, int32_t, int32_t)>;
std::string decompile_script(const std::vector<Entry>&,
                             const ResourcePathLookup& paths = {},
                             const ResourceResolver& resolver = {});
PayloadMap compile_script(const std::string&, const ResourceResolver& resolver = {});
Json catalog();

struct Image
{
    uint32_t width = 0, height = 0;
    Bytes bgra;
};

Image decode_standard(std::span<const uint8_t>);
Bytes encode_png(const Image&);
Image decode_image(std::span<const uint8_t>, const std::function<Image(uint32_t)>&);
Image decode_image_alpha(std::span<const uint8_t>, const std::function<Image(uint32_t)>&);
Bytes wrap_png(std::span<const uint8_t>, uint8_t flag = 0);
Json inspect_game(const fs::path&);
Json export_project(const fs::path&, const fs::path&, bool media);
Json export_scenario_project(const fs::path&, const fs::path&);
Json build_scenario_project(const fs::path&, const fs::path&);
Json build_project(const fs::path&, const fs::path&);
Json verify_project(const fs::path&, const fs::path&);
}
