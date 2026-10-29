#include "legacy.hpp"
#include "image_profiles.hpp"
#include <windows.h>
#include <algorithm>
#include <bit>
#include <limits>

namespace csystem
{
namespace
{
void reverse_bitmap_rows(std::span<uint8_t> bytes, uint32_t height)
{
    if (!height || bytes.size() % height) throw Error("invalid bitmap plane row layout");
    const auto stride = bytes.size() / height;
    for (uint32_t y = 0; y < height / 2; ++y)
    {
        auto first = bytes.begin() + size_t(y) * stride;
        auto last = bytes.begin() + size_t(height - 1 - y) * stride;
        std::swap_ranges(first, first + stride, last);
    }
}

Image vertically_flipped(Image image)
{
    if (!image.width || !image.height || image.bgra.size() != uint64_t(image.width) * image.height * 4)
        throw Error("invalid bitmap dimensions");
    reverse_bitmap_rows(image.bgra, image.height);
    return image;
}

Bytes top_down_bitmap(Bytes canonical)
{
    // These legacy layouts store their rows for a positive-height GDI DIB.
    // Canonical PNG-facing buffers instead start at the top-left pixel.
    Cursor reader{canonical};
    reader.take(13);
    const auto height = reader.u32();
    reader.u32();
    const auto alpha_size = reader.u32();
    const auto alpha_offset = reader.pos;
    reader.take(alpha_size);
    if (reader.u32()) throw Error("unexpected delta mask in a legacy full bitmap");
    const auto color_size = reader.u32();
    const auto color_offset = reader.pos;
    reader.take(color_size);
    reader.finish();
    if (alpha_size) reverse_bitmap_rows(std::span(canonical).subspan(alpha_offset, alpha_size), height);
    reverse_bitmap_rows(std::span(canonical).subspan(color_offset, color_size), height);
    return canonical;
}

void validate_sjis_tunnel_table(std::u16string_view table)
{
    if (table.size() > 59 * 58)
    {
        throw Error("SJIS tunnel table exceeds 3422 characters");
    }
    for (const auto character : table)
    {
        if (!character || (character >= 0xd800 && character <= 0xdfff))
        {
            throw Error("SJIS tunnel table contains a null or surrogate character");
        }
    }
}

std::optional<size_t> sjis_tunnel_index(uint8_t lead, uint8_t trail)
{
    if (!((lead >= 0x81 && lead <= 0x9f) || (lead >= 0xe0 && lead <= 0xfc)) || trail < 1 ||
        trail > 0x3f)
    {
        return {};
    }
    for (const auto excluded : {9, 10, 13, 32, 44})
    {
        if (trail == excluded)
        {
            return {};
        }
    }
    const auto row = lead <= 0x9f ? lead - 0x81 : 31 + lead - 0xe0;
    for (const auto excluded : {44, 32, 13, 10, 9})
    {
        if (trail > excluded)
        {
            --trail;
        }
    }
    return size_t(row) * 58 + trail - 1;
}

void append_sjis_tunnel_pair(Bytes& bytes, size_t index)
{
    const auto row = index / 58;
    auto trail = index % 58 + 1;
    for (const auto excluded : {9, 10, 13, 32, 44})
    {
        if (trail >= size_t(excluded))
        {
            ++trail;
        }
    }
    bytes.push_back(uint8_t(row < 31 ? 0x81 + row : 0xe0 + row - 31));
    bytes.push_back(uint8_t(trail));
}

std::string escape_ruby(const std::string& text, bool field)
{
    std::string result;
    for (const char character : text)
    {
        if (character == '\\' || character == '{' ||
            (field && (character == '|' || character == '}')))
        {
            result += '\\';
        }
        result += character;
    }
    return result;
}

std::string ruby_field(const std::string& text, size_t& position, char delimiter)
{
    std::string result;
    while (position < text.size())
    {
        const auto character = text[position++];
        if (character == delimiter)
        {
            return result;
        }
        if (character == '\\')
        {
            if (position == text.size())
            {
                throw Error("incomplete ruby escape");
            }
            result += text[position++];
        }
        else
        {
            result += character;
        }
    }
    throw Error("unterminated ruby field");
}

uint32_t read_attribute(Cursor& reader)
{
    const auto first = reader.u8();
    const uint64_t low = first == 251 ? 0 : first;
    uint64_t high = 0;
    uint64_t middle = 0;
    for (;;)
    {
        const auto digit = reader.u8();
        if (digit == 239)
        {
            return checked32((high * 233 + middle) * 233 + low);
        }
        if (digit == 233)
        {
            if (++high > UINT32_MAX / (233 * 233))
            {
                throw Error("legacy image attribute overflow");
            }
        }
        else
        {
            middle = digit == 251 ? 0 : digit;
        }
    }
}

void write_attribute(Bytes& bytes, uint32_t value)
{
    const auto low = value % 233;
    const auto middle = (value / 233) % 233;
    bytes.push_back(uint8_t(low ? low : 251));
    bytes.insert(bytes.end(), value / (233 * 233), uint8_t{233});
    bytes.push_back(uint8_t(middle ? middle : 251));
    bytes.push_back(239);
}

uint32_t read_attribute22(Cursor& reader)
{
    const auto first = reader.u8();
    const uint64_t low = first == 250 ? 0 : first;
    if (first >= 239 && first != 250)
    {
        throw Error("invalid version-22 image attribute");
    }
    uint64_t high = 0;
    uint64_t middle = 0;
    for (;;)
    {
        const auto digit = reader.u8();
        if (digit == 251)
        {
            return checked32((high * 239 + middle) * 239 + low);
        }
        if (digit == 239)
        {
            if (++high > UINT32_MAX / (239 * 239))
            {
                throw Error("version-22 image attribute overflow");
            }
        }
        else if (digit < 239 || digit == 250)
        {
            middle = digit == 250 ? 0 : digit;
        }
        else
        {
            throw Error("invalid version-22 image attribute digit");
        }
    }
}

void write_attribute22(Bytes& bytes, uint32_t value)
{
    const auto low = value % 239;
    const auto middle = value / 239 % 239;
    bytes.push_back(uint8_t(low ? low : 250));
    bytes.insert(bytes.end(), value / (239 * 239), uint8_t{239});
    bytes.push_back(uint8_t(middle ? middle : 250));
    bytes.push_back(251);
}

std::string decode_legacy_plain(std::span<const uint8_t> bytes, std::u16string_view tunnel)
{
    try
    {
        const auto decoded = decode_cp932(bytes, tunnel);
        if (encode_cp932(decoded, tunnel) == Bytes(bytes.begin(), bytes.end()))
        {
            return escape_ruby(decoded, false);
        }
    }
    catch (const Error&)
    {
    }
    std::string result;
    for (size_t position = 0; position < bytes.size();)
    {
        const size_t count =
            IsDBCSLeadByteEx(932, bytes[position]) && position + 1 < bytes.size() ? 2 : 1;
        const auto character = bytes.subspan(position, count);
        bool decoded = false;
        try
        {
            const auto text = decode_cp932(character, tunnel);
            if (encode_cp932(text, tunnel) == Bytes(character.begin(), character.end()))
            {
                result += escape_ruby(text, false);
                decoded = true;
            }
        }
        catch (const Error&)
        {
        }
        if (!decoded)
        {
            result += "{bytes:" + hex(character) + "}";
        }
        position += count;
    }
    return result;
}

}

Json decode_compact_settings(std::span<const uint8_t> bytes, DataFormat format)
{
    Cursor reader{bytes};
    const auto read_unit = [&]() -> uint16_t
    {
        return format == DataFormat::Utf16 ? reader.u16() : reader.u8();
    };
    if (read_unit() != '#')
    {
        throw Error("compact config settings marker is missing");
    }
    Json settings = Json::array();
    for (;;)
    {
        if (format == DataFormat::Cp932 && reader.done())
        {
            return settings;
        }
        auto character = read_unit();
        const auto terminator = format == DataFormat::Utf16 ? 10 : 254;
        if (character == terminator)
        {
            if (read_unit() != 10)
            {
                throw Error("invalid compact settings terminator");
            }
            reader.finish();
            return settings;
        }
        std::string key;
        while (true)
        {
            if (character < 32 || character > 126 || key.size() >= 16)
            {
                throw Error("invalid compact config key");
            }
            key += char(character);
            if (character == ':')
            {
                break;
            }
            character = read_unit();
        }
        int64_t value = -1;
        while (true)
        {
            const auto token = read_unit();
            if (token == 201)
            {
                break;
            }
            if (token == 250)
            {
                value = 0;
            }
            else if (token <= 200)
            {
                value = std::max<int64_t>(value, 0) + token;
            }
            else
            {
                throw Error("unknown compact config value token");
            }
            if (value > INT32_MAX)
            {
                throw Error("compact config value overflow");
            }
        }
        settings.push_back({{"key", key}, {"value", value}});
    }
}

Bytes encode_compact_settings(const Json& settings, DataFormat format, bool terminated)
{
    if (!terminated && format != DataFormat::Cp932)
    {
        throw Error("unterminated compact settings are only supported for CP932");
    }
    Bytes result;
    const auto write_unit = [&](uint16_t value)
    {
        put(result, value, format == DataFormat::Utf16 ? 2 : 1);
    };
    write_unit('#');
    for (const auto& setting : settings)
    {
        const auto key = setting.at("key").get<std::string>();
        if (key.empty() || key.size() > 16 || key.back() != ':' ||
            key.find(':') != key.size() - 1 ||
            key.find_first_not_of(
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789:") !=
                std::string::npos)
        {
            throw Error("invalid compact config key");
        }
        for (const auto character : key)
        {
            write_unit(uint8_t(character));
        }
        const auto value = setting.at("value").get<int64_t>();
        if (value < -1 || value > INT32_MAX)
        {
            throw Error("compact config values must be -1 or nonnegative signed integers");
        }
        if (value == 0)
        {
            write_unit(250);
        }
        else if (value > 0)
        {
            if (value > 10000000)
            {
                throw Error("compact config value exceeds the serialization limit");
            }
            for (int64_t count = value / 200; count > 0; --count)
            {
                write_unit(200);
            }
            if (value % 200)
            {
                write_unit(uint16_t(value % 200));
            }
        }
        write_unit(201);
    }
    if (terminated)
    {
        write_unit(format == DataFormat::Utf16 ? 10 : 254);
        write_unit(10);
    }
    return result;
}

std::string data_format_name(DataFormat format)
{
    return format == DataFormat::Utf16 ? "utf16" : "cp932";
}

DataFormat parse_data_format(const std::string& name)
{
    if (name == "utf16")
    {
        return DataFormat::Utf16;
    }
    if (name == "cp932")
    {
        return DataFormat::Cp932;
    }
    throw Error("unsupported native data format: " + name);
}

std::u16string read_sjis_tunnel_table(const fs::path& path)
{
    if (!fs::exists(path))
    {
        return {};
    }
    if (fs::file_size(path) % 2)
    {
        throw Error("invalid sjis_ext.bin: expected UTF-16LE character pairs");
    }
    if (fs::file_size(path) > 3422 * 2)
    {
        throw Error("invalid sjis_ext.bin: SJIS tunnel table exceeds 3422 characters");
    }
    const auto bytes = read_file(path);
    Cursor reader{bytes};
    std::u16string table;
    while (!reader.done())
    {
        table += char16_t(reader.u16());
    }
    validate_sjis_tunnel_table(table);
    return table;
}

std::u16string project_sjis_tunnel_table(const Json& manifest)
{
    if (!manifest.contains("sjis_tunnel"))
    {
        return {};
    }
    auto table = utf16(manifest.at("sjis_tunnel").at("characters").get<std::string>());
    validate_sjis_tunnel_table(table);
    return table;
}

std::string decode_cp932(std::span<const uint8_t> bytes, std::u16string_view tunnel)
{
    if (!tunnel.empty())
    {
        std::string result;
        size_t start = 0;
        for (size_t position = 0; position < bytes.size();)
        {
            if (IsDBCSLeadByteEx(932, bytes[position]) && position + 1 < bytes.size())
            {
                const auto index = sjis_tunnel_index(bytes[position], bytes[position + 1]);
                if (index && *index < tunnel.size())
                {
                    result += decode_cp932(bytes.subspan(start, position - start));
                    result += utf8(std::u16string(1, tunnel[*index]));
                    start = position + 2;
                }
                position += 2;
            }
            else
            {
                ++position;
            }
        }
        return result + decode_cp932(bytes.subspan(start));
    }
    if (bytes.empty())
    {
        return {};
    }
    const auto count = MultiByteToWideChar(932,
                                           MB_ERR_INVALID_CHARS,
                                           reinterpret_cast<const char*>(bytes.data()),
                                           int(checked32(bytes.size())),
                                           nullptr,
                                           0);
    if (!count)
    {
        throw Error("invalid CP932 text");
    }
    std::u16string text(size_t(count), 0);
    if (!MultiByteToWideChar(932,
                             MB_ERR_INVALID_CHARS,
                             reinterpret_cast<const char*>(bytes.data()),
                             int(bytes.size()),
                             reinterpret_cast<wchar_t*>(text.data()),
                             count))
    {
        throw Error("CP932 decoding failed");
    }
    return utf8(text);
}

Bytes encode_cp932(const std::string& source, std::u16string_view tunnel)
{
    const auto text = utf16(source);
    if (!tunnel.empty())
    {
        Bytes bytes;
        size_t start = 0;
        for (size_t position = 0; position < text.size(); ++position)
        {
            const auto index = tunnel.find(text[position]);
            if (index != std::u16string_view::npos)
            {
                append(bytes, encode_cp932(utf8(text.substr(start, position - start))));
                append_sjis_tunnel_pair(bytes, index);
                start = position + 1;
            }
        }
        append(bytes, encode_cp932(utf8(text.substr(start))));
        return bytes;
    }
    if (text.empty())
    {
        return {};
    }
    BOOL replaced = FALSE;
    const auto count = WideCharToMultiByte(932,
                                           WC_NO_BEST_FIT_CHARS,
                                           reinterpret_cast<const wchar_t*>(text.data()),
                                           int(checked32(text.size())),
                                           nullptr,
                                           0,
                                           nullptr,
                                           &replaced);
    if (!count || replaced)
    {
        throw Error("text contains a character that CP932 cannot represent");
    }
    Bytes bytes(size_t(count), 0);
    if (!WideCharToMultiByte(932,
                             WC_NO_BEST_FIT_CHARS,
                             reinterpret_cast<const wchar_t*>(text.data()),
                             int(text.size()),
                             reinterpret_cast<char*>(bytes.data()),
                             count,
                             nullptr,
                             &replaced) ||
        replaced)
    {
        throw Error("text contains a character that CP932 cannot represent");
    }
    return bytes;
}

std::string decode_legacy_text(std::span<const uint8_t> bytes, std::u16string_view tunnel)
{
    std::string text;
    Cursor reader{bytes};
    while (!reader.done())
    {
        const auto start = reader.pos;
        while (!reader.done() && reader.data[reader.pos] != 10 && reader.data[reader.pos] != 254 &&
               reader.data[reader.pos] != 255)
        {
            if (IsDBCSLeadByteEx(932, reader.u8()) && !reader.done())
            {
                reader.u8();
            }
        }
        text += decode_legacy_plain(bytes.subspan(start, reader.pos - start), tunnel);
        if (reader.done())
        {
            break;
        }
        const auto marker_position = reader.pos;
        const auto marker = reader.u8();
        if (marker == 10)
        {
            text += "{lf}";
            continue;
        }
        if (marker == 254)
        {
            text += '\n';
            continue;
        }
        if (reader.u8() != 255)
        {
            throw Error("unknown legacy inline text marker");
        }
        const auto reading_bytes = reader.take(size_t(reader.u8()) * 2);
        const auto base_bytes = reader.take(size_t(reader.u8()) * 2);
        try
        {
            const auto reading = decode_cp932(reading_bytes, tunnel);
            const auto base = decode_cp932(base_bytes, tunnel);
            if (encode_cp932(reading, tunnel) !=
                    Bytes(reading_bytes.begin(), reading_bytes.end()) ||
                encode_cp932(base, tunnel) != Bytes(base_bytes.begin(), base_bytes.end()))
            {
                throw Error("noncanonical CP932 ruby bytes");
            }
            text += "{ruby:" + escape_ruby(base, true) + "|" + escape_ruby(reading, true) + "}";
        }
        catch (const Error&)
        {
            text +=
                "{bytes:" + hex(bytes.subspan(marker_position, reader.pos - marker_position)) + "}";
        }
    }
    return text;
}

Bytes encode_legacy_text(const std::string& text, std::u16string_view tunnel)
{
    Bytes output;
    std::string plain;
    auto flush = [&]()
    {
        append(output, encode_cp932(plain, tunnel));
        plain.clear();
    };
    size_t position = 0;
    while (position < text.size())
    {
        if (text[position] == '\\')
        {
            if (++position == text.size())
            {
                throw Error("incomplete legacy text escape");
            }
            plain += text[position++];
        }
        else if (text.compare(position, 7, "{bytes:") == 0)
        {
            flush();
            const auto end = text.find('}', position + 7);
            if (end == std::string::npos || end == position + 7)
            {
                throw Error("incomplete raw text bytes");
            }
            append(output, unhex(text.substr(position + 7, end - position - 7)));
            position = end + 1;
        }
        else if (text.compare(position, 4, "{lf}") == 0)
        {
            flush();
            output.push_back(10);
            position += 4;
        }
        else if (text[position] == '\n')
        {
            flush();
            output.push_back(254);
            ++position;
        }
        else if (text.compare(position, 6, "{ruby:") == 0)
        {
            flush();
            position += 6;
            const auto base = encode_cp932(ruby_field(text, position, '|'), tunnel);
            const auto reading = encode_cp932(ruby_field(text, position, '}'), tunnel);
            for (const auto& field : {reading, base})
            {
                if (field.empty() || field.size() % 2 || field.size() > 510)
                {
                    throw Error("legacy ruby needs 1 to 255 double-byte characters per field");
                }
                for (size_t index = 0; index < field.size(); index += 2)
                {
                    if (!IsDBCSLeadByteEx(932, field[index]))
                    {
                        throw Error("legacy ruby supports only double-byte CP932 characters");
                    }
                }
            }
            append(output, Bytes{255, 255, uint8_t(reading.size() / 2)});
            append(output, reading);
            output.push_back(uint8_t(base.size() / 2));
            append(output, base);
        }
        else if (text[position] == '{')
        {
            throw Error(
                "unknown legacy text markup; escape a literal opening brace with a backslash");
        }
        else
        {
            plain += text[position++];
        }
    }
    flush();
    return output;
}

std::string decode_utf16_text(std::u16string_view text)
{
    std::string result;
    size_t position = 0;
    while (position < text.size())
    {
        const auto start = position;
        while (position < text.size() &&
               !(text[position] == 255 && position + 1 < text.size() && text[position + 1] == 255))
        {
            ++position;
        }
        result += escape_ruby(utf8(std::u16string(text.substr(start, position - start))), false);
        if (position == text.size())
        {
            break;
        }
        position += 2;
        auto field = [&](bool allow_empty)
        {
            if (position == text.size())
            {
                throw Error("truncated UTF-16 ruby field length");
            }
            const auto length = size_t(text[position++]);
            if ((!length && !allow_empty) || length > text.size() - position)
            {
                throw Error("invalid UTF-16 ruby field length");
            }
            const auto value = utf8(std::u16string(text.substr(position, length)));
            position += length;
            return escape_ruby(value, true);
        };
        const auto reading = field(false);
        const auto base = field(true);
        result += "{ruby:" + base + "|" + reading + "}";
    }
    return result;
}

std::u16string encode_utf16_text(const std::string& text)
{
    std::u16string output;
    std::string plain;
    auto flush = [&]()
    {
        output += utf16(plain);
        plain.clear();
    };
    size_t position = 0;
    while (position < text.size())
    {
        if (text[position] == '\\')
        {
            if (++position == text.size())
            {
                throw Error("incomplete UTF-16 text escape");
            }
            plain += text[position++];
        }
        else if (text.compare(position, 6, "{ruby:") == 0)
        {
            flush();
            position += 6;
            const auto base = utf16(ruby_field(text, position, '|'));
            const auto reading = utf16(ruby_field(text, position, '}'));
            output += u"\u00ff\u00ff";
            if (reading.empty()) throw Error("UTF-16 ruby reading must not be empty");
            for (const auto& field : {reading, base})
            {
                if (field.size() > 65535)
                {
                    throw Error("UTF-16 ruby fields cannot exceed 65535 code units");
                }
                output += char16_t(field.size());
                output += field;
            }
        }
        else if (text[position] == '{')
        {
            throw Error("unknown UTF-16 text markup; escape a literal opening brace with a backslash");
        }
        else
        {
            plain += text[position++];
        }
    }
    flush();
    return output;
}

Bytes canonical_scenario(std::span<const uint8_t> input,
                         uint32_t id,
                         DataFormat format,
                         std::u16string_view tunnel,
                         uint32_t archive_version)
{
    if (format == DataFormat::Utf16)
    {
        auto records = decode_records(input);
        for (auto& record : records)
        {
            if (record.typed && record.tag == 'T')
            {
                record.text = utf16(decode_utf16_text(record.text));
            }
            for (auto& operand : record.operands)
            {
                if (operand.type == 'T')
                {
                    operand.text = utf16(decode_utf16_text(operand.text));
                }
            }
        }
        return encode_records(records);
    }
    Cursor stream{input};
    std::vector<Record> records;
    while (!stream.done())
    {
        Cursor reader{stream.take(stream.u32())};
        Record record;
        record.tag = char(reader.u8());
        const auto text_id = checked32(uint64_t(id) * 100000 + records.size() + 1);
        auto read_text = [&](size_t suffix = 0)
        {
            const auto key = archive_version == 22 ? 0 : reader.u32();
            // The XOR key is not a reliable length field. Opcode 139 has a
            // fixed five-byte N block_id after its expression string.
            if (suffix > reader.data.size() - reader.pos)
                throw Error("truncated legacy expression block ID");
            const auto cipher = reader.take(reader.data.size() - reader.pos - suffix);
            Bytes plain(cipher.begin(), cipher.end());
            for (auto& byte : plain)
            {
                byte ^= uint8_t(key);
            }
            return utf16(decode_legacy_text(plain, tunnel));
        };
        if (record.tag == 'S')
        {
            record.tag = 'T';
            record.text_id = text_id;
            record.text = read_text();
        }
        else if (record.tag == 'M' && reader.data.size() >= 7 && reader.data[2] == 'N' &&
                 (reader.data[1] == '#' || reader.data[1] == '$' || reader.data[1] == 'L'))
        {
            record.marker = char(reader.u8());
            reader.u8();
            record.opcode = std::bit_cast<int32_t>(reader.u32());
            while (!reader.done())
            {
                Operand operand;
                operand.type = char(reader.u8());
                if (operand.type == 'N' || operand.type == 'A')
                {
                    operand.value = std::bit_cast<int32_t>(reader.u32());
                }
                else if (operand.type == 'S')
                {
                    operand.type = 'T';
                    operand.text_id = text_id;
                    const bool expression = record.marker == '#' && record.opcode == 139 &&
                                            record.operands.empty();
                    if (expression && (reader.data.size() - reader.pos < (archive_version == 22 ? 5u : 9u) ||
                                       reader.data[reader.data.size() - 5] != 'N'))
                        throw Error("legacy expression requires a literal block ID");
                    operand.text = read_text(expression ? 5 : 0);
                }
                else
                {
                    throw Error("unknown legacy script operand");
                }
                record.operands.push_back(std::move(operand));
            }
        }
        else
        {
            record.typed = false;
            const auto data = reader.take(reader.data.size() - reader.pos);
            record.raw.assign(data.begin(), data.end());
        }
        reader.finish();
        records.push_back(std::move(record));
    }
    return encode_records(records);
}

Bytes native_scenario(std::span<const uint8_t> input,
                      DataFormat format,
                      std::u16string_view tunnel,
                      uint32_t archive_version)
{
    if (format == DataFormat::Utf16)
    {
        auto records = decode_records(input);
        for (auto& record : records)
        {
            if (record.typed && record.tag == 'T')
            {
                record.text = encode_utf16_text(utf8(record.text));
            }
            for (auto& operand : record.operands)
            {
                if (operand.type == 'T')
                {
                    operand.text = encode_utf16_text(utf8(operand.text));
                }
            }
        }
        return encode_records(records);
    }
    Bytes output;
    for (const auto& record : decode_records(input))
    {
        if (record.typed && record.trailer != 9999)
        {
            throw Error("legacy records cannot store a modern record trailer");
        }
        Bytes body;
        auto write_text = [&](const std::u16string& text)
        {
            auto plain = encode_legacy_text(utf8(text), tunnel);
            const auto key = archive_version == 22 ? 0 : checked32(plain.size());
            if (archive_version != 22)
            {
                put(body, key);
            }
            for (auto& byte : plain)
            {
                byte ^= uint8_t(key);
            }
            append(body, plain);
        };
        if (!record.typed)
        {
            body.push_back(uint8_t(record.tag));
            append(body, record.raw);
        }
        else if (record.tag == 'T')
        {
            body.push_back('S');
            write_text(record.text);
        }
        else if (record.tag == 'M')
        {
            if (record.trailer != 9999)
            {
                throw Error("legacy commands cannot store a modern record trailer");
            }
            append(body, Bytes{'M', uint8_t(record.marker), 'N'});
            put(body, uint32_t(record.opcode));
            for (size_t index = 0; index < record.operands.size(); ++index)
            {
                const auto& operand = record.operands[index];
                if (operand.type == 'T')
                {
                    const bool expression = record.marker == '#' && record.opcode == 139 && index == 0 &&
                                            record.operands.size() == 2 && record.operands[1].type == 'N';
                    if (index + 1 != record.operands.size() && !expression)
                    {
                        throw Error("legacy string operand must be the final argument");
                    }
                    body.push_back('S');
                    write_text(operand.text);
                }
                else
                {
                    body.push_back(uint8_t(operand.type));
                    put(body, uint32_t(operand.value));
                }
            }
        }
        else
        {
            throw Error("unsupported legacy script record");
        }
        put(output, checked32(body.size()));
        append(output, body);
    }
    return output;
}

Json decode_legacy_config(std::span<const uint8_t> raw, std::span<const uint8_t> trailing)
{
    Cursor reader{raw};
    Json result = {{"format", "csystem-config-1"},
                   {"data_format", "cp932"},
                   {"strings", Json::array()},
                   {"blocks", Json::array({""})}};
    size_t index = 0;
    while (!reader.done())
    {
        const auto bytes = reader.take(reader.u32());
        if (index++ == 1)
        {
            result["settings"] = decode_compact_settings(bytes, DataFormat::Cp932);
            if (bytes.size() < 2 || bytes[bytes.size() - 2] != 254 || bytes.back() != 10)
            {
                result["compact_settings_terminated"] = false;
            }
            continue;
        }
        Json item = {{"hex", hex(bytes)}};
        auto text_bytes = bytes;
        if (bytes.size() >= 2 && bytes[bytes.size() - 2] == 254 && bytes.back() == 10)
        {
            text_bytes = bytes.first(bytes.size() - 2);
            item["native_suffix"] = "fe0a";
        }
        try
        {
            item["text"] = decode_cp932(text_bytes);
        }
        catch (const Error&)
        {
            item.erase("native_suffix");
        }
        result["strings"].push_back(item);
    }
    if (!result.contains("settings"))
    {
        throw Error("legacy config has no settings");
    }
    Cursor blocks{trailing};
    while (!blocks.done())
    {
        result["blocks"].push_back(hex(blocks.take(read_decimal(blocks))));
    }
    return result;
}

Bytes encode_legacy_config(const Json& config)
{
    Bytes raw;
    auto item = [&](const Bytes& bytes)
    {
        put(raw, checked32(bytes.size()));
        append(raw, bytes);
    };
    for (size_t index = 0; index < config.at("strings").size(); ++index)
    {
        if (index == 1)
        {
            item(encode_compact_settings(config.at("settings"),
                                         DataFormat::Cp932,
                                         config.value("compact_settings_terminated", true)));
        }
        const auto& string = config.at("strings")[index];
        if (string.contains("text"))
        {
            auto bytes = encode_cp932(string.at("text"));
            append(bytes, unhex(string.value("native_suffix", "")));
            item(bytes);
        }
        else
        {
            item(unhex(string.at("hex")));
        }
    }
    auto output = index_encode(raw);
    for (size_t index = 1; index < config.at("blocks").size(); ++index)
    {
        const auto bytes = unhex(config.at("blocks")[index]);
        write_decimal(output, checked32(bytes.size()));
        append(output, bytes);
    }
    return output;
}

std::optional<std::array<uint32_t, 20>> compact_image_attributes(std::span<const uint8_t> input)
{
    if (input.size() < 2 || (input[0] != 'a' && input[0] != 'd') || input[1] != 239)
    {
        return std::nullopt;
    }
    try
    {
        Cursor reader{input};
        reader.take(2);
        std::array<uint32_t, 20> attributes{};
        for (auto& value : attributes)
        {
            value = read_attribute(reader);
        }
        const auto flags = attributes[13];
        const auto alpha = flags & 4 ? uint64_t(attributes[9]) : 0;
        const auto size = alpha + attributes[11] + uint64_t(attributes[17]);
        if (flags > 7 || size != input.size() - reader.pos || (!(flags & 1) && attributes[17]))
        {
            return std::nullopt;
        }
        return attributes;
    }
    catch (const Error&)
    {
        return std::nullopt;
    }
}

std::optional<std::array<uint32_t, 20>> legacy_image_attributes(std::span<const uint8_t> input)
{
    if (input.size() < 2 || input[0] != 'a' || input[1] != 239) return std::nullopt;
    try
    {
        Cursor reader{input};
        reader.take(2);
        std::array<uint32_t, 20> attributes{};
        for (auto& value : attributes) value = read_attribute(reader);
        if ((attributes[13] != 0 && attributes[13] != 7) || attributes[17] ||
            (!attributes[13] && attributes[6]) ||
            uint64_t(attributes[6]) + attributes[1] != input.size() - reader.pos)
            return std::nullopt;
        return attributes;
    }
    catch (const Error&) { return std::nullopt; }
}

std::optional<std::array<uint32_t, 8>> legacy22_image_attributes(std::span<const uint8_t> input)
{
    if (input.size() < 2 || input[0] != 'a' || input[1] != 251)
    {
        return std::nullopt;
    }
    // A modern image's base ID can also have 0xfb as its low byte.
    try
    {
        Cursor reader{input};
        reader.take(2);
        std::array<uint32_t, 8> values{};
        for (auto& value : values)
        {
            value = read_attribute22(reader);
        }
        const auto flags = values[7];
        const uint64_t size =
            uint64_t(values[5]) * 2 + (flags & 4 ? values[3] : 0) + uint64_t(values[2]);
        if (flags > 7 || size != input.size() - reader.pos ||
            (!(flags & 2) && (values[5] || values[3])) ||
            ((flags & 2) && (values[5] < (uint64_t(values[0]) * values[4] + 7) / 8 ||
                             values[5] > uint64_t(values[0]) * values[4] / 8 + 1)))
        {
            return std::nullopt;
        }
        return values;
    }
    catch (const Error&)
    {
        return std::nullopt;
    }
}

namespace
{
// Width, height, flags, mask length, opacity length, color length.
constexpr std::array<size_t, 6> masked_fields{1, 3, 10, 17, 6, 8};
constexpr std::array<size_t, 6> harami_fields{10, 1, 14, 3, 8, 17};
constexpr std::array<size_t, 6> oshioki_fields{2, 4, 9, 13, 6, 17};
constexpr std::array<size_t, 6> zettai_fields{4, 6, 13, 17, 9, 11};

std::optional<std::array<uint32_t, 20>> masked_attributes(
    std::span<const uint8_t> input, const std::array<size_t, 6>& fields,
    bool empty_mask_flag = false)
{
    if (input.size() < 2 || input[0] != 'a' || input[1] != 239) return std::nullopt;
    try
    {
        Cursor reader{input};
        reader.take(2);
        std::array<uint32_t, 20> values{};
        for (auto& value : values) value = read_attribute(reader);
        const auto width = values[fields[0]], height = values[fields[1]];
        const auto original_flags = values[fields[2]], mask = values[fields[3]];
        const auto flags = empty_mask_flag && original_flags == 1 ? 3u : original_flags;
        const auto alpha = values[fields[4]], color = values[fields[5]];
        const uint64_t pixels = uint64_t(width) * height;
        const uint64_t size = uint64_t(mask) * 2 + (flags & 4 ? alpha : 0) + uint64_t(color);
        if (!width || !height || pixels > 64 * 1024 * 1024 ||
            (flags != 0 && flags != 3 && flags != 7) || size != input.size() - reader.pos ||
            (!flags && (mask || alpha || (color != pixels && color != pixels * 3))) ||
            (flags && (mask != pixels / 8 + 1 || color % 3 || (!(flags & 4) && alpha))))
            return std::nullopt;
        if (original_flags == 1 &&
            (alpha || color || std::any_of(input.begin() + reader.pos, input.end(),
                                           [](uint8_t value) { return value != 0; })))
            return std::nullopt;
        return values;
    }
    catch (const Error&) { return std::nullopt; }
}
}

std::optional<std::array<uint32_t, 20>> masked_image_attributes(std::span<const uint8_t> input)
{
    return masked_attributes(input, masked_fields);
}

std::optional<std::array<uint32_t, 20>> harami_image_attributes(std::span<const uint8_t> input)
{
    return masked_attributes(input, harami_fields);
}

std::optional<std::array<uint32_t, 20>> oshioki_image_attributes(std::span<const uint8_t> input)
{
    return masked_attributes(input, oshioki_fields);
}

std::optional<std::array<uint32_t, 20>> zettai_image_attributes(std::span<const uint8_t> input)
{
    return masked_attributes(input, zettai_fields, true);
}

std::optional<std::array<uint32_t, 20>> fuka_image_attributes(std::span<const uint8_t> input)
{
    if (input.size() < 2 || input[0] != 'a' || input[1] != 239) return std::nullopt;
    try
    {
        Cursor reader{input};reader.take(2);
        std::array<uint32_t,20> values{};
        for (auto& value : values) value=read_attribute(reader);
        const uint64_t pixels=uint64_t(values[0])*values[5];
        const uint64_t padded=(uint64_t(values[0])*3+(values[0]&3))*values[5];
        const auto alpha=values[9],color=values[11],flags=values[13];
        if (!values[0] || !values[5] || pixels>64*1024*1024 || values[17] ||
            (flags!=0 && flags!=7) || uint64_t(alpha)+color!=input.size()-reader.pos ||
            (!flags && alpha) ||
            (color!=pixels*3 && color!=padded && (flags || color!=pixels)) ||
            (flags && alpha!=color))
            return std::nullopt;
        return values;
    }
    catch (const Error&) { return std::nullopt; }
}

std::optional<std::array<uint32_t, 8>> legacy8_image_attributes(std::span<const uint8_t> input)
{
    if (input.size() < 2 || input[0] != 'a' || input[1] != 239) return std::nullopt;
    try
    {
        Cursor reader{input};
        reader.take(2);
        std::array<uint32_t, 8> values{};
        for (auto& value : values) value = read_attribute(reader);
        const uint64_t pixels = uint64_t(values[1]) * values[2];
        const auto flags = values[4];
        const uint64_t size = uint64_t(values[5]) * 2 +
                              (flags & 4 ? values[3] : 0) + uint64_t(values[6]);
        if (!values[1] || !values[2] || pixels > 64 * 1024 * 1024 || flags > 7 ||
            size != input.size() - reader.pos ||
            (!(flags & 2) && (values[5] || values[3] ||
                             (values[6] != pixels && values[6] != pixels * 3))) ||
            ((flags & 2) && (values[5] < (pixels + 7) / 8 ||
                             values[5] > pixels / 8 + 1 || values[6] % 3)))
            return std::nullopt;
        return values;
    }
    catch (const Error&) { return std::nullopt; }
}

std::optional<std::array<uint32_t, 8>> ani8_image_attributes(std::span<const uint8_t> input)
{
    if (input.size() < 2 || input[0] != 'a' || input[1] != 239) return std::nullopt;
    if (legacy8_image_attributes(input)) return std::nullopt;
    try
    {
        Cursor reader{input}; reader.take(2);
        std::array<uint32_t, 8> v{};
        for (auto& value : v) value = read_attribute(reader);
        // aniyomeiji.exe 0x456db0, jump table 0x4572f0:
        // width, height, mask bytes, unknown, unknown, color bytes, alpha bytes, flags.
        const uint64_t pixels = uint64_t(v[0]) * v[1];
        const auto flags = v[7];
        const uint64_t size = uint64_t(v[2])*2 + (flags&4 ? v[6] : 0) + uint64_t(v[5]);
        if (!pixels || pixels > 64*1024*1024 || flags > 7 || size != input.size()-reader.pos)
            return std::nullopt;
        if (flags == 1)
        {
            if (v[6] || v[5] || v[2] != pixels/8+1 ||
                std::any_of(input.begin()+reader.pos, input.end(), [](uint8_t b){return b!=0;}))
                return std::nullopt;
        }
        else if ((!(flags&2) && (v[2] || v[6] || (v[5]!=pixels && v[5]!=pixels*3))) ||
                 ((flags&2) && (v[2] < (pixels+7)/8 || v[2] > pixels/8+1 || v[5]%3)))
            return std::nullopt;
        return v;
    }
    catch (const Error&) { return std::nullopt; }
}

Bytes ani8_image(const Image& image, std::array<uint32_t, 8> v, const Image* channel_alpha,
                 bool top_down)
{
    const auto encoded = iji22_image(image, {v[3],v[2],v[0],v[1],v[6],v[5],v[4],v[7]},
                                     channel_alpha, top_down);
    Cursor reader{encoded}; reader.take(2);
    std::array<uint32_t, 8> attrs{};
    for (auto& value : attrs) value = read_attribute22(reader);
    v = {attrs[2],attrs[3],attrs[1],attrs[0],attrs[6],attrs[5],attrs[4],attrs[7]};
    Bytes output{'a',239};
    for (const auto value : v) write_attribute(output,value);
    append(output,std::span(encoded).subspan(reader.pos));
    return output;
}

std::optional<std::array<uint32_t, 8>> natsu8_image_attributes(std::span<const uint8_t> input)
{
    if (input.size() < 2 || input[0] != 'a' || input[1] != 239) return std::nullopt;
    if (legacy8_image_attributes(input) || ani8_image_attributes(input)) return std::nullopt;
    try
    {
        Cursor reader{input}; reader.take(2);
        std::array<uint32_t, 8> v{};
        for (auto& value : v) value = read_attribute(reader);
        // Natsu Doki: unknown, height, mask, color, opacity, width, flags, unknown.
        const uint64_t pixels = uint64_t(v[5]) * v[1];
        const auto flags = v[6];
        const uint64_t size = uint64_t(v[2])*2 + (flags&4 ? v[4] : 0) + uint64_t(v[3]);
        if (!pixels || pixels > 64*1024*1024 || flags > 7 || size != input.size()-reader.pos)
            return std::nullopt;
        if (flags == 1)
        {
            if (v[4] || v[3] || v[2] != pixels/8+1 ||
                std::any_of(input.begin()+reader.pos, input.end(), [](uint8_t b){return b!=0;}))
                return std::nullopt;
        }
        else if ((!(flags&2) && (v[2] || v[4] || (v[3]!=pixels && v[3]!=pixels*3))) ||
                 ((flags&2) && (v[2] < (pixels+7)/8 || v[2] > pixels/8+1 || v[3]%3)))
            return std::nullopt;
        return v;
    }
    catch (const Error&) { return std::nullopt; }
}

Bytes natsu8_image(const Image& image, std::array<uint32_t, 8> v, const Image* channel_alpha,
                   bool top_down)
{
    const auto encoded = iji22_image(image, {v[0],v[2],v[5],v[1],v[4],v[3],v[7],v[6]},
                                     channel_alpha, top_down);
    Cursor reader{encoded}; reader.take(2);
    std::array<uint32_t, 8> attrs{};
    for (auto& value : attrs) value = read_attribute22(reader);
    v = {attrs[0],attrs[3],attrs[1],attrs[5],attrs[4],attrs[2],attrs[7],attrs[6]};
    Bytes output{'a',239};
    for (const auto value : v) write_attribute(output,value);
    append(output,std::span(encoded).subspan(reader.pos));
    return output;
}

std::optional<std::array<uint32_t, 8>> gyk8_image_attributes(std::span<const uint8_t> input)
{
    if (input.size() < 2 || input[0] != 'a' || input[1] != 239) return std::nullopt;
    if (legacy8_image_attributes(input) || ani8_image_attributes(input) ||
        natsu8_image_attributes(input)) return std::nullopt;
    try
    {
        Cursor reader{input}; reader.take(2);
        std::array<uint32_t, 8> v{};
        for (auto& value : v) value = read_attribute(reader);
        // GYK: unknown, flags, mask, color, height, width, opacity, unknown.
        const uint64_t pixels = uint64_t(v[5]) * v[4];
        const auto flags = v[1];
        const uint64_t size = uint64_t(v[2])*2 + (flags&4 ? v[6] : 0) + uint64_t(v[3]);
        if (!pixels || pixels > 64*1024*1024 || flags > 7 || size != input.size()-reader.pos)
            return std::nullopt;
        if (flags == 1)
        {
            if (v[6] || v[3] || v[2] != pixels/8+1 ||
                std::any_of(input.begin()+reader.pos, input.end(), [](uint8_t b){return b!=0;}))
                return std::nullopt;
        }
        else if ((!(flags&2) && (v[2] || v[6] || (v[3]!=pixels && v[3]!=pixels*3))) ||
                 ((flags&2) && (v[2] < (pixels+7)/8 || v[2] > pixels/8+1 || v[3]%3)))
            return std::nullopt;
        return v;
    }
    catch (const Error&) { return std::nullopt; }
}

Bytes gyk8_image(const Image& image, std::array<uint32_t, 8> v, const Image* channel_alpha,
                 bool top_down)
{
    const auto encoded = iji22_image(image, {v[0],v[2],v[5],v[4],v[6],v[3],v[7],v[1]},
                                     channel_alpha, top_down);
    Cursor reader{encoded}; reader.take(2);
    std::array<uint32_t, 8> attrs{};
    for (auto& value : attrs) value = read_attribute22(reader);
    v = {attrs[0],attrs[7],attrs[1],attrs[5],attrs[3],attrs[2],attrs[4],attrs[6]};
    Bytes output{'a',239};
    for (const auto value : v) write_attribute(output,value);
    append(output,std::span(encoded).subspan(reader.pos));
    return output;
}

std::optional<std::array<uint32_t, 8>> shuffled22_image_attributes(std::span<const uint8_t> input)
{
    if (input.size() < 2 || input[0] != 'a' || input[1] != 251) return std::nullopt;
    if (legacy22_image_attributes(input)) return std::nullopt;
    try
    {
        Cursor reader{input}; reader.take(2);
        std::array<uint32_t, 8> values{};
        for (auto& value : values) value = read_attribute22(reader);
        const uint64_t pixels = uint64_t(values[6]) * values[7];
        const auto flags = values[2];
        const uint64_t size = uint64_t(values[4])*2 + (flags&4 ? values[1] : 0) + uint64_t(values[3]);
        if (!values[6] || !values[7] || pixels > 64*1024*1024 || flags > 7 ||
            size != input.size()-reader.pos ||
            (!(flags&2) && (values[4] || values[1] || (values[3]!=pixels && values[3]!=pixels*3))) ||
            ((flags&2) && (values[4] < (pixels+7)/8 || values[4] > pixels/8+1 || values[3]%3)))
            return std::nullopt;
        return values;
    }
    catch(const Error&) { return std::nullopt; }
}

Bytes shuffled22_image(const Image& image, std::array<uint32_t, 8> values, const Image* channel_alpha)
{
    // Gibo reader 0x450d20: height at +0x0c, width at +0x10, flags at +0xa4.
    const auto original_flags = values[2];
    const bool plain_color = !(values[2]&2) && values[3] == uint64_t(values[6])*values[7]*3;
    std::array<uint32_t, 8> translated{values[7],values[0],values[3],values[1],
                                      values[6],values[4],values[5],plain_color ? 3u : values[2]};
    const auto encoded = legacy22_image(image, translated, channel_alpha);
    const auto attrs = *legacy22_image_attributes(encoded);
    Cursor reader{encoded}; reader.take(2);
    for (size_t i=0;i<8;++i) read_attribute22(reader);
    values[7]=attrs[0]; values[6]=attrs[4]; values[3]=attrs[2]; values[1]=attrs[3];
    values[4]=attrs[5]; values[2]=attrs[7];
    if (plain_color && values[2]==3)
    {
        reader.take(size_t(values[4])*2);
        values[4]=0; values[2]=original_flags;
    }
    Bytes output{'a',251};
    for (const auto value : values) write_attribute22(output,value);
    append(output,std::span(encoded).subspan(reader.pos));
    return output;
}

std::optional<std::array<uint32_t, 8>> iji22_image_attributes(std::span<const uint8_t> input)
{
    if (input.size() < 2 || input[0] != 'a' || input[1] != 251) return std::nullopt;
    if (legacy22_image_attributes(input) || shuffled22_image_attributes(input)) return std::nullopt;
    try
    {
        Cursor reader{input}; reader.take(2);
        std::array<uint32_t, 8> v{};
        for (auto& value : v) value = read_attribute22(reader);
        const uint64_t pixels = uint64_t(v[2]) * v[3];
        const auto flags = v[7];
        const uint64_t size = uint64_t(v[1])*2 + (flags&4 ? v[4] : 0) + uint64_t(v[5]);
        if (!pixels || pixels > 64*1024*1024 || flags > 7 || size != input.size()-reader.pos)
            return std::nullopt;
        // Native renderer 0x4125f5 skips drawing when bit 0 is set without bit 1.
        // The two observed no-draw packets retain empty masks and no color plane.
        if (flags == 1)
        {
            if (v[4] || v[5] || v[1] != pixels/8+1 ||
                std::any_of(input.begin()+reader.pos, input.end(), [](uint8_t b){return b!=0;}))
                return std::nullopt;
        }
        else if ((!(flags&2) && (v[1] || v[4] || (v[5]!=pixels && v[5]!=pixels*3))) ||
                 ((flags&2) && (v[1] < (pixels+7)/8 || v[1] > pixels/8+1 || v[5]%3)))
            return std::nullopt;
        return v;
    }
    catch (const Error&) { return std::nullopt; }
}

Bytes iji22_image(const Image& image, std::array<uint32_t, 8> v, const Image* channel_alpha,
                  bool top_down)
{
    const auto original_flags = v[7];
    const bool plain_color = !v[7] && v[5] == uint64_t(v[2])*v[3]*3;
    std::array<uint32_t, 8> translated{v[3],v[0],v[5],v[4],v[2],v[1],v[6],
                                      plain_color || v[7]==1 ? 3u : v[7]};
    const auto encoded = legacy22_image(image,translated,channel_alpha,top_down);
    const auto attrs = *legacy22_image_attributes(encoded);
    Cursor reader{encoded}; reader.take(2);
    for (size_t i=0;i<8;++i) read_attribute22(reader);
    v[3]=attrs[0]; v[2]=attrs[4]; v[5]=attrs[2]; v[4]=attrs[3]; v[1]=attrs[5]; v[7]=attrs[7];
    if (plain_color && v[7]==3)
    {
        reader.take(size_t(v[1])*2);
        v[1]=0; v[7]=0;
    }
    if (original_flags==1 && !v[4] && !v[5]) v[7]=1;
    Bytes output{'a',251};
    for (const auto value : v) write_attribute22(output,value);
    append(output,std::span(encoded).subspan(reader.pos));
    return output;
}

std::optional<std::array<uint32_t, 8>> lesson22_image_attributes(std::span<const uint8_t> input)
{
    if (input.size() < 2 || input[0] != 'a' || input[1] != 251) return std::nullopt;
    if (legacy22_image_attributes(input) || shuffled22_image_attributes(input) ||
        iji22_image_attributes(input)) return std::nullopt;
    try
    {
        Cursor reader{input}; reader.take(2);
        std::array<uint32_t, 8> v{};
        for (auto& value : v) value = read_attribute22(reader);
        // oyakolesson.exe 0x456650, jump table 0x456b90: width, mask,
        // color, height, alpha, unknown, unknown, flags.
        const uint64_t pixels = uint64_t(v[0]) * v[3];
        const auto flags = v[7];
        const uint64_t size = uint64_t(v[1])*2 + (flags&4 ? v[4] : 0) + uint64_t(v[2]);
        if (!pixels || pixels > 64*1024*1024 || flags > 7 || size != input.size()-reader.pos)
            return std::nullopt;
        if (flags == 1)
        {
            if (v[4] || v[2] || v[1] != pixels/8+1 ||
                std::any_of(input.begin()+reader.pos, input.end(), [](uint8_t b){return b!=0;}))
                return std::nullopt;
        }
        else if ((!(flags&2) && (v[1] || v[4] || (v[2]!=pixels && v[2]!=pixels*3))) ||
                 ((flags&2) && (v[1] < (pixels+7)/8 || v[1] > pixels/8+1 || v[2]%3)))
            return std::nullopt;
        return v;
    }
    catch (const Error&) { return std::nullopt; }
}

Bytes lesson22_image(const Image& image, std::array<uint32_t, 8> v, const Image* channel_alpha,
                     bool top_down)
{
    // The pixel codec is the same as Iji Care; only three attribute positions differ.
    const auto encoded = iji22_image(image, {v[5],v[1],v[0],v[3],v[4],v[2],v[6],v[7]},
                                     channel_alpha, top_down);
    Cursor reader{encoded}; reader.take(2);
    std::array<uint32_t, 8> attrs{};
    for (auto& value : attrs) value = read_attribute22(reader);
    v = {attrs[2],attrs[1],attrs[5],attrs[3],attrs[4],attrs[0],attrs[6],attrs[7]};
    Bytes output{'a',251};
    for (const auto value : v) write_attribute22(output,value);
    append(output,std::span(encoded).subspan(reader.pos));
    return output;
}

Bytes legacy22_image(const Image& source, std::array<uint32_t, 8> values, const Image* channel_alpha,
                     bool top_down)
{
    const auto image = top_down ? vertically_flipped(source) : source;
    const auto native_alpha = channel_alpha && top_down
                                  ? std::optional<Image>(vertically_flipped(*channel_alpha)) : std::nullopt;
    if (native_alpha) channel_alpha = &*native_alpha;
    const auto pixels = checked32(uint64_t(image.width) * image.height);
    if (!pixels || image.bgra.size() != uint64_t(pixels) * 4)
    {
        throw Error("invalid version-22 replacement image");
    }
    if (channel_alpha &&
        (channel_alpha->width != image.width || channel_alpha->height != image.height ||
         channel_alpha->bgra.size() != image.bgra.size()))
    {
        throw Error("channel alpha image dimensions differ from the color image");
    }
    const bool grayscale = !(values[7] & 2) && values[2] == uint64_t(values[0]) * values[4];
    values[0] = image.height;
    values[4] = image.width;
    Bytes opaque, translucent, alpha, color;
    if (grayscale)
    {
        for (size_t pixel = 0; pixel < pixels; ++pixel)
        {
            const auto at = pixel * 4;
            if (image.bgra[at] != image.bgra[at + 1] || image.bgra[at] != image.bgra[at + 2] ||
                image.bgra[at + 3] != 255)
            {
                throw Error("grayscale mask must contain opaque gray pixels");
            }
            color.push_back(image.bgra[at]);
        }
        values[7] = 0;
    }
    else
    {
        opaque.resize(pixels / 8 + 1);
        translucent.resize(opaque.size());
        for (size_t pixel = 0; pixel < pixels; ++pixel)
        {
            const auto at = pixel * 4;
            std::array<uint8_t, 3> opacity{};
            for (size_t channel = 0; channel < 3; ++channel)
            {
                opacity[channel] =
                    channel_alpha ? channel_alpha->bgra[at + channel] : image.bgra[at + 3];
            }
            const auto bit = uint8_t(1 << (pixel % 8));
            const bool solid = opacity == std::array<uint8_t, 3>{255, 255, 255};
            const bool visible = solid || opacity != std::array<uint8_t, 3>{0, 0, 0} ||
                                 image.bgra[at] || image.bgra[at + 1] || image.bgra[at + 2];
            if (solid)
            {
                opaque[pixel / 8] |= bit;
            }
            else if (visible)
            {
                translucent[pixel / 8] |= bit;
                append(alpha, opacity);
            }
            if (visible)
            {
                append(color, std::span(image.bgra).subspan(at, 3));
            }
        }
        values[7] = (values[7] & 1) | 2 | (alpha.empty() ? 0 : 4);
    }
    values[2] = checked32(color.size());
    values[3] = checked32(alpha.size());
    values[5] = checked32(opaque.size());
    Bytes output{'a', 251};
    for (const auto value : values)
    {
        write_attribute22(output, value);
    }
    append(output, opaque);
    append(output, translucent);
    append(output, alpha);
    append(output, color);
    return output;
}

Bytes legacy8_image(const Image& image, std::array<uint32_t, 8> values, const Image* channel_alpha,
                    bool top_down)
{
    const bool plain_color = !(values[4] & 2) &&
                             values[6] == uint64_t(values[1]) * values[2] * 3;
    // Yumekoi's reader stores attribute 1 at +0x0c (height), and attribute 2
    // at +0x10 (width). Its rectangle copier uses +0x10 as the row stride.
    std::array<uint32_t, 8> translated{values[1], 0, values[6], values[3],
                                       values[2], values[5], 0, plain_color ? 3u : values[4]};
    const auto encoded = legacy22_image(image, translated, channel_alpha, top_down);
    const auto attributes = *legacy22_image_attributes(encoded);
    Cursor reader{encoded};
    reader.take(2);
    for (size_t index = 0; index < attributes.size(); ++index) read_attribute22(reader);
    values[1] = attributes[0];
    values[2] = attributes[4];
    values[3] = attributes[3];
    values[4] = attributes[7];
    values[5] = attributes[5];
    values[6] = attributes[2];
    if (plain_color && values[4] == 3)
    {
        reader.take(size_t(attributes[5]) * 2);
        values[4] = values[5] = 0;
    }
    Bytes output{'a', 239};
    for (const auto value : values) write_attribute(output, value);
    append(output, std::span(encoded).subspan(reader.pos));
    return output;
}

namespace
{
Bytes encode_masked_bitmap(const Image& image, std::array<uint32_t, 20> values,
                           const std::array<size_t, 6>& fields, const Image* channel_alpha,
                           bool top_down, bool empty_mask_flag = false)
{
    // This v23 layout retains the two visibility masks and BGR opacity of v22,
    // with a different attribute radix and shuffled fields in a 20-word header.
    std::array<uint32_t, 8> old_values{values[fields[1]], 0, values[fields[5]], values[fields[4]],
                                      values[fields[0]], values[fields[3]], 0, values[fields[2]]};
    const bool plain_color = !old_values[7] &&
                             old_values[2] == uint64_t(old_values[4]) * old_values[0] * 3;
    if (plain_color) old_values[7] = 3;
    const auto encoded = legacy22_image(image, old_values, channel_alpha, top_down);
    const auto attributes = *legacy22_image_attributes(encoded);
    Cursor reader{encoded};
    reader.take(2);
    for (size_t index = 0; index < attributes.size(); ++index) read_attribute22(reader);
    values[fields[0]] = attributes[4];
    values[fields[1]] = attributes[0];
    values[fields[5]] = attributes[2];
    values[fields[4]] = attributes[3];
    values[fields[3]] = attributes[5];
    values[fields[2]] = attributes[7];
    if (empty_mask_flag && old_values[7] == 1 && !attributes[2] && !attributes[3])
        values[fields[2]] = 1;
    if (plain_color && values[fields[2]] == 3)
    {
        reader.take(size_t(attributes[5]) * 2);
        values[fields[3]] = values[fields[2]] = 0;
    }
    Bytes output{'a', 239};
    for (const auto value : values) write_attribute(output, value);
    append(output, std::span(encoded).subspan(reader.pos));
    return output;
}
}

Bytes masked_image(const Image& image, std::array<uint32_t, 20> values, const Image* channel_alpha,
                   bool top_down)
{
    return encode_masked_bitmap(image, values, masked_fields, channel_alpha, top_down);
}

Bytes harami_image(const Image& image, std::array<uint32_t, 20> values, const Image* channel_alpha,
                   bool top_down)
{
    return encode_masked_bitmap(image, values, harami_fields, channel_alpha, top_down);
}

Bytes oshioki_image(const Image& image, std::array<uint32_t, 20> values, const Image* channel_alpha,
                    bool top_down)
{
    return encode_masked_bitmap(image, values, oshioki_fields, channel_alpha, top_down);
}

Bytes zettai_image(const Image& image, std::array<uint32_t, 20> values, const Image* channel_alpha,
                   bool top_down)
{
    return encode_masked_bitmap(image, values, zettai_fields, channel_alpha, top_down, true);
}

Bytes fuka_image(const Image& image, std::array<uint32_t, 20> values, const Image* channel_alpha,
                 bool top_down)
{
    auto attributes=values;
    attributes[0]=values[0];attributes[8]=values[5];attributes[1]=values[11];
    attributes[6]=values[9];
    const auto reversed_alpha=channel_alpha && !top_down
                                  ? std::optional<Image>(vertically_flipped(*channel_alpha)) : std::nullopt;
    const auto encoded=legacy_bitmap(top_down ? image : vertically_flipped(image),attributes,
                                      reversed_alpha ? &*reversed_alpha : channel_alpha);
    Cursor reader{encoded};reader.take(2);
    for (auto& value : attributes) value=read_attribute(reader);
    values[0]=attributes[0];values[5]=attributes[8];values[9]=attributes[6];
    values[11]=attributes[1];values[13]=attributes[13];values[17]=0;
    Bytes output{'a',239};
    for (const auto value : values) write_attribute(output,value);
    append(output,std::span(encoded).subspan(reader.pos));
    return output;
}

Bytes canonical_image(std::span<const uint8_t> input,
                      DataFormat format,
                      std::span<const uint32_t> bitmap_ids,
                      const ImageProfileContext* profile, uint32_t entry_id)
{
    if (input.empty())
    {
        return Bytes(input.begin(), input.end());
    }
    if (is_tb1(input)) return Bytes(input.begin(), input.end());
    if (const auto metadata = bgra_profile_metadata(input))
        return canonical_profile_image(input, *metadata);
    if (profile && !profile->id.empty())
    {
        if (const auto metadata = profile_image_metadata(input, profile->id))
        {
            const auto baseline = profile->baselines.find(entry_id);
            return canonical_profile_image(input, *metadata, baseline == profile->baselines.end()
                ? std::nullopt : std::optional<uint32_t>(baseline->second));
        }
        const auto& profiles = experimental_image_profiles().at("profiles");
        for (const auto& item : profiles)
            if (item.at("id") == profile->id && input.size() >= 2 &&
                ((input[0] == 'a' || input[0] == 'd') && input[1] == item.at("markers")[1]) &&
                !known_image_layout(input))
            {
                try { return canonical_profile_image(input, {{"profile",profile->id}}); }
                catch (const Error& error) { throw Error("experimental image profile " + profile->id + ": " + error.what()); }
            }
    }
    if (input[0] == 'b' || input[0] == 'c')
    {
        if (input.size() < 5) throw Error("truncated image wrapper");
        const uint32_t size = (uint32_t(input[1])<<24) | (uint32_t(input[2])<<16) |
                              (uint32_t(input[3])<<8) | input[4];
        if (input[0] == 'b')
        {
            if (size != input.size()-5) throw Error("image wrapper size mismatch");
            return Bytes(input.begin(),input.end());
        }
        if (size == input.size()-5)
        {
            Bytes result(input.begin(),input.begin()+5);
            result.push_back(0);
            append(result,input.subspan(5));
            return result;
        }
        if (input.size() >= 6 && size == input.size()-6) return Bytes(input.begin(),input.end());
        throw Error("image wrapper size mismatch");
    }
    if (const auto attributes = shuffled22_image_attributes(input))
    {
        Cursor reader{input}; reader.take(2);
        for (size_t i=0;i<8;++i) read_attribute22(reader);
        const auto& v=*attributes;
        Bytes translated{'a',251};
        for (const auto value : {v[7],v[0],v[3],v[1],v[6],v[4],v[5],v[2]})
            write_attribute22(translated,value);
        append(translated,input.subspan(reader.pos));
        return canonical_image(translated,format);
    }
    if (const auto attributes = iji22_image_attributes(input))
    {
        Cursor reader{input}; reader.take(2);
        for (size_t i=0;i<8;++i) read_attribute22(reader);
        const auto& v=*attributes;
        Bytes translated{'a',251};
        for (const auto value : {v[3],v[0],v[5],v[4],v[2],v[1],v[6],v[7]==1 ? 3u : v[7]})
            write_attribute22(translated,value);
        append(translated,input.subspan(reader.pos));
        return canonical_image(translated,format);
    }
    if (const auto attributes = lesson22_image_attributes(input))
    {
        Cursor reader{input}; reader.take(2);
        for (size_t i=0;i<8;++i) read_attribute22(reader);
        const auto& v=*attributes;
        Bytes translated{'a',251};
        for (const auto value : {v[3],v[5],v[2],v[4],v[0],v[1],v[6],v[7]==1 ? 3u : v[7]})
            write_attribute22(translated,value);
        append(translated,input.subspan(reader.pos));
        return canonical_image(translated,format);
    }
    if (const auto attributes = legacy8_image_attributes(input))
    {
        Cursor reader{input};
        reader.take(2);
        for (size_t index = 0; index < attributes->size(); ++index) read_attribute(reader);
        const auto& values = *attributes;
        Bytes translated{'a', 251};
        for (const auto value : {values[1], 0u, values[6], values[3],
                                 values[2], values[5], 0u, values[4]})
            write_attribute22(translated, value);
        append(translated, input.subspan(reader.pos));
        return canonical_image(translated, format);
    }
    if (const auto attributes = ani8_image_attributes(input))
    {
        Cursor reader{input}; reader.take(2);
        for (size_t i=0;i<8;++i) read_attribute(reader);
        const auto& v=*attributes;
        Bytes translated{'a',251};
        for (const auto value : {v[1],v[3],v[5],v[6],v[0],v[2],v[4],v[7]==1 ? 3u : v[7]})
            write_attribute22(translated,value);
        append(translated,input.subspan(reader.pos));
        return canonical_image(translated,format);
    }
    if (const auto attributes = natsu8_image_attributes(input))
    {
        Cursor reader{input}; reader.take(2);
        for (size_t i=0;i<8;++i) read_attribute(reader);
        const auto& v=*attributes;
        Bytes translated{'a',251};
        for (const auto value : {v[1],v[0],v[3],v[4],v[5],v[2],v[7],v[6]==1 ? 3u : v[6]})
            write_attribute22(translated,value);
        append(translated,input.subspan(reader.pos));
        return canonical_image(translated,format);
    }
    if (const auto attributes = gyk8_image_attributes(input))
    {
        Cursor reader{input}; reader.take(2);
        for (size_t i=0;i<8;++i) read_attribute(reader);
        const auto& v=*attributes;
        Bytes translated{'a',251};
        for (const auto value : {v[4],v[0],v[3],v[6],v[5],v[2],v[7],v[1]==1 ? 3u : v[1]})
            write_attribute22(translated,value);
        append(translated,input.subspan(reader.pos));
        return canonical_image(translated,format);
    }
    if (const auto attributes=fuka_image_attributes(input))
    {
        Cursor reader{input};reader.take(2);
        for (size_t i=0;i<attributes->size();++i) read_attribute(reader);
        const auto& values=*attributes;
        Bytes output{'a'};
        for (const auto value : {0u,0u,values[0],values[5],values[13],values[9]}) put(output,value);
        append(output,reader.take(values[9]));
        put(output,0);put(output,values[11]);append(output,reader.take(values[11]));
        reader.finish();
        return output;
    }
    const auto masked = masked_image_attributes(input);
    const auto harami = masked ? std::nullopt : harami_image_attributes(input);
    const auto oshioki = masked || harami ? std::nullopt : oshioki_image_attributes(input);
    const auto reordered = masked ? masked : harami ? harami : oshioki ? oshioki : zettai_image_attributes(input);
    if (reordered)
    {
        Cursor reader{input};
        reader.take(2);
        for (size_t index = 0; index < reordered->size(); ++index) read_attribute(reader);
        const auto& values = *reordered;
        const auto& fields = masked ? masked_fields : harami ? harami_fields : oshioki ? oshioki_fields
                             : zettai_fields;
        Bytes translated{'a', 251};
        for (const auto value : {values[fields[1]], 0u, values[fields[5]], values[fields[4]],
                                 values[fields[0]], values[fields[3]], 0u,
                                 values[fields[2]] == 1 ? 3u : values[fields[2]]})
            write_attribute22(translated, value);
        append(translated, input.subspan(reader.pos));
        return canonical_image(translated, format);
    }
    if (const auto attributes = legacy22_image_attributes(input))
    {
        Cursor reader{input};
        reader.take(2);
        for (size_t index = 0; index < attributes->size(); ++index)
        {
            read_attribute22(reader);
        }
        const auto& values = *attributes;
        const auto width = values[4];
        const auto height = values[0];
        const auto pixels = uint64_t(width) * height;
        if (!width || !height || pixels > 64 * 1024 * 1024)
        {
            throw Error("invalid version-22 image dimensions");
        }
        const auto opaque = reader.take(values[5]);
        const auto translucent = reader.take(values[5]);
        const auto alpha_offset = reader.pos;
        reader.take(values[7] & 4 ? values[3] : 0);
        Cursor alpha{input.subspan(alpha_offset)};
        Cursor color{reader.take(values[2])};
        reader.finish();
        Bytes colors, alphas;
        if (values[7] & 2)
        {
            colors.resize(size_t(pixels) * 3);
            alphas.resize(size_t(pixels) * 3);
            for (size_t pixel = 0; pixel < pixels; ++pixel)
            {
                const auto bit = uint8_t(1 << (pixel % 8));
                const bool partial = (translucent[pixel / 8] & bit) != 0;
                const bool solid = !partial && (opaque[pixel / 8] & bit) != 0;
                if (solid || ((values[7] & 4) && partial))
                {
                    const auto rgb = color.take(3);
                    std::copy(rgb.begin(), rgb.end(), colors.begin() + pixel * 3);
                }
                std::fill_n(alphas.begin() + pixel * 3, 3, uint8_t(solid ? 255 : 0));
                if (partial && (values[7] & 4))
                {
                    const auto rgb = alpha.take(3);
                    std::copy(rgb.begin(), rgb.end(), alphas.begin() + pixel * 3);
                }
            }
            color.finish();
        }
        else
        {
            const auto bytes = color.take(values[2]);
            colors.assign(bytes.begin(), bytes.end());
        }
        Bytes result{'a'};
        put(result, 0);
        put(result, values[6]);
        put(result, width);
        put(result, height);
        put(result, alphas.empty() ? 0 : 4);
        put(result, checked32(alphas.size()));
        append(result, alphas);
        put(result, 0);
        put(result, checked32(colors.size()));
        append(result, colors);
        return top_down_bitmap(std::move(result));
    }
    const auto compact_attributes = compact_image_attributes(input);
    if (compact_attributes)
    {
        Cursor reader{input};
        reader.take(2);
        for (size_t index = 0; index < compact_attributes->size(); ++index)
        {
            read_attribute(reader);
        }
        const auto& values = *compact_attributes;
        const auto flags = values[13];
        const auto mask = reader.take(values[17]);
        const auto alpha = reader.take(flags & 4 ? values[9] : 0);
        const auto color = reader.take(values[11]);
        reader.finish();
        uint32_t base = values[0];
        if (!mask.empty() && !bitmap_ids.empty())
        {
            if (base >= bitmap_ids.size())
            {
                throw Error("compact image base ordinal is outside the bitmap table");
            }
            base = bitmap_ids[base];
        }
        Bytes result{input[0]};
        put(result, base);
        put(result, 0);
        put(result, values[3]);
        put(result, values[format == DataFormat::Cp932 ? 5 : 8]);
        put(result, flags);
        put(result, checked32(alpha.size()));
        append(result, alpha);
        put(result, checked32(mask.size()));
        append(result, mask);
        put(result, checked32(color.size()));
        append(result, color);
        return result;
    }
    if (const auto metadata = profile_image_metadata(input))
        return canonical_profile_image(input, *metadata);
    if (format == DataFormat::Utf16)
    {
        return Bytes(input.begin(), input.end());
    }
    if (input[0] != 'a')
    {
        throw Error("unsupported legacy image marker");
    }
    Cursor reader{input};
    reader.u8();
    if (reader.u8() != 239)
    {
        throw Error("invalid legacy image header");
    }
    std::array<uint32_t, 20> attributes{};
    for (auto& value : attributes)
    {
        value = read_attribute(reader);
    }
    const auto width = attributes[0];
    const auto height = attributes[8];
    const auto flags = attributes[13];
    const auto color_size = attributes[1];
    const auto alpha_size = attributes[6];
    if (attributes[17] || (flags != 0 && flags != 7) || (flags == 0 && alpha_size))
    {
        throw Error("unsupported legacy image attributes");
    }
    const auto alpha = reader.take(alpha_size);
    const auto color = reader.take(color_size);
    reader.finish();
    Bytes result{'a'};
    put(result, 0);
    put(result, 0);
    put(result, width);
    put(result, height);
    put(result, flags);
    put(result, alpha_size);
    append(result, alpha);
    put(result, 0);
    put(result, color_size);
    append(result, color);
    return result;
}

std::optional<Image> legacy22_channel_alpha(std::span<const uint8_t> input)
{
    if (!legacy22_image_attributes(input))
    {
        return std::nullopt;
    }
    const auto canonical = canonical_image(input, DataFormat::Cp932);
    Cursor reader{canonical};
    reader.take(9);
    Image image;
    image.width = reader.u32();
    image.height = reader.u32();
    reader.u32();
    const auto size = reader.u32();
    const auto alpha = reader.take(size);
    if (size != uint64_t(image.width) * image.height * 3)
    {
        return std::nullopt;
    }
    bool separate = false;
    image.bgra.resize(size_t(image.width) * image.height * 4, 255);
    for (size_t pixel = 0; pixel < alpha.size() / 3; ++pixel)
    {
        const auto rgb = alpha.subspan(pixel * 3, 3);
        separate = separate || rgb[0] != rgb[1] || rgb[0] != rgb[2];
        std::copy(rgb.begin(), rgb.end(), image.bgra.begin() + pixel * 4);
    }
    return separate ? std::optional<Image>(std::move(image)) : std::nullopt;
}

Bytes compact_grayscale(const Image& image, std::array<uint32_t, 20> attributes, DataFormat format)
{
    attributes[3] = image.width;
    attributes[format == DataFormat::Cp932 ? 5 : 8] = image.height;
    attributes[9] = 0;
    attributes[11] = checked32(uint64_t(image.width) * image.height);
    attributes[13] = 0;
    attributes[17] = 0;
    Bytes output{'a', 239};
    for (const auto value : attributes)
    {
        write_attribute(output, value);
    }
    for (size_t pixel = 0; pixel < image.bgra.size(); pixel += 4)
    {
        if (image.bgra[pixel] != image.bgra[pixel + 1] ||
            image.bgra[pixel] != image.bgra[pixel + 2] || image.bgra[pixel + 3] != 255)
        {
            throw Error("grayscale mask must contain opaque gray pixels");
        }
        output.push_back(image.bgra[pixel]);
    }
    return output;
}

Bytes compact_bitmap(const Image& image, std::array<uint32_t, 20> attributes,
                     DataFormat format, const Image* channel_alpha)
{
    if (!image.width || !image.height ||
        image.bgra.size() != uint64_t(image.width) * image.height * 4 ||
        (channel_alpha && (channel_alpha->width != image.width ||
                           channel_alpha->height != image.height ||
                           channel_alpha->bgra.size() != image.bgra.size())))
    {
        throw Error("invalid compact bitmap or channel alpha dimensions");
    }
    const size_t stride = size_t(image.width) * 3 + (image.width & 3);
    Bytes color(stride * image.height), alpha(color.size());
    bool translucent = false;
    for (uint32_t y = 0; y < image.height; ++y)
    {
        for (uint32_t x = 0; x < image.width; ++x)
        {
            const auto pixel = (size_t(y) * image.width + x) * 4;
            const auto at = size_t(y) * stride + x * 3;
            for (size_t channel = 0; channel < 3; ++channel)
            {
                color[at + channel] = image.bgra[pixel + channel];
                const auto opacity = channel_alpha ? channel_alpha->bgra[pixel + channel]
                                                   : image.bgra[pixel + 3];
                alpha[at + channel] = opacity;
                translucent = translucent || opacity != 255;
            }
        }
    }
    if (!translucent) alpha.clear();
    attributes[0] = 0;
    attributes[3] = image.width;
    attributes[format == DataFormat::Cp932 ? 5 : 8] = image.height;
    attributes[9] = checked32(alpha.size());
    attributes[11] = checked32(color.size());
    attributes[13] = alpha.empty() ? 1 : 7;
    attributes[17] = 0;
    Bytes output{'a', 239};
    for (const auto value : attributes) write_attribute(output, value);
    append(output, alpha);
    append(output, color);
    return output;
}

Bytes legacy_bitmap(const Image& image, std::array<uint32_t, 20> attributes,
                    const Image* channel_alpha)
{
    Bytes alpha, color;
    const bool grayscale = attributes[13] == 0 &&
                           attributes[1] == uint64_t(attributes[0]) * attributes[8];
    if (grayscale)
    {
        for (size_t pixel = 0; pixel < image.bgra.size(); pixel += 4)
        {
            if (image.bgra[pixel] != image.bgra[pixel + 1] ||
                image.bgra[pixel] != image.bgra[pixel + 2] || image.bgra[pixel + 3] != 255)
                throw Error("grayscale mask must contain opaque gray pixels");
            color.push_back(image.bgra[pixel]);
        }
    }
    else
    {
        const auto canonical = canonical_image(compact_bitmap(image, {}, DataFormat::Cp932,
                                                              channel_alpha), DataFormat::Cp932);
        Cursor reader{canonical};
        reader.take(21);
        const auto alpha_bytes = reader.take(reader.u32());
        alpha.assign(alpha_bytes.begin(), alpha_bytes.end());
        reader.u32();
        const auto color_bytes = reader.take(reader.u32());
        color.assign(color_bytes.begin(), color_bytes.end());
    }
    attributes[0] = image.width;
    attributes[8] = image.height;
    attributes[1] = checked32(color.size());
    attributes[6] = checked32(alpha.size());
    attributes[13] = alpha.empty() ? 0 : 7;
    attributes[17] = 0;
    Bytes output{'a', 239};
    for (const auto value : attributes) write_attribute(output, value);
    append(output, alpha);
    append(output, color);
    return output;
}

std::optional<Image> compact_channel_alpha(std::span<const uint8_t> input, DataFormat format)
{
    if (!compact_image_attributes(input)) return std::nullopt;
    const auto canonical = canonical_image(input, format);
    Cursor reader{canonical};
    reader.take(9);
    Image image;
    image.width = reader.u32();
    image.height = reader.u32();
    reader.u32();
    const auto alpha = reader.take(reader.u32());
    const auto mask_size = reader.u32();
    const size_t stride = size_t(image.width) * 3 + (image.width & 3);
    if (mask_size || !image.width || !image.height ||
        alpha.size() != stride * image.height) return std::nullopt;
    image.bgra.resize(size_t(image.width) * image.height * 4, 255);
    bool separate = false;
    for (uint32_t y = 0; y < image.height; ++y)
    {
        for (uint32_t x = 0; x < image.width; ++x)
        {
            const auto pixel = (size_t(y) * image.width + x) * 4;
            const auto at = size_t(y) * stride + x * 3;
            separate = separate || alpha[at] != alpha[at + 1] || alpha[at] != alpha[at + 2];
            std::copy_n(alpha.begin() + at, 3, image.bgra.begin() + pixel);
        }
    }
    return separate ? std::optional<Image>(std::move(image)) : std::nullopt;
}

Bytes legacy_grayscale(const Image& image)
{
    const auto count = checked32(uint64_t(image.width) * image.height);
    std::array<uint32_t, 20> attributes{};
    attributes[0] = image.width;
    attributes[1] = count;
    attributes[8] = image.height;
    Bytes output{'a', 239};
    for (const auto value : attributes)
    {
        write_attribute(output, value);
    }
    for (size_t pixel = 0; pixel < image.bgra.size(); pixel += 4)
    {
        if (image.bgra[pixel] != image.bgra[pixel + 1] ||
            image.bgra[pixel] != image.bgra[pixel + 2] || image.bgra[pixel + 3] != 255)
        {
            throw Error("grayscale mask must contain opaque gray pixels");
        }
        output.push_back(image.bgra[pixel]);
    }
    return output;
}
}
