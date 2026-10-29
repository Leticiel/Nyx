#include "tool.hpp"
#include <array>
#include <bit>
#include <limits>

namespace csystem
{
namespace
{
std::u16string read_string(Cursor& reader, bool encrypted = false)
{
    const auto length = reader.u32();
    if (length > (reader.data.size() - reader.pos) / 2 || (encrypted && length > 65535))
    {
        throw Error("invalid UTF-16 string length");
    }
    std::u16string result;
    result.reserve(length);
    const auto mask = encrypted ? length : 0;
    for (uint32_t index = 0; index < length; ++index)
    {
        result.push_back(char16_t(reader.u16() ^ mask));
    }
    return result;
}

void write_string(Bytes& output, const std::u16string& text, bool encrypted = false)
{
    if (encrypted && text.size() > 65535)
    {
        throw Error("script string exceeds 65535 UTF-16 units");
    }
    const auto length = checked32(text.size());
    put(output, length);
    const auto mask = encrypted ? length : 0;
    for (const auto character : text)
    {
        put(output, uint16_t(character) ^ mask, 2);
    }
}

Operand read_operand(Cursor& reader)
{
    Operand operand;
    operand.type = char(reader.u8());
    switch (operand.type)
    {
    case 'N':
    case 'A':
        operand.value = std::bit_cast<int32_t>(reader.u32());
        break;
    case 'T':
        operand.text_id = reader.u32();
        operand.text = read_string(reader, true);
        break;
    default:
        throw Error("unknown script operand tag");
    }
    return operand;
}

void write_operand(Bytes& output, const Operand& operand)
{
    put(output, operand.type, 1);
    switch (operand.type)
    {
    case 'N':
    case 'A':
        put(output, std::bit_cast<uint32_t>(operand.value));
        break;
    case 'T':
        put(output, operand.text_id);
        write_string(output, operand.text, true);
        break;
    default:
        throw Error("unknown script operand tag");
    }
}
}

Bytes read_file(const std::filesystem::path& path)
{
    return read_range(path, 0, std::filesystem::file_size(path));
}

Bytes lzss_decode(std::span<const uint8_t> input, size_t expected)
{
    if (expected > 256 * 1024 * 1024)
    {
        throw Error("decoded entry exceeds 256 MiB limit");
    }
    Cursor reader{input};
    std::array<uint8_t, 4096> dictionary{};
    size_t write_position = 4078;
    Bytes output;
    output.reserve(expected);
    auto emit = [&](uint8_t value)
    {
        output.push_back(value);
        dictionary[write_position] = value;
        write_position = (write_position + 1) & 4095;
    };
    while (output.size() < expected)
    {
        const auto flags = reader.u8();
        for (unsigned bit = 0; bit < 8 && output.size() < expected; ++bit)
        {
            if (flags & (1u << bit))
            {
                emit(reader.u8());
                continue;
            }
            const auto low = reader.u8();
            const auto high = reader.u8();
            const size_t position = low | ((high & 240) << 4);
            const size_t length = (high & 15) + 3;
            if (length > expected - output.size())
            {
                throw Error("LZSS copy exceeds declared length");
            }
            for (size_t index = 0; index < length; ++index)
            {
                emit(dictionary[(position + index) & 4095]);
            }
        }
    }
    reader.finish();
    return output;
}

std::vector<Record> decode_records(std::span<const uint8_t> input)
{
    Cursor stream{input};
    std::vector<Record> records;
    while (!stream.done())
    {
        const auto size = stream.u32();
        Cursor reader{stream.take(size)};
        Record record;
        record.tag = char(reader.u8());
        if (record.tag == 'T')
        {
            record.text_id = reader.u32();
            record.text = read_string(reader, true);
            record.trailer = reader.u32();
        }
        else if (record.tag == 'M' && size >= 11 && reader.data[2] == 'N' &&
                 (reader.data[1] == '#' || reader.data[1] == '$' || reader.data[1] == 'L'))
        {
            record.marker = char(reader.u8());
            reader.u8();
            record.opcode = std::bit_cast<int32_t>(reader.u32());
            Cursor arguments{reader.take(size - 11)};
            while (!arguments.done())
            {
                record.operands.push_back(read_operand(arguments));
            }
            record.trailer = reader.u32();
        }
        else
        {
            record.typed = false;
            const auto data = reader.take(size - 1);
            record.raw.assign(data.begin(), data.end());
        }
        reader.finish();
        records.push_back(std::move(record));
    }
    return records;
}

Bytes encode_records(const std::vector<Record>& records)
{
    Bytes output;
    for (const auto& record : records)
    {
        Bytes body;
        put(body, record.tag, 1);
        if (!record.typed)
        {
            append(body, record.raw);
        }
        else if (record.tag == 'T')
        {
            put(body, record.text_id);
            write_string(body, record.text, true);
            put(body, record.trailer);
        }
        else if (record.tag == 'M')
        {
            put(body, record.marker, 1);
            put(body, 'N', 1);
            put(body, std::bit_cast<uint32_t>(record.opcode));
            for (const auto& operand : record.operands)
            {
                write_operand(body, operand);
            }
            put(body, record.trailer);
        }
        else
        {
            throw Error("unknown typed record tag");
        }
        put(output, checked32(body.size()));
        append(output, body);
    }
    return output;
}

JavaScript decode_javascript(std::span<const uint8_t> input)
{
    Cursor reader{input};
    JavaScript script;
    script.id = reader.u32();
    script.name = read_string(reader);
    script.source = read_string(reader);
    const auto count = reader.count(4);
    for (uint32_t index = 0; index < count; ++index)
    {
        script.dependencies.push_back(reader.u32());
    }
    reader.finish();
    return script;
}

Bytes encode_javascript(const JavaScript& script)
{
    Bytes output;
    put(output, script.id);
    write_string(output, script.name);
    write_string(output, script.source);
    put(output, checked32(script.dependencies.size()));
    for (const auto dependency : script.dependencies)
    {
        put(output, dependency);
    }
    return output;
}

Metadata decode_metadata(std::span<const uint8_t> input)
{
    Cursor reader{input};
    Metadata metadata;
    metadata.name = read_string(reader);
    const auto property_count = reader.count(8);
    for (uint32_t index = 0; index < property_count; ++index)
    {
        Property property;
        property.name = read_string(reader);
        const auto value_count = reader.count(8);
        for (uint32_t value_index = 0; value_index < value_count; ++value_index)
        {
            MetadataValue value;
            value.type = reader.u32();
            switch (value.type)
            {
            case 1:
                value.text = read_string(reader);
                break;
            case 2:
                value.number = std::bit_cast<double>(reader.u64());
                break;
            case 3:
                value.id = reader.u32();
                break;
            default:
                throw Error("unknown UI property type");
            }
            property.values.push_back(value);
        }
        metadata.properties.push_back(property);
    }
    reader.finish();
    return metadata;
}

Bytes encode_metadata(const Metadata& metadata)
{
    Bytes output;
    write_string(output, metadata.name);
    put(output, checked32(metadata.properties.size()));
    for (const auto& property : metadata.properties)
    {
        write_string(output, property.name);
        put(output, checked32(property.values.size()));
        for (const auto& value : property.values)
        {
            put(output, value.type);
            switch (value.type)
            {
            case 1:
                write_string(output, value.text);
                break;
            case 2:
                put(output, std::bit_cast<uint64_t>(value.number), 8);
                break;
            case 3:
                put(output, value.id);
                break;
            default:
                throw Error("unknown UI property type");
            }
        }
    }
    return output;
}

SoundGroup decode_sound_group(std::span<const uint8_t> input)
{
    Cursor reader{input};
    SoundGroup group;
    group.name = read_string(reader);
    const auto count = reader.count(4);
    for (uint32_t index = 0; index < count; ++index)
    {
        group.ids.push_back(reader.u32());
    }
    reader.finish();
    return group;
}

Bytes encode_sound_group(const SoundGroup& group)
{
    Bytes output;
    write_string(output, group.name);
    put(output, checked32(group.ids.size()));
    for (const auto sound : group.ids)
    {
        put(output, sound);
    }
    return output;
}
}
