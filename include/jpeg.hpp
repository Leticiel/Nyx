#pragma once
#include "core.hpp"
#include <optional>

namespace csystem
{
// JPEG data remains byte-for-byte unchanged; the engine wrapper is rebuilt.
std::optional<std::span<const uint8_t>> unwrap_jpeg(std::span<const uint8_t>);
Bytes wrap_jpeg(std::span<const uint8_t>);
}
