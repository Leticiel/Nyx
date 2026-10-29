#include "image_profiles.hpp"
#include "progress.hpp"
#include <algorithm>
#include <limits>

namespace csystem
{
const Json& experimental_image_profiles()
{
    static const Json registry = Json::parse(
#include "experimental_image_profiles.inc"
    );
    return registry;
}
namespace
{
const Json& bgra_profiles()
{
    static const Json profiles = [] {
        Json values = Json::array();
        const auto add = [&](const char* name, size_t count, uint32_t radix, bool flipped,
                             std::array<size_t,6> fields) {
            values.push_back({{"id",std::string("BGRA-")+name},{"attribute_count",count},
                {"markers",radix==239 ? Json::array({239,251,250}) : Json::array({233,239,251})},
                {"flipped",flipped},{"validation","UNTESTED_ON_GAME_DATA"},{"runtime_tested",false},
                {"fields",{{"width",fields[0]},{"height",fields[1]},{"flags",fields[2]},
                           {"colorLength",fields[3]},{"alphaLength",fields[4]},{"maskLength",fields[5]}}}});
        };
        add("compact-cp932",20,233,false,{3,5,13,11,9,17});
        add("compact-utf16",20,233,false,{3,8,13,11,9,17});
        add("legacy20",20,233,false,{0,8,13,1,6,17});
        add("masked20",20,233,true,{1,3,10,8,6,17});
        add("harami20",20,233,true,{10,1,14,17,8,3});
        add("oshioki20",20,233,true,{2,4,9,17,6,13});
        add("zettai20",20,233,true,{4,6,13,11,9,17});
        add("fuka20",20,233,false,{0,5,13,11,9,17});
        add("legacy22",8,239,true,{4,0,7,2,3,5});
        add("shuffled22",8,239,true,{6,7,2,3,1,4});
        add("iji22",8,239,true,{2,3,7,5,4,1});
        add("lesson22",8,239,true,{0,3,7,2,4,1});
        add("legacy8",8,233,true,{2,1,4,6,3,5});
        add("ani8",8,233,true,{0,1,7,5,6,2});
        add("natsu8",8,233,true,{5,1,6,3,4,2});
        add("gyk8",8,233,true,{5,4,1,3,6,2});
        return values;
    }();
    return profiles;
}
const Json& profile_by_id(const std::string& id)
{
    for (const auto& profile : experimental_image_profiles().at("profiles"))
        if (profile.at("id") == id) return profile;
    for (const auto& profile : bgra_profiles())
        if (profile.at("id") == id) return profile;
    throw Error("unknown experimental image profile " + id);
}
uint32_t read_value(Cursor& reader, const Json& profile)
{
    const auto radix = profile.at("markers")[0].get<uint32_t>();
    const auto end = profile.at("markers")[1].get<uint8_t>();
    const auto zero = profile.at("markers")[2].get<uint8_t>();
    const auto first = reader.u8();
    const uint64_t low = first == zero ? 0 : first;
    if (low >= radix) throw Error("invalid profile integer digit");
    uint64_t high = 0, middle = 0;
    while (true)
    {
        const auto byte = reader.u8();
        if (byte == end) break;
        if (byte == radix) ++high;
        else
        {
            middle = byte == zero ? 0 : byte;
            if (middle >= radix) throw Error("invalid profile integer digit");
        }
        if (high > std::numeric_limits<uint32_t>::max() / (radix * radix))
            throw Error("profile integer overflow");
    }
    return checked32(low + (middle + high * radix) * radix);
}
void write_value(Bytes& output, uint32_t value, const Json& profile)
{
    const auto radix = profile.at("markers")[0].get<uint32_t>();
    const auto zero = profile.at("markers")[2].get<uint8_t>();
    output.push_back(value % radix ? uint8_t(value % radix) : zero);
    output.insert(output.end(), value / (radix * radix), uint8_t(radix));
    const auto middle = value / radix % radix;
    output.push_back(middle ? uint8_t(middle) : zero);
    output.push_back(profile.at("markers")[1].get<uint8_t>());
}
struct Packet
{
    std::vector<uint32_t> attrs;
    uint32_t width = 0, height = 0, flags = 0;
    uint8_t kind = 'a';
    bool marker_only = false;
    size_t alpha_shortfall = 0;
    std::string mode;
    std::span<const uint8_t> mask, partial, alpha, color;
};
Packet parse_packet(std::span<const uint8_t> input, const Json& profile)
{
    Cursor reader{input};
    Packet packet;
    const auto end = profile.at("markers")[1].get<uint8_t>();
    const auto marker = reader.u8();
    packet.marker_only = marker == end;
    if (!packet.marker_only)
    {
        if ((marker != 'a' && marker != 'd') || reader.u8() != end)
            throw Error("profile image marker differs");
        packet.kind = marker;
    }
    for (size_t i = 0; i < profile.at("attribute_count").get<size_t>(); ++i)
        packet.attrs.push_back(read_value(reader, profile));
    const auto field = [&](const char* name) { return packet.attrs.at(profile.at("fields").at(name)); };
    packet.width = field("width"); packet.height = field("height"); packet.flags = field("flags");
    const uint64_t pixels = uint64_t(packet.width) * packet.height;
    if (!packet.width || !packet.height || packet.width >= 0x8000 || packet.height >= 0x8000 ||
        pixels > 64 * 1024 * 1024 || packet.flags > 7)
        throw Error("invalid profile image dimensions or flags");
    const auto mask_size = field("maskLength"), alpha_size = field("alphaLength"), color_size = field("colorLength");
    const auto remaining = input.size() - reader.pos;
    const auto stride = (size_t(packet.width) * 3 + 3) & ~size_t(3);
    const auto plane = stride * packet.height;
    if (!mask_size)
    {
        if ((packet.flags & 6) == 6)
        {
            if (alpha_size != plane || color_size != plane || remaining != plane * 2)
                throw Error("profile full alpha plane lengths differ");
            packet.mode = "full_alpha";
            packet.alpha = reader.take(plane); packet.color = reader.take(plane);
        }
        else
        {
            if ((packet.flags & 6) == 2 || alpha_size || remaining != color_size)
                throw Error("invalid profile full color lengths");
            if (color_size == pixels) packet.mode = "gray8";
            else if (color_size == pixels * 3) packet.mode = "bgr24";
            else if (color_size == pixels * 4) packet.mode = "bgra32";
            else if (color_size == plane) packet.mode = "bgr24_padded";
            else throw Error("unsupported profile full color plane");
            packet.color = reader.take(color_size);
        }
    }
    else
    {
        if (mask_size < (pixels + 7) / 8 || mask_size > pixels / 8 + 1)
            throw Error("invalid profile bitmap length");
        packet.mask = reader.take(mask_size);
        const bool delta = (packet.flags & 1) && packet.kind == 'd';
        const bool two = (packet.flags & 2) && !delta;
        if (two) packet.partial = reader.take(mask_size);
        const bool has_alpha = (packet.flags & 6) == 6;
        size_t colored = 0, translucent = 0;
        for (size_t i = 0; i < pixels; ++i)
        {
            const auto bit = uint8_t(1 << (i % 8));
            const bool solid = (packet.mask[i / 8] & bit) != 0;
            const bool partial = two && (packet.partial[i / 8] & bit) != 0;
            colored += two ? (has_alpha ? (solid || partial) : (solid && !partial)) : solid;
            translucent += two ? partial : solid;
        }
        const size_t expected_alpha = has_alpha ? translucent * (two ? 3 : 1) : 0;
        const size_t expected_color = colored * 3;
        // Some baseline-only records declare zero color bytes but still carry
        // the changed BGR triples, as in GARbro's UnpackV6NoAlpha branch.
        if (alpha_size > expected_alpha ||
            (color_size != expected_color && !(color_size == 0 && !alpha_size && (packet.flags & 1))) ||
            input.size() - reader.pos != alpha_size + expected_color)
            throw Error("profile sparse plane lengths differ from masks");
        // GARbro bounds its sparse opacity reads by the declared plane length.
        // Keep that behavior for short legacy planes without borrowing color
        // bytes or reading past a buffer. Preserve the native packet unchanged.
        packet.alpha_shortfall = expected_alpha - alpha_size;
        packet.alpha = reader.take(alpha_size); packet.color = reader.take(expected_color);
        packet.mode = delta ? "delta" : two ? "sparse" : "mask";
    }
    reader.finish();
    return packet;
}
Json metadata(const Packet& packet, const Json& profile)
{
    Json result = {{"profile", profile.at("id")}, {"validation", "UNTESTED_ON_GAME_DATA"},
            {"runtime_tested", false}, {"attributes", packet.attrs},
            {"marker_only", packet.marker_only}, {"kind", std::string(1, char(packet.kind))},
            {"pixel_mode", packet.mode}, {"flipped", profile.at("flipped")}};
    if (packet.alpha_shortfall)
    {
        result["alpha_shortfall"] = packet.alpha_shortfall;
        result["alpha_recovery"] = "bounded sparse opacity; missing channels default to opaque";
    }
    return result;
}
Bytes fixed_packet(uint32_t width, uint32_t height, const Bytes& color, const Bytes& alpha,
                   const Bytes& mask = {}, uint32_t baseline = 0)
{
    Bytes result{'a'};
    for (const auto value : {baseline, 0u, width, height, alpha.empty() ? 0u : 4u,
                             checked32(alpha.size())}) put(result, value);
    append(result, alpha); put(result, mask.size()); append(result, mask);
    put(result, color.size()); append(result, color);
    return result;
}
}

bool known_image_layout(std::span<const uint8_t> input)
{
    return shuffled22_image_attributes(input) || iji22_image_attributes(input) ||
           lesson22_image_attributes(input) || legacy8_image_attributes(input) ||
           ani8_image_attributes(input) || natsu8_image_attributes(input) || gyk8_image_attributes(input) ||
           fuka_image_attributes(input) || masked_image_attributes(input) || harami_image_attributes(input) ||
           oshioki_image_attributes(input) || zettai_image_attributes(input) ||
           legacy22_image_attributes(input) || compact_image_attributes(input) || legacy_image_attributes(input);
}
std::optional<Json> profile_image_metadata(std::span<const uint8_t> input, const std::string& id)
{
    if (input.empty()) return std::nullopt;
    if (!id.empty())
    {
        const auto& profile = profile_by_id(id);
        try { return metadata(parse_packet(input, profile), profile); }
        catch (const Error&) { return std::nullopt; }
    }
    std::optional<Json> match;
    for (const auto& profile : experimental_image_profiles().at("profiles"))
    {
        try
        {
            const auto parsed = parse_packet(input, profile);
            if (match) return std::nullopt; // Never guess between distinct layouts.
            match = metadata(parsed, profile);
        }
        catch (const Error&) {}
    }
    return match;
}
std::optional<Json> bgra_profile_metadata(std::span<const uint8_t> input)
{
    std::optional<Json> match;
    for (const auto& profile : bgra_profiles())
    {
        try
        {
            const auto parsed = parse_packet(input,profile);
            if (parsed.mode != "bgra32") continue;
            if (match) return std::nullopt;
            match = metadata(parsed,profile);
        }
        catch(const Error&) {}
    }
    return match;
}
ImageProfileContext select_image_profile(const std::vector<Entry>& entries, const fs::path& content)
{
    ImageProfileContext result;
    std::vector<const Entry*> candidates;
    for (const auto& entry : entries)
        if (entry.subtype == '0' && std::string_view("bno").find(entry.kind) != std::string_view::npos)
            candidates.push_back(&entry);
    std::set<size_t> selected;
    for (size_t i = 0; i < std::min<size_t>(16, candidates.size()); ++i) selected.insert(i);
    for (size_t i = 0; i < 80 && !candidates.empty(); ++i)
        selected.insert(i * (candidates.size() - 1) / 79);
    std::map<std::string, size_t> scores;
    size_t unknown = 0;
    for (const auto index : selected)
    {
        check_cancelled();
        const auto bytes = entry_payload(*candidates[index], content);
        if (bytes.empty() || bytes[0] == 'b' || bytes[0] == 'c' || is_tb1(bytes) || known_image_layout(bytes) || bgra_profile_metadata(bytes)) continue;
        ++unknown;
        for (const auto& profile : experimental_image_profiles().at("profiles"))
        {
            try { parse_packet(bytes, profile); ++scores[profile.at("id").get<std::string>()]; }
            catch (const Error&) {}
        }
    }
    size_t best = 0, ties = 0;
    for (const auto& [id, score] : scores)
    {
        if (score > best) { result.id = id; best = score; ties = 1; }
        else if (score == best) ++ties;
    }
    if (!best || ties != 1 || best * 4 < unknown * 3) { result.id.clear(); return result; }
    const auto& profile = profile_by_id(result.id);
    std::optional<std::array<uint32_t, 3>> prior;
    for (const auto* entry : candidates)
    {
        check_cancelled();
        const auto bytes = entry_payload(*entry, content);
        try
        {
            const auto packet = parse_packet(bytes, profile);
            if ((packet.flags & 1) && prior && (*prior)[1] == packet.width && (*prior)[2] == packet.height)
                result.baselines[entry->id] = (*prior)[0];
            if (packet.kind != 'd' && packet.flags != 1) prior = {entry->id, packet.width, packet.height};
        }
        catch (const Error&)
        {
            prior.reset();
            if (!bytes.empty() && (bytes[0] == 'b' || bytes[0] == 'c'))
            {
                try
                {
                    const auto image = decode_image(canonical_image(bytes, DataFormat::Utf16), {});
                    prior = {entry->id, image.width, image.height};
                }
                catch (const Error&) {}
            }
        }
    }
    return result;
}
Bytes canonical_profile_image(std::span<const uint8_t> input, const Json& meta,
                              std::optional<uint32_t> baseline)
{
    const auto profile_id = meta.at("profile").get<std::string>();
    const auto& profile = profile_by_id(profile_id);
    const auto packet = parse_packet(input, profile);
    const size_t pixels = size_t(packet.width) * packet.height;
    const bool flipped = profile.at("flipped");
    const auto native_pixel = [&](size_t pixel) {
        return flipped ? (size_t(packet.height - 1) - pixel / packet.width) * packet.width + pixel % packet.width : pixel;
    };
    Bytes color, alpha, mask;
    if (packet.mode == "gray8" || packet.mode == "bgra32" || packet.mode == "bgr24" || packet.mode == "bgr24_padded" || packet.mode == "full_alpha")
    {
        const auto stride = packet.color.size() / packet.height;
        for (uint32_t y = 0; y < packet.height; ++y)
        {
            const auto row = flipped ? packet.height - 1 - y : y;
            append(color, packet.color.subspan(size_t(row) * stride, stride));
            if (!packet.alpha.empty()) append(alpha, packet.alpha.subspan(size_t(row) * stride, stride));
        }
        return fixed_packet(packet.width, packet.height, color, alpha);
    }
    const bool two = !packet.partial.empty();
    const bool delta = baseline && ((packet.kind == 'd' && (packet.flags & 1)) || packet.flags == 1);
    Bytes colors(pixels * 3), alphas(pixels * 3), present(pixels);
    size_t ci = 0, ai = 0;
    for (size_t i = 0; i < pixels; ++i)
    {
        const auto bit = uint8_t(1 << (i % 8));
        const bool solid = (packet.mask[i / 8] & bit) != 0;
        const bool partial = two && (packet.partial[i / 8] & bit) != 0;
        const bool visible = two ? ((packet.flags & 4) ? solid || partial : solid && !partial) : solid;
        if (visible)
        {
            present[i] = 1;
            std::copy_n(packet.color.begin() + ci, 3, colors.begin() + i * 3); ci += 3;
            std::fill_n(alphas.begin() + i * 3, 3, 255);
        }
        if ((packet.flags & 6) == 6 && (two ? partial : solid))
        {
            for (size_t channel = 0; channel < 3; ++channel)
            {
                const auto source = ai + (two ? channel : 0);
                alphas[i * 3 + channel] = source < packet.alpha.size() ? packet.alpha[source] : 255;
            }
            ai += two ? 3 : 1;
        }
    }
    if (delta) mask.resize((pixels + 7) / 8);
    for (size_t pixel = 0; pixel < pixels; ++pixel)
    {
        const auto at = native_pixel(pixel);
        if (delta && !present[at]) continue;
        if (delta) mask[pixel / 8] |= uint8_t(1 << (pixel % 8));
        append(color, std::span(colors).subspan(at * 3, 3));
        if (!delta || !packet.alpha.empty()) append(alpha, std::span(alphas).subspan(at * 3, 3));
    }
    return fixed_packet(packet.width, packet.height, color, alpha, mask, baseline.value_or(0));
}
Bytes encode_profile_image(const Image& image, Json meta, const Image* channel_alpha)
{
    const auto profile_id = meta.at("profile").get<std::string>();
    const auto& profile = profile_by_id(profile_id);
    const size_t pixels = size_t(image.width) * image.height;
    if (!pixels || pixels > 64 * 1024 * 1024 || image.width >= 0x8000 || image.height >= 0x8000 || image.bgra.size() != pixels * 4)
        throw Error("invalid experimental replacement image");
    if (channel_alpha && (channel_alpha->width != image.width || channel_alpha->height != image.height || channel_alpha->bgra.size() != image.bgra.size()))
        throw Error("channel alpha image dimensions differ");
    auto attrs = meta.at("attributes").get<std::vector<uint32_t>>();
    if (attrs.size() != profile.at("attribute_count")) throw Error("profile attribute count differs");
    const auto set = [&](const char* field, uint32_t value) { attrs.at(profile.at("fields").at(field)) = value; };
    const bool flipped = profile.at("flipped");
    const auto offset = [&](size_t pixel) {
        const auto row = pixel / image.width;
        return ((flipped ? image.height - 1 - row : row) * image.width + pixel % image.width) * 4;
    };
    const std::string mode = meta.at("pixel_mode");
    Bytes color, alpha, mask, partial;
    uint32_t flags = 0;
    if (mode == "bgra32" || mode == "gray8" || mode == "bgr24" || mode == "bgr24_padded" || mode == "full_alpha")
    {
        flags = mode == "full_alpha" ? 6 : 0;
        const size_t stride = (size_t(image.width) * 3 + 3) & ~size_t(3);
        for (size_t pixel = 0; pixel < pixels; ++pixel)
        {
            const auto at = offset(pixel);
            if (mode == "gray8")
            {
                if (image.bgra[at] != image.bgra[at+1] || image.bgra[at] != image.bgra[at+2] || image.bgra[at+3] != 255)
                    throw Error("grayscale image requires opaque gray pixels");
                color.push_back(image.bgra[at]);
            }
            else
            {
                if (mode != "bgra32" && mode != "full_alpha" && image.bgra[at+3] != 255)
                    throw Error("BGR image cannot store alpha; use a native alpha image");
                append(color, std::span(image.bgra).subspan(at, mode == "bgra32" ? 4 : 3));
                if (mode == "full_alpha")
                    for (size_t c = 0; c < 3; ++c) alpha.push_back(channel_alpha ? channel_alpha->bgra[at+c] : image.bgra[at+3]);
            }
            if ((pixel + 1) % image.width == 0 && (mode == "full_alpha" || mode == "bgr24_padded"))
            {
                color.insert(color.end(), stride - image.width * 3, 0);
                if (mode == "full_alpha") alpha.insert(alpha.end(), stride - image.width * 3, 0);
            }
        }
    }
    else
    {
        // Re-encode edited sparse/delta pictures as self-contained native 'a'
        // packets. This removes baseline dependencies without PNG wrappers.
        flags = 6;
        mask.resize(pixels / 8 + 1); partial.resize(mask.size());
        for (size_t pixel = 0; pixel < pixels; ++pixel)
        {
            const auto at = offset(pixel);
            std::array<uint8_t, 3> opacity;
            for (size_t c = 0; c < 3; ++c) opacity[c] = channel_alpha ? channel_alpha->bgra[at+c] : image.bgra[at+3];
            const bool solid = opacity == std::array<uint8_t, 3>{255,255,255};
            const bool visible = solid || opacity != std::array<uint8_t, 3>{0,0,0} || image.bgra[at] || image.bgra[at+1] || image.bgra[at+2];
            if (solid) mask[pixel / 8] |= uint8_t(1 << (pixel % 8));
            else if (visible) { partial[pixel / 8] |= uint8_t(1 << (pixel % 8)); append(alpha, opacity); }
            if (visible) append(color, std::span(image.bgra).subspan(at, 3));
        }
    }
    set("width", image.width); set("height", image.height); set("flags", flags);
    set("colorLength", checked32(color.size())); set("alphaLength", checked32(alpha.size())); set("maskLength", checked32(mask.size()));
    Bytes output;
    if (!meta.value("marker_only", false)) output.push_back('a');
    output.push_back(profile.at("markers")[1].get<uint8_t>());
    for (const auto value : attrs) write_value(output, value, profile);
    append(output, mask); append(output, partial); append(output, alpha); append(output, color);
    return output;
}

bool is_tb1(std::span<const uint8_t> bytes)
{
    return bytes.size() >= 7 && std::equal(bytes.begin(), bytes.begin() + 7, "LEAF64K");
}
Image decode_tb1(std::span<const uint8_t> bytes)
{
    if (!is_tb1(bytes) || bytes.size() < 24) throw Error("invalid TB1 header");
    Cursor reader{bytes}; reader.take(12);
    const uint32_t width = reader.u16(), height = reader.u16();
    if (reader.u16() != 24 || !width || !height || uint64_t(width) * height > 64 * 1024 * 1024)
        throw Error("invalid TB1 dimensions or bit depth");
    reader.take(6);
    Bytes pixels(size_t(width) * height * 3), frame(4096);
    size_t dst = 0, frame_pos = 0xfee;
    while (dst < pixels.size())
    {
        const auto flags = uint8_t(~reader.u8());
        for (uint32_t bit = 0x80; bit && dst < pixels.size(); bit >>= 1)
        {
            if (flags & bit)
            {
                const auto value = uint8_t(~reader.u8());
                pixels[dst++] = frame[frame_pos++ & 0xfff] = value;
            }
            else
            {
                const auto pair = uint16_t(~reader.u16());
                size_t offset = pair >> 4;
                const size_t count = std::min<size_t>((pair & 15) + 3, pixels.size() - dst);
                for (size_t i = 0; i < count; ++i)
                {
                    const auto value = frame[offset++ & 0xfff];
                    pixels[dst++] = frame[frame_pos++ & 0xfff] = value;
                }
            }
        }
    }
    Image result{width, height, Bytes(size_t(width) * height * 4, 255)};
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x)
            std::copy_n(pixels.begin() + (size_t(height - 1 - y) * width + x) * 3, 3,
                        result.bgra.begin() + (size_t(y) * width + x) * 4);
    return result;
}
Bytes encode_tb1(const Image& image, std::span<const uint8_t> header)
{
    if (header.size() != 24 || !is_tb1(header) || !image.width || !image.height ||
        image.width > 65535 || image.height > 65535 || uint64_t(image.width) * image.height > 64 * 1024 * 1024 ||
        image.bgra.size() != uint64_t(image.width) * image.height * 4)
        throw Error("invalid TB1 replacement");
    Bytes output(header.begin(), header.end());
    patch(output, 12, image.width, 2); patch(output, 14, image.height, 2); patch(output, 16, 24, 2);
    size_t count = 0;
    // Literal packets are valid in the original complemented, MSB-first LZSS.
    for (uint32_t y = image.height; y-- > 0;)
        for (uint32_t x = 0; x < image.width; ++x)
        {
            const auto at = (size_t(y) * image.width + x) * 4;
            if (image.bgra[at+3] != 255) throw Error("TB1 does not store alpha");
            for (size_t c = 0; c < 3; ++c)
            {
                if (count++ % 8 == 0) output.push_back(0);
                output.push_back(uint8_t(~image.bgra[at+c]));
            }
        }
    return output;
}
Bytes encode_fixed_bgra(const Image& image, uint32_t base, uint32_t parameter)
{
    if (!image.width || !image.height || uint64_t(image.width) * image.height > 64 * 1024 * 1024 ||
        image.bgra.size() != uint64_t(image.width) * image.height * 4) throw Error("invalid BGRA replacement");
    auto result = fixed_packet(image.width, image.height, image.bgra, {});
    patch(result, 1, base); patch(result, 5, parameter); return result;
}
}
