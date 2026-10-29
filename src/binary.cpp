#include "tool.hpp"
#include "legacy.hpp"
#include <algorithm>
#include <array>
#include <windows.h>
#include <bcrypt.h>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>

namespace csystem
{
namespace
{
using ReadHandle = std::unique_ptr<void, decltype(&CloseHandle)>;

Error file_io_error(const fs::path& path, const char* operation, DWORD code)
{
    const auto path_bytes = path.u8string();
    wchar_t message[512]{};
    const auto length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                      nullptr, code, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
                                      message, 512, nullptr);
    std::u16string detail(message, message + length);
    while (!detail.empty() && (detail.back() == u'\r' || detail.back() == u'\n'))
    {
        detail.pop_back();
    }
    const bool locked = code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION;
    return Error(std::string(operation) + " " + std::string(path_bytes.begin(), path_bytes.end()) +
                 " (Windows error " + std::to_string(code) +
                 (detail.empty() ? "" : ": " + utf8(detail)) + ")" +
                 (locked ? ". Close the game or other application using this file and try again."
                         : ""));
}

ReadHandle open_shared_file(const fs::path& path)
{
    for (unsigned attempt = 0; ; ++attempt)
    {
        const auto file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            return ReadHandle(file, CloseHandle);
        }
        const auto code = GetLastError();
        if (attempt == 4 || (code != ERROR_SHARING_VIOLATION && code != ERROR_LOCK_VIOLATION))
        {
            throw file_io_error(path, "cannot open", code);
        }
        Sleep(50 * (attempt + 1));
    }
}
}

std::span<const uint8_t> Cursor::take(size_t n)
{
    if (n > data.size() - pos)
    {
        throw Error("truncated binary at " + std::to_string(pos));
    }
    auto b = data.subspan(pos, n);
    pos += n;
    return b;
}

uint8_t Cursor::u8()
{
    return take(1)[0];
}

uint16_t Cursor::u16()
{
    auto b = take(2);
    return b[0] | uint16_t(b[1]) << 8;
}

uint32_t Cursor::u32()
{
    auto b = take(4);
    return uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24;
}

uint64_t Cursor::u64()
{
    auto lo = u32();
    return lo | (uint64_t(u32()) << 32);
}

uint32_t Cursor::count(size_t min)
{
    auto n = u32();
    if (n > (data.size() - pos) / min)
    {
        throw Error("invalid binary count");
    }
    return n;
}

void Cursor::finish() const
{
    if (!done())
    {
        throw Error("unconsumed binary data at " + std::to_string(pos));
    }
}

uint32_t checked32(uint64_t n)
{
    if (n > UINT32_MAX)
    {
        throw Error("32-bit field overflow");
    }
    return uint32_t(n);
}

void put(Bytes& b, uint64_t n, size_t width)
{
    for (size_t i = 0; i < width; i++)
    {
        b.push_back(uint8_t(n >> (i * 8)));
    }
}

void patch(Bytes& b, size_t at, uint64_t n, size_t width)
{
    if (at > b.size() || width > b.size() - at)
    {
        throw Error("invalid patch range");
    }
    for (size_t i = 0; i < width; i++)
    {
        b[at + i] = uint8_t(n >> (i * 8));
    }
}

void append(Bytes& b, std::span<const uint8_t> a)
{
    b.insert(b.end(), a.begin(), a.end());
}

void write_file(const fs::path& p, std::span<const uint8_t> b)
{
    if (!p.parent_path().empty())
    {
        fs::create_directories(p.parent_path());
    }
    std::ofstream f(p, std::ios::binary);
    if (!f || !f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size())))
    {
        throw Error("cannot write " + p.string());
    }
}

void write_text(const fs::path& p, const std::string& s)
{
    write_file(p, {reinterpret_cast<const uint8_t*>(s.data()), s.size()});
}

std::string read_text(const fs::path& p)
{
    auto b = read_file(p);
    size_t at = b.size() >= 3 && b[0] == 239 && b[1] == 187 && b[2] == 191 ? 3 : 0;
    return {b.begin() + at, b.end()};
}

void validate_output_directory(const fs::path& source, const fs::path& output)
{
    const auto input = fs::weakly_canonical(fs::absolute(source));
    const auto target = fs::weakly_canonical(fs::absolute(output));
    auto target_part = target.begin();
    auto input_part = input.begin();
    while (target_part != target.end() && input_part != input.end() &&
           _wcsicmp(target_part->c_str(), input_part->c_str()) == 0)
    {
        ++target_part;
        ++input_part;
    }
    if (output.empty() || target == target.root_path() || target_part == target.end())
    {
        throw Error("output cannot be the source directory, an ancestor of it, or a filesystem root");
    }
    const auto attributes = GetFileAttributesW(fs::absolute(output).c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
    {
        throw Error("output directory cannot be a symbolic link or junction");
    }
    if (fs::exists(output) && !fs::is_directory(output))
    {
        throw Error("output path is not a directory");
    }
}

void remove_output_directory(const fs::path& source, const fs::path& output)
{
    validate_output_directory(source, output);
    fs::remove_all(output);
}

std::string utf8(const std::u16string& s)
{
    if (s.empty())
    {
        return {};
    }
    auto w = reinterpret_cast<const wchar_t*>(s.data());
    int n = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, w, int(s.size()), nullptr, 0, nullptr, nullptr);
    if (!n)
    {
        throw Error("invalid UTF-16");
    }
    std::string r(n, 0);
    WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, w, int(s.size()), r.data(), n, nullptr, nullptr);
    return r;
}

std::u16string utf16(const std::string& s)
{
    if (s.empty())
    {
        return {};
    }
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), nullptr, 0);
    if (!n)
    {
        throw Error("invalid UTF-8");
    }
    std::u16string r(n, 0);
    MultiByteToWideChar(CP_UTF8,
                        MB_ERR_INVALID_CHARS,
                        s.data(),
                        int(s.size()),
                        reinterpret_cast<wchar_t*>(r.data()),
                        n);
    return r;
}

std::string hex(std::span<const uint8_t> b)
{
    static const char* h = "0123456789abcdef";
    std::string r;
    r.reserve(b.size() * 2);
    for (auto v : b)
    {
        r += h[v >> 4];
        r += h[v & 15];
    }
    return r;
}

Bytes unhex(const std::string& s)
{
    if (s.size() % 2)
    {
        throw Error("odd hex length");
    }
    Bytes b;
    auto nib = [](char c) -> int
    {
        if (c >= '0' && c <= '9')
        {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f')
        {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F')
        {
            return c - 'A' + 10;
        }
        throw Error("invalid hex");
    };
    for (size_t i = 0; i < s.size(); i += 2)
    {
        b.push_back(uint8_t(nib(s[i]) * 16 + nib(s[i + 1])));
    }
    return b;
}

struct Hash
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    Bytes object;

    Hash()
    {
        DWORD n = 0, got = 0;
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
            BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&n), 4, &got, 0) <
                0)
        {
            throw Error("SHA256 init failed");
        }
        object.resize(n);
        if (BCryptCreateHash(alg, &h, object.data(), n, nullptr, 0, 0) < 0)
        {
            throw Error("SHA256 create failed");
        }
    }

    ~Hash()
    {
        if (h)
        {
            BCryptDestroyHash(h);
        }
        if (alg)
        {
            BCryptCloseAlgorithmProvider(alg, 0);
        }
    }

    void add(std::span<const uint8_t> b)
    {
        if (BCryptHashData(h, const_cast<PUCHAR>(b.data()), checked32(b.size()), 0) < 0)
        {
            throw Error("SHA256 update failed");
        }
    }

    std::string finish()
    {
        std::array<uint8_t, 32> b{};
        if (BCryptFinishHash(h, b.data(), 32, 0) < 0)
        {
            throw Error("SHA256 finish failed");
        }
        return hex(b);
    }
};

std::string sha256(std::span<const uint8_t> b)
{
    Hash h;
    h.add(b);
    return h.finish();
}

std::string file_hash(const fs::path& p)
{
    const auto file = open_shared_file(p);
    Hash h;
    std::array<uint8_t, 1024 * 1024> b;
    for (;;)
    {
        DWORD received = 0;
        if (!ReadFile(file.get(), b.data(), DWORD(b.size()), &received, nullptr))
        {
            throw file_io_error(p, "cannot hash", GetLastError());
        }
        if (!received)
        {
            break;
        }
        h.add({b.data(), size_t(received)});
    }
    return h.finish();
}

std::string padded(uint64_t n, int width)
{
    std::ostringstream s;
    s << std::setfill('0') << std::setw(width) << n;
    return s.str();
}

uint32_t read_decimal(Cursor& r)
{
    uint32_t v = 0;
    for (int i = 0; i < 8; i++)
    {
        auto b = r.u8();
        unsigned d = b == 255 ? 0 : b ^ 127;
        if (d > 9)
        {
            throw Error("invalid decimal header");
        }
        v = v * 10 + d;
    }
    return v;
}

void write_decimal(Bytes& b, uint32_t v)
{
    if (v > 99999999)
    {
        throw Error("decimal field overflow");
    }
    for (char c : padded(v, 8))
    {
        b.push_back(c == '0' ? 255 : uint8_t((c - '0') ^ 127));
    }
}

Bytes lzss_encode(std::span<const uint8_t> b)
{
    Bytes r;
    r.reserve(b.size() + b.size() / 8 + 1);
    constexpr size_t window = 4096;
    constexpr size_t absent = std::numeric_limits<size_t>::max();
    std::array<size_t, 65536> heads;
    std::array<size_t, window> previous;
    heads.fill(absent);
    previous.fill(absent);
    const auto hash = [&](size_t p)
    {
        return ((uint32_t(b[p]) * 251 + b[p + 1]) * 251 + b[p + 2]) & 65535;
    };
    for (size_t p = 0; p < b.size();)
    {
        const auto flags = r.size();
        r.push_back(0);
        for (unsigned bit = 0; bit < 8 && p < b.size(); ++bit)
        {
            size_t length = 1, source = 0;
            const auto limit = std::min(size_t(18), b.size() - p);
            if (limit >= 3)
            {
                auto candidate = heads[hash(p)];
                for (unsigned attempts = 0;
                     attempts < 64 && candidate != absent && p - candidate <= window;
                     ++attempts)
                {
                    size_t matched = 0;
                    while (matched < limit && b[candidate + matched] == b[p + matched])
                    {
                        ++matched;
                    }
                    if (matched >= 3 && matched > length)
                    {
                        length = matched;
                        source = (0xfee + candidate) & 0xfff;
                        if (length == limit)
                        {
                            break;
                        }
                    }
                    candidate = previous[candidate & (window - 1)];
                }
            }
            if (length >= 3)
            {
                r.push_back(uint8_t(source));
                r.push_back(uint8_t(((source >> 4) & 0xf0) | (length - 3)));
            }
            else
            {
                r[flags] |= uint8_t(1u << bit);
                r.push_back(b[p]);
            }
            for (size_t end = p + length; p < end; ++p)
            {
                if (b.size() - p >= 3)
                {
                    const auto key = hash(p);
                    previous[p & (window - 1)] = heads[key];
                    heads[key] = p;
                }
            }
        }
    }
    // Arc entries use unequal size fields to distinguish LZSS from raw storage.
    if (!b.empty() && r.size() == b.size())
    {
        r.clear();
        for (size_t p = 0; p < b.size(); p += 8)
        {
            const auto n = std::min(size_t(8), b.size() - p);
            r.push_back(uint8_t((1u << n) - 1));
            append(r, b.subspan(p, n));
        }
    }
    return r;
}

Bytes index_decode(std::span<const uint8_t> b)
{
    Cursor c{b};
    auto n = read_decimal(c), p = read_decimal(c);
    auto r = lzss_decode(c.take(p), n);
    c.finish();
    return r;
}

Bytes index_encode(std::span<const uint8_t> b)
{
    auto p = lzss_encode(b);
    Bytes r;
    write_decimal(r, checked32(b.size()));
    write_decimal(r, checked32(p.size()));
    append(r, p);
    return r;
}

namespace
{
size_t archive_record_size(std::span<const uint8_t> raw)
{
    if (raw.empty())
    {
        return 27;
    }
    Cursor reader{raw};
    const auto version = reader.u32();
    if (version != 22 && version != 23)
    {
        throw Error("unsupported archive index version: " + std::to_string(version));
    }
    const size_t stride = version == 22 ? 26 : 27;
    if (raw.size() % stride)
    {
        throw Error("truncated archive index record for version " + std::to_string(version));
    }
    return stride;
}

bool valid_archive_ranges(const std::vector<Entry>& entries,
                          const std::map<uint8_t, uint64_t>& content_sizes)
{
    std::map<uint8_t, std::vector<std::pair<uint64_t, uint64_t>>> parts;
    for (const auto& entry : entries)
    {
        if (index_only_archive_entry(entry))
        {
            // Synthetic v23 bitmap slots are not allocations. Their part byte
            // stays zero even after real images move to Arc05a, and a slot at
            // a part boundary can deliberately point past Arc05 EOF.
            if (entry.size > 32767 || entry.size % 3 ||
                entry.packed_size != entry.size * 4 / 7 ||
                entry.reserved != -1 || entry.part != 0)
                return false;
            continue;
        }
        // Newer v23 archives also use size=0 for a nonempty stored payload.
        if ((entry.size && !entry.packed_size) ||
            (entry.version != 23 && !entry.size && entry.packed_size))
        {
            return false;
        }
        const uint64_t prefix = archive_entry_prefix_size(entry);
        const uint64_t end = uint64_t(entry.offset) + entry.packed_size + prefix;
        if (!content_sizes.empty() &&
            (!content_sizes.contains(entry.part) || end > content_sizes.at(entry.part)))
        {
            return false;
        }
        if (entry.leading.size() > entry.offset) return false;
        if (end != entry.offset && !opaque_archive_entry(entry))
        {
            parts[entry.part].emplace_back(entry.offset - entry.leading.size(), end);
        }
    }
    for (auto& [part, ranges] : parts)
    {
        std::sort(ranges.begin(), ranges.end());
        for (size_t index = 1; index < ranges.size(); ++index)
        {
            if (ranges[index].first < ranges[index - 1].second)
            {
                return false;
            }
        }
    }
    return true;
}
}

std::string archive_index_layout_name(ArchiveIndexLayout layout)
{
    switch (layout)
    {
    case ArchiveIndexLayout::SizePackedOffset:
        return "size,packed_size,offset";
    case ArchiveIndexLayout::SizeOffsetPacked:
        return "size,offset,packed_size";
    }
    throw Error("unsupported archive index layout");
}

ArchiveIndexLayout parse_archive_index_layout(const std::string& name)
{
    for (const auto layout :
         {ArchiveIndexLayout::SizePackedOffset, ArchiveIndexLayout::SizeOffsetPacked})
    {
        if (name == archive_index_layout_name(layout))
        {
            return layout;
        }
    }
    throw Error("unsupported archive index layout: " + name);
}

ArchiveIndex decode_arc_index(std::span<const uint8_t> b, std::optional<uint64_t> content_size)
{
    return decode_arc_index(b,
                            content_size ? std::map<uint8_t, uint64_t>{{uint8_t{0}, *content_size}}
                                         : std::map<uint8_t, uint64_t>{});
}

namespace
{
ArchiveIndex decode_index_storage(std::span<const uint8_t> b,
                                 const std::map<uint8_t, uint64_t>& content_sizes,
                                 const std::function<void(std::vector<Entry>&)>& infer_storage)
{
    auto raw = index_decode(b);
    const auto stride = archive_record_size(raw);
    Cursor r{raw};
    std::vector<Entry> es;
    std::set<uint32_t> ids;
    while (!r.done())
    {
        Entry e;
        e.version = r.u32();
        e.id = r.u32();
        e.size = r.u32();
        e.packed_size = r.u32();
        e.offset = r.u32();
        e.kind = char(r.u8());
        e.subtype = char(r.u8());
        e.reserved = int32_t(r.u32());
        e.part = stride == 27 ? r.u8() : 0;
        if (e.version != (stride == 26 ? 22u : 23u) || e.part > 26 || !ids.insert(e.id).second)
        {
            throw Error("unsupported or duplicate archive entry");
        }
        es.push_back(e);
    }
    auto swapped = es;
    bool identical = true;
    for (auto& entry : swapped)
    {
        identical = identical && entry.offset == entry.packed_size;
        std::swap(entry.offset, entry.packed_size);
    }
    if (infer_storage)
    {
        infer_storage(es);
        infer_storage(swapped);
    }
    const bool standard_valid = valid_archive_ranges(es, content_sizes);
    const bool swapped_valid = valid_archive_ranges(swapped, content_sizes);
    if (standard_valid && swapped_valid && !identical)
    {
        throw Error("ambiguous archive index layout: both field orders have valid ranges");
    }
    if (standard_valid)
    {
        return {ArchiveIndexLayout::SizePackedOffset, std::move(es)};
    }
    if (swapped_valid)
    {
        return {ArchiveIndexLayout::SizeOffsetPacked, std::move(swapped)};
    }
    throw Error("invalid archive index: neither field order has valid non-overlapping ranges");
}
}

ArchiveIndex decode_arc_index(std::span<const uint8_t> b,
                              const std::map<uint8_t, uint64_t>& content_sizes)
{
    return decode_index_storage(b, content_sizes, {});
}

ArchiveIndex read_arc_index(const fs::path& index, const fs::path& content)
{
    try
    {
        const auto bytes = read_file(index);
        const auto raw = index_decode(bytes);
        const auto stride = archive_record_size(raw);
        std::map<uint8_t, uint64_t> sizes;
        for (size_t position = 0; position < raw.size(); position += stride)
        {
            const uint8_t part = stride == 27 ? raw[position + 26] : 0;
            if (!sizes.contains(part))
            {
                const auto path = archive_part_path(content, part);
                if (!fs::is_regular_file(path))
                {
                    throw Error("missing content part: " + path.filename().string());
                }
                sizes[part] = fs::file_size(path);
            }
        }
        return decode_index_storage(bytes, sizes, [&](std::vector<Entry>& entries)
        {
            std::map<uint8_t, ReadHandle> handles;
            for (auto& entry : entries)
            {
                if (entry.subtype != '0' || std::string_view("jku").find(entry.kind) == std::string_view::npos ||
                    entry.size != entry.packed_size || entry.packed_size < 16 ||
                    uint64_t(entry.offset) + entry.packed_size > sizes.at(entry.part))
                    continue;
                if (!handles.contains(entry.part))
                    handles.emplace(entry.part, open_shared_file(archive_part_path(content, entry.part)));
                const auto preceding = std::min(entry.offset, 44u);
                LARGE_INTEGER position{};
                position.QuadPart = entry.offset - preceding;
                std::array<uint8_t, 60> probe{};
                DWORD received = 0;
                if (!SetFilePointerEx(handles.at(entry.part).get(), position, nullptr, FILE_BEGIN) ||
                    !ReadFile(handles.at(entry.part).get(), probe.data(), preceding + 16, &received, nullptr) ||
                    received != preceding + 16)
                    throw file_io_error(content, "cannot inspect audio storage", GetLastError());
                const auto signature = [&](size_t offset, const char* text)
                { return std::equal(probe.begin() + offset, probe.begin() + offset + 4, text); };
                if (signature(preceding, "Tink") || signature(preceding, "OggS"))
                {
                    entry.audio_prefix_size = 0;
                    if (preceding == 44 && signature(0, "RIFF") && signature(8, "WAVE") &&
                        signature(12, "fmt ") && probe[16] == 16 && probe[17] == 0 &&
                        probe[18] == 0 && probe[19] == 0 && signature(36, "data"))
                        entry.leading.assign(probe.begin(), probe.begin() + 44);
                }
                else if (entry.version == 23 && entry.kind == 'u' &&
                         (signature(preceding + 12, "Tink") || signature(preceding + 12, "OggS")))
                    entry.audio_prefix_size = 12;
            }
        });
    }
    catch (const Error& error)
    {
        throw Error(index.filename().string() + ": " + error.what());
    }
}

fs::path archive_part_path(const fs::path& content, uint8_t part)
{
    if (part > 26)
    {
        throw Error("archive part must be between 0 and 26");
    }
    if (!part)
    {
        return content;
    }
    return content.parent_path() /
           (content.stem().wstring() + wchar_t(L'a' + part - 1) + content.extension().wstring());
}

std::vector<Entry> arc_index(std::span<const uint8_t> bytes)
{
    return decode_arc_index(bytes).entries;
}

Bytes encode_arc_index(const std::vector<Entry>& entries, ArchiveIndexLayout layout)
{
    archive_index_layout_name(layout);
    Bytes raw;
    std::set<uint32_t> ids;
    for (const auto& entry : entries)
    {
        if ((entry.version != 22 && entry.version != 23) ||
            entry.version != entries.front().version || entry.part > 26 ||
            (entry.version == 22 && entry.part != 0) || !ids.insert(entry.id).second)
        {
            throw Error("unsupported or duplicate archive entry");
        }
        put(raw, entry.version);
        put(raw, entry.id);
        put(raw, entry.size);
        if (layout == ArchiveIndexLayout::SizeOffsetPacked)
        {
            put(raw, entry.offset);
            put(raw, entry.packed_size);
        }
        else
        {
            put(raw, entry.packed_size);
            put(raw, entry.offset);
        }
        put(raw, uint8_t(entry.kind), 1);
        put(raw, uint8_t(entry.subtype), 1);
        put(raw, uint32_t(entry.reserved));
        if (entry.version == 23)
        {
            put(raw, entry.part, 1);
        }
    }
    if (!valid_archive_ranges(entries, {}))
    {
        throw Error("invalid archive entry ranges");
    }
    return index_encode(raw);
}

Bytes read_range(const fs::path& p, uint64_t at, size_t n)
{
    const auto path_bytes = p.u8string();
    const std::string path(path_bytes.begin(), path_bytes.end());
    const auto file = open_shared_file(p);
    LARGE_INTEGER file_size{};
    if (!GetFileSizeEx(file.get(), &file_size))
    {
        throw file_io_error(p, "cannot inspect", GetLastError());
    }
    const auto size = uint64_t(file_size.QuadPart);
    if (at > size || n > size - at)
    {
        throw Error("archive range outside file: " + path + " (offset " + std::to_string(at) +
                    ", size " + std::to_string(n) + ", file size " + std::to_string(size) + ")");
    }
    LARGE_INTEGER position{};
    position.QuadPart = LONGLONG(at);
    if (!SetFilePointerEx(file.get(), position, nullptr, FILE_BEGIN))
    {
        throw file_io_error(p, "cannot seek", GetLastError());
    }
    Bytes b(n);
    for (size_t done = 0; done < n; )
    {
        const auto requested = DWORD(std::min(n - done, size_t(1024 * 1024)));
        DWORD received = 0;
        if (!ReadFile(file.get(), b.data() + done, requested, &received, nullptr))
        {
            throw file_io_error(p, "cannot read", GetLastError());
        }
        if (!received)
        {
            throw Error("archive read ended early: " + path + " (offset " +
                        std::to_string(at + done) + ")");
        }
        done += received;
    }
    return b;
}

size_t archive_entry_prefix_size(const Entry& entry)
{
    // v22 indexes the entire stream. Most v23 u0 entries add three loop
    // words outside both sizes; file-based reading identifies direct streams.
    if (entry.audio_prefix_size != -1)
    {
        if (entry.audio_prefix_size != 0 && entry.audio_prefix_size != 12)
            throw Error("unsupported audio prefix size");
        return size_t(entry.audio_prefix_size);
    }
    return entry.version == 23 && entry.kind == 'u' && entry.subtype == '0' ? 12 : 0;
}

Bytes entry_payload(const Entry& e, const fs::path& p)
{
    if (index_only_archive_entry(e)) throw Error("synthetic bitmap index slot has no independent payload");
    const size_t prefix_size = archive_entry_prefix_size(e);
    if (prefix_size && archive_entry_compression(e) == "lzss")
    {
        throw Error("compressed music entries are not supported by this format profile");
    }
    auto b =
        read_range(archive_part_path(p, e.part), e.offset, size_t(e.packed_size) + prefix_size);
    return archive_entry_compression(e, b) == "lzss" ? lzss_decode(b, e.size) : b;
}

bool opaque_archive_entry(const Entry& entry)
{
    return entry.version == 22 && entry.kind == 'b' && entry.subtype == '\0';
}

bool index_only_archive_entry(const Entry& entry)
{
    return entry.version == 23 && entry.kind == 'b' && entry.subtype == '\0';
}

std::string archive_entry_compression(const Entry& entry, std::span<const uint8_t> stored)
{
    if (opaque_archive_entry(entry)) return "none";
    if (entry.version == 23 && !entry.size && entry.packed_size) return "none_zero_size";
    // GYK entry 12917 is LZSS even though both indexed lengths are 1,449.
    // Require a complete, valid GYK bitmap after decoding; never guess for opaque
    // b/NUL records, arbitrary raw data, or a truncated compressed stream.
    if (entry.version == 22 && entry.subtype == '0' &&
        std::string_view("bno").find(entry.kind) != std::string_view::npos &&
        entry.size == entry.packed_size && stored.size() == entry.packed_size &&
        stored.size() >= 3 && stored[0] == 255 && stored[1] == 'a' && stored[2] == 239)
    {
        try
        {
            if (gyk8_image_attributes(lzss_decode(stored, entry.size))) return "lzss";
        }
        catch (const Error&) {}
    }
    return entry.size == entry.packed_size ? "none" : "lzss";
}

Json decode_config(std::span<const uint8_t> b)
{
    Cursor r{b};
    auto n = read_decimal(r), p = read_decimal(r);
    auto raw = lzss_decode(r.take(p), n);
    if (raw.size() < 5)
    {
        throw Error("truncated config strings");
    }
    bool legacy = raw[4] != 'S';
    if (!legacy)
    {
        try
        {
            Cursor probe{raw};
            probe.take(probe.u32());
            const auto settings = probe.take(probe.u32());
            legacy = !settings.empty() && settings.front() == '#';
        }
        catch (const Error&)
        {
        }
    }
    if (legacy)
    {
        return decode_legacy_config(raw, r.take(b.size() - r.pos));
    }
    Cursor s{raw};
    Json strings = Json::array();
    while (!s.done())
    {
        auto len = s.u32();
        if (s.u8() != 'S')
        {
            throw Error("unknown config string marker");
        }
        auto data = s.take(len);
        Json item = {{"hex", hex(data)}};
        if (len % 2 == 0)
        {
            std::u16string t;
            Cursor tr{data};
            while (!tr.done())
            {
                t += char16_t(tr.u16());
            }
            try
            {
                item["text"] = utf8(t);
            }
            catch (const Error&)
            {
            }
        }
        strings.push_back(item);
    }
    Json blocks = Json::array();
    while (!r.done())
    {
        auto size = read_decimal(r);
        blocks.push_back(hex(r.take(size)));
    }
    Json j = {{"format", "csystem-config-1"}, {"strings", strings}, {"blocks", blocks}};
    if (strings.size() > 1 && strings[1].value("text", "").starts_with('#'))
    {
        j["settings"] = decode_compact_settings(unhex(strings[1].at("hex")), DataFormat::Utf16);
        j["settings_storage"] = "inline_utf16";
        j["strings"].erase(j["strings"].begin() + 1);
        j["blocks"].insert(j["blocks"].begin(), "");
        return j;
    }
    if (!blocks.empty())
    {
        auto first = unhex(blocks[0]);
        Cursor d{first};
        auto count = d.u32();
        if (first.size() == 4 + uint64_t(count) * 20)
        {
            Json settings = Json::array();
            for (uint32_t i = 0; i < count; i++)
            {
                auto bytes = d.take(16);
                Cursor k{bytes};
                std::u16string key;
                while (!k.done())
                {
                    auto w = k.u16();
                    if (!w)
                    {
                        break;
                    }
                    key += char16_t(w);
                }
                settings.push_back({{"key", utf8(key)}, {"value", int32_t(d.u32())}});
            }
            j["settings"] = settings;
        }
    }
    if (!j.contains("settings"))
    {
        throw Error("unsupported Arc00.dat settings layout");
    }
    return j;
}

Bytes encode_config(const Json& j)
{
    if (j.at("format") != "csystem-config-1")
    {
        throw Error("unknown config format");
    }
    if (parse_data_format(j.value("data_format", "utf16")) == DataFormat::Cp932)
    {
        return encode_legacy_config(j);
    }
    const auto storage = j.value("settings_storage", "block_utf16");
    if (storage != "block_utf16" && storage != "inline_utf16")
    {
        throw Error("unsupported Arc00.dat settings storage: " + storage);
    }
    const bool inline_settings = storage == "inline_utf16";
    Bytes raw;
    const auto write_string = [&](std::span<const uint8_t> bytes)
    {
        put(raw, checked32(bytes.size()));
        put(raw, 'S', 1);
        append(raw, bytes);
    };
    bool first_string = true;
    for (const auto& i : j.at("strings"))
    {
        Bytes b;
        if (i.contains("text"))
        {
            for (char16_t c : utf16(i.at("text")))
            {
                put(b, c, 2);
            }
        }
        else
        {
            b = unhex(i.at("hex"));
        }
        write_string(b);
        if (inline_settings && first_string)
        {
            write_string(encode_compact_settings(j.at("settings"), DataFormat::Utf16));
        }
        first_string = false;
    }
    if (inline_settings && first_string)
    {
        throw Error("inline config settings require a title string");
    }
    auto out = index_encode(raw);
    auto blocks = j.at("blocks");
    if (!inline_settings && j.contains("settings"))
    {
        auto old = unhex(blocks.at(0));
        auto settings = j.at("settings");
        Bytes b;
        put(b, checked32(settings.size()));
        for (size_t i = 0; i < settings.size(); i++)
        {
            auto key = utf16(settings[i].at("key"));
            if (key.size() > 8)
            {
                throw Error("config key too long");
            }
            Bytes kb;
            for (auto c : key)
            {
                put(kb, c, 2);
            }
            kb.resize(16);
            append(b, kb);
            auto v = settings[i].at("value").get<int64_t>();
            if (v < INT32_MIN || v > INT32_MAX)
            {
                throw Error("config value overflow");
            }
            put(b, uint32_t(v));
        }
        blocks[0] = hex(b);
    }
    for (size_t index = inline_settings ? 1 : 0; index < blocks.size(); ++index)
    {
        auto b = unhex(blocks[index]);
        write_decimal(out, checked32(b.size()));
        append(out, b);
    }
    return out;
}

Resources::Resources(const fs::path& d) : directory(d)
{
    if (!fs::exists(d))
    {
        index = Bytes(4, 0);
        return;
    }
    present = true;
    index = read_file(d / "Data00.dat");
    Cursor r{index};
    auto count = r.count(12);
    if (count > 215)
    {
        throw Error("too many resource parts");
    }
    for (unsigned p = 0; p < count; p++)
    {
        ResourcePart part{p, {}};
        auto n = r.count(16);
        uint64_t end = 0;
        for (uint32_t i = 0; i < n; i++)
        {
            ResourceEntry e{uint32_t((p + 1) * 10000000 + i), 0, 0, 0, r.pos};
            e.size = r.u32();
            e.packed = r.u32();
            e.offset = r.u64();
            if (e.offset != end)
            {
                throw Error("non-contiguous Resources layout");
            }
            end = e.offset + e.packed;
            part.entries.push_back(e);
        }
        if (end != fs::file_size(d / ("Data" + padded(p, 3) + ".dat")))
        {
            throw Error("Resources file extent mismatch");
        }
        auto groups = r.count(16);
        for (uint32_t g = 0; g < groups; g++)
        {
            r.take(12);
            auto nrec = r.u32();
            if (nrec > 1)
            {
                r.take(size_t(nrec - 1) * 4);
            }
            r.take(size_t(nrec) * 12);
        }
        auto flag = r.u32();
        if (flag == 1)
        {
            r.u32();
            auto pairs = r.count(8);
            r.take(size_t(pairs) * 8);
        }
        else if (flag != 0)
        {
            throw Error("unknown Resources metadata flag");
        }
        parts.push_back(std::move(part));
    }
    r.finish();
}

const ResourceEntry& Resources::find(uint32_t id) const
{
    uint32_t q = id / 10000000, i = id % 10000000;
    if (!q || q > parts.size() || i >= parts[q - 1].entries.size())
    {
        throw Error("missing resource " + std::to_string(id));
    }
    return parts[q - 1].entries[i];
}

Bytes Resources::payload(uint32_t id) const
{
    auto& e = find(id);
    auto b = read_range(
        directory / ("Data" + padded(id / 10000000 - 1, 3) + ".dat"), e.offset, e.packed);
    return e.size ? lzss_decode(b, e.size) : b;
}

}
