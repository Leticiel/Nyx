#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace csystem
{
using Bytes = std::vector<std::uint8_t>;

struct Error : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

struct Operand
{
    char type = 'N';
    std::int32_t value = 0;
    std::uint32_t text_id = 0;
    std::u16string text;
};

struct Record
{
    char tag = 'M';
    char marker = '#';
    std::int32_t opcode = 0;
    std::vector<Operand> operands;
    std::uint32_t text_id = 0;
    std::uint32_t trailer = 9999;
    std::u16string text;
    Bytes raw;
    bool typed = true;
};

struct Entry
{
    std::uint32_t id = 0;
    std::uint32_t version = 23;
    std::uint32_t size = 0;
    std::uint32_t packed_size = 0;
    std::uint32_t offset = 0;
    char kind = 0;
    char subtype = 0;
    std::int32_t reserved = -1;
    std::uint8_t part = 0;
    Bytes payload;
    Bytes stored;
    // Inferred from audio content, not serialized in an Arc index.
    std::int32_t audio_prefix_size = -1;
    Bytes leading;
};

struct JavaScript
{
    std::uint32_t id = 0;
    std::u16string name;
    std::u16string source;
    std::vector<std::uint32_t> dependencies;
};

struct MetadataValue
{
    std::uint32_t type = 0;
    std::uint32_t id = 0;
    double number = 0;
    std::u16string text;
};

struct Property
{
    std::u16string name;
    std::vector<MetadataValue> values;
};

struct Metadata
{
    std::u16string name;
    std::vector<Property> properties;
};

struct SoundGroup
{
    std::u16string name;
    std::vector<std::uint32_t> ids;
};

Bytes read_file(const std::filesystem::path& path);
Bytes lzss_decode(std::span<const std::uint8_t> input, std::size_t expected);
std::vector<Record> decode_records(std::span<const std::uint8_t> input);
Bytes encode_records(const std::vector<Record>& records);
JavaScript decode_javascript(std::span<const std::uint8_t> input);
Bytes encode_javascript(const JavaScript& script);
Metadata decode_metadata(std::span<const std::uint8_t> input);
Bytes encode_metadata(const Metadata& metadata);
SoundGroup decode_sound_group(std::span<const std::uint8_t> input);
Bytes encode_sound_group(const SoundGroup& group);
}
