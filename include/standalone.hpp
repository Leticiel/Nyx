#pragma once

#include "tool.hpp"

namespace csystem
{
struct ArchivePosition
{
    uint8_t part = 0;
    uint64_t offset = 0;
};

ArchivePosition reserve_archive_space(ArchivePosition& cursor, uint8_t source_part, uint64_t size);
void export_build_metadata(const fs::path& game, const fs::path& project, Json& manifest);
Json build_standalone_project(const fs::path& project, const fs::path& destination);
}
