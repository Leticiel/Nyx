#include "audio.hpp"
#include "tool.hpp"
#include <algorithm>
#include <array>
#include <cstring>

namespace csystem
{
namespace
{
bool signature(std::span<const uint8_t> bytes, size_t offset, const char* text)
{
    return offset <= bytes.size() && bytes.size() - offset >= 4 &&
           std::memcmp(bytes.data() + offset, text, 4) == 0;
}

void transform_header(Bytes& data)
{
    static constexpr char key[] = "DBB3206F-F171-4885-A131-EC7FBA6FF491 Copyright 2004 Cyberworks "
                                  "\"TinkerBell\"., all rights reserved.";

    if (data.size() < 3615)
    {
        throw Error("C,system audio requires at least 3615 bytes for its header transform");
    }

    size_t key_index = 0;
    for (size_t offset = 4; offset < 3615; ++offset)
    {
        data[offset] ^= uint8_t(key[key_index]);
        if (key_index == sizeof(key) - 1)
        {
            key_index = 0;
        }
        ++key_index;
    }
}

uint32_t page_crc(std::span<const uint8_t> page)
{
    uint32_t crc = 0;
    for (size_t index = 0; index < page.size(); ++index)
    {
        const auto byte = index >= 22 && index < 26 ? 0 : page[index];
        crc ^= uint32_t(byte) << 24;
        for (int bit = 0; bit < 8; ++bit)
        {
            crc = crc & 0x80000000 ? (crc << 1) ^ 0x04c11db7 : crc << 1;
        }
    }
    return crc;
}
}

size_t validate_ogg(std::span<const uint8_t> data)
{
    size_t offset = 0;
    size_t pages = 0;
    while (signature(data, offset, "OggS"))
    {
        if (data.size() - offset < 27 || data[offset + 4] != 0)
        {
            throw Error("invalid Ogg page header");
        }
        const auto segments = data[offset + 26];
        size_t length = 27 + segments;
        if (length > data.size() - offset)
        {
            throw Error("truncated Ogg segment table");
        }
        for (size_t index = 0; index < segments; ++index)
        {
            length += data[offset + 27 + index];
        }
        if (length > data.size() - offset)
        {
            throw Error("truncated Ogg page body");
        }
        Cursor checksum{data.subspan(offset + 22, 4)};
        if (checksum.u32() != page_crc(data.subspan(offset, length)))
        {
            throw Error("Ogg page checksum mismatch");
        }
        if (pages == 0)
        {
            const auto packet_offset = offset + 27 + segments;
            if (length < 27 + size_t(segments) + 7 || data[packet_offset] != 1 ||
                std::memcmp(data.data() + packet_offset + 1, "vorbis", 6) != 0)
            {
                throw Error("engine audio requires Ogg Vorbis");
            }
        }
        offset += length;
        ++pages;
    }
    if (pages == 0)
    {
        throw Error("no Ogg Vorbis pages found");
    }
    return offset;
}

AudioFile decode_audio(std::span<const uint8_t> payload)
{
    AudioFile audio;
    size_t offset = 0;
    if (!signature(payload, 0, "Tink") && !signature(payload, 0, "OggS"))
    {
        if (!signature(payload, 12, "Tink") && !signature(payload, 12, "OggS"))
        {
            throw Error("unrecognized C,system audio payload");
        }
        offset = 12;
        audio.prefix.assign(payload.begin(), payload.begin() + 12);
    }
    audio.ogg.assign(payload.begin() + offset, payload.end());
    audio.obfuscated = signature(audio.ogg, 0, "Tink");
    if (audio.obfuscated)
    {
        transform_header(audio.ogg);
        std::copy_n("OggS", 4, audio.ogg.begin());
    }
    const auto length = validate_ogg(audio.ogg);
    audio.suffix.assign(audio.ogg.begin() + length, audio.ogg.end());
    audio.ogg.resize(length);
    return audio;
}

void validate_wave(std::span<const uint8_t> data)
{
    if (!signature(data, 0, "RIFF") || !signature(data, 8, "WAVE") || data.size() < 12)
    {
        throw Error("replacement must be a RIFF WAVE file");
    }
    Cursor reader{data};
    reader.take(4);
    if (uint64_t(reader.u32()) + 8 != data.size())
    {
        throw Error("RIFF WAVE size mismatch");
    }
    reader.take(4);
    bool format = false;
    bool samples = false;
    uint16_t alignment = 0;
    while (!reader.done())
    {
        const auto id = reader.take(4);
        const auto size = reader.u32();
        const auto chunk = reader.take(size);
        if (size & 1)
        {
            reader.u8();
        }
        if (signature(id, 0, "fmt "))
        {
            Cursor fields{chunk};
            const auto encoding = fields.u16();
            const auto channels = fields.u16();
            const auto rate = fields.u32();
            const auto byte_rate = fields.u32();
            alignment = fields.u16();
            const auto bits = fields.u16();
            if (format || encoding != 1 || !channels || !rate || (bits != 8 && bits != 16) ||
                alignment != uint32_t(channels) * bits / 8 ||
                byte_rate != uint64_t(rate) * alignment)
            {
                throw Error("engine WAVE audio requires valid 8-bit or 16-bit PCM");
            }
            format = true;
        }
        else if (signature(id, 0, "data"))
        {
            if (!format || samples || !size || size % alignment)
            {
                throw Error("invalid PCM WAVE sample data");
            }
            samples = true;
        }
    }
    if (!format || !samples)
    {
        throw Error("WAVE format or sample data is missing");
    }
}

Bytes encode_audio(std::span<const uint8_t> ogg, const AudioFile& original)
{
    if (validate_ogg(ogg) != ogg.size())
    {
        throw Error("replacement Ogg contains trailing data");
    }
    Bytes transformed(ogg.begin(), ogg.end());
    append(transformed, original.suffix);
    if (original.obfuscated)
    {
        transform_header(transformed);
        std::copy_n("Tink", 4, transformed.begin());
    }
    Bytes result = original.prefix;
    append(result, transformed);
    return result;
}
}
