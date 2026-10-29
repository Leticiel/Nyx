#pragma once
#include "core.hpp"

namespace csystem
{
struct AudioFile
{
    Bytes ogg;
    Bytes prefix;
    Bytes suffix;
    bool obfuscated = false;
};

AudioFile decode_audio(std::span<const uint8_t> payload);
Bytes encode_audio(std::span<const uint8_t> ogg, const AudioFile& original);
size_t validate_ogg(std::span<const uint8_t> data);
void validate_wave(std::span<const uint8_t> data);
}
