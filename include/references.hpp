#pragma once

#include "tool.hpp"

namespace csystem
{
struct AssetReferences
{
    ResourceResolver resolve;
    ResourcePathLookup path;
};

AssetReferences asset_references(const fs::path& project, const Json& manifest);
}
