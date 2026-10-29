#include "tool.hpp"
#include <algorithm>

namespace csystem
{
std::vector<ArchivePair> game_archives(const fs::path& game)
{
    std::vector<ArchivePair> pairs = {{"scripts", "Arc01.dat", "Arc04.dat"},
                                      {"images", "Arc02.dat", "Arc05.dat"},
                                      {"audio", "Arc03.dat", "Arc06.dat"}};
    const bool index = fs::is_regular_file(game / "Arc07.dat");
    const bool content = fs::is_regular_file(game / "Arc08.dat");
    if (index != content)
        throw Error("Arc07.dat and Arc08.dat must both be present");
    if (index) pairs.push_back({"videos", "Arc07.dat", "Arc08.dat"});
    return pairs;
}

void transform_archive_video(Bytes& bytes)
{
    // Native y0 decoder: offsets are relative to each movie. XOR is its own inverse.
    size_t offset = 0x800;
    uint8_t key = 1;
    while (offset < bytes.size())
    {
        bytes[offset] ^= key;
        if (++key == 0x80) key = 1;
        offset += key;
    }
}

std::string archive_video_extension(std::span<const uint8_t> bytes)
{
    auto has = [&](size_t offset, const std::string& magic)
    {
        return offset <= bytes.size() && magic.size() <= bytes.size() - offset &&
               std::equal(magic.begin(), magic.end(), bytes.begin() + offset);
    };
    if (has(0, "RIFF") && has(8, "AVI ")) return ".avi";
    if (bytes.size() >= 4 && bytes[0] == 0 && bytes[1] == 0 &&
        bytes[2] == 1 && bytes[3] == 0xba) return ".mpg";
    return ".bin";
}
}
