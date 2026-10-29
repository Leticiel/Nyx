#include "standalone.hpp"
#include "audio.hpp"
#include "documents.hpp"
#include "progress.hpp"
#include "references.hpp"
#include "legacy.hpp"
#include "graphics.hpp"
#include "page_parameters.hpp"
#include "image_cache.hpp"
#include "jpeg.hpp"
#include "image_profiles.hpp"
#include <algorithm>

namespace csystem
{
namespace
{
fs::path local_path(const fs::path& root, const std::string& name)
{
    const fs::path relative(utf16(name));
    if (relative.empty() || relative.is_absolute() || relative.has_root_name())
    {
        throw Error("invalid project file path");
    }
    for (const auto& part : relative)
    {
        if (part == "..")
        {
            throw Error("project file path escapes its directory");
        }
    }
    return root / relative;
}

void stream_bytes(std::ofstream& stream, std::span<const uint8_t> bytes)
{
    if (!stream.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size())))
    {
        throw Error("archive write failed");
    }
}

void finish_stream(std::ofstream& stream)
{
    stream.close();
    if (!stream)
    {
        throw Error("archive close failed");
    }
}

std::string image_metadata_hash(Json file)
{
    file.erase("native_source");
    const auto text = file.dump();
    return sha256({reinterpret_cast<const uint8_t*>(text.data()), text.size()});
}

std::optional<Bytes> preserved_image_packet(const fs::path& project, const Json& file)
{
    if (!file.contains("native_source")) return std::nullopt;
    const auto& source = file.at("native_source");
    if (source.at("metadata_sha256") != image_metadata_hash(file) ||
        source.at("editable_sha256") != sha256(read_file(local_path(project, file.at("path")))))
        return std::nullopt;
    if (file.contains("channel_alpha") &&
        source.at("alpha_sha256") != sha256(read_file(local_path(project, file.at("channel_alpha")))))
        return std::nullopt;
    const auto path = local_path(project, source.at("path"));
    return source.contains("offset") ? read_range(path, source.at("offset"), source.at("packed_size"))
                                     : read_file(path);
}

bool top_down_bitmap_source(const Json& file)
{
    const auto orientation = file.value("image_orientation", "bottom_up");
    if (orientation != "top_down" && orientation != "bottom_up")
        throw Error("unsupported image_orientation " + orientation);
    return orientation == "top_down";
}

Bytes import_asset(const fs::path& project, const Json& file, DataFormat format, uint32_t default_wrapper_header,
                   const ResourceResolver& resolve)
{
    const auto type = file.at("type").get<std::string>();
    auto bytes = read_file(local_path(project, file.at("path")));
    if (type == "raw")
    {
        return bytes;
    }
    if (type == "jpg")
    {
        auto wrapped = wrap_jpeg(bytes);
        decode_standard(bytes);
        return wrapped;
    }
    if (type == "video")
    {
        if (file.at("archive") != "videos") throw Error("video asset must belong to Arc07/Arc08");
        transform_archive_video(bytes);
        return bytes;
    }
    if (type == "timeline" || type == "native_page" || type == "motion")
    {
        const auto document = Json::parse(bytes.begin(), bytes.end());
        return type == "timeline" ? encode_graphics_timeline(document, resolve)
             : type == "native_page" ? encode_native_page(document, resolve)
                                      : encode_motion_preset(document);
    }
    if (type == "wav")
    {
        validate_wave(bytes);
        return bytes;
    }
    if (type == "external_audio")
    {
        return encode_cp932(file.at("engine_path"));
    }
    if (type == "ogg")
    {
        AudioFile audio;
        audio.obfuscated = file.at("obfuscated");
        for (const auto& value : file.at("loop_fields"))
        {
            put(audio.prefix, value.get<uint32_t>());
        }
        if (file.contains("trailing_data"))
        {
            audio.suffix = read_file(local_path(project, file.at("trailing_data")));
        }
        return encode_audio(bytes, audio);
    }
    if (type != "png")
    {
        throw Error("unsupported asset type " + type);
    }
    if (file.contains("experimental_image"))
    {
        std::optional<Image> alpha;
        if (file.contains("channel_alpha")) alpha = decode_standard(read_file(local_path(project, file.at("channel_alpha"))));
        return encode_profile_image(decode_standard(bytes), file.at("experimental_image"), alpha ? &*alpha : nullptr);
    }
    if (file.value("image_encoding", "") == "tb1")
        return encode_tb1(decode_standard(bytes), unhex(file.at("tb1_header")));
    if (file.value("image_encoding", "") == "native_bgra32")
        return encode_fixed_bgra(decode_standard(bytes), file.value("image_base", 0u), file.value("image_parameter", 0u));
    if (file.contains("shuffled22_attributes") || file.contains("iji22_attributes") ||
        file.contains("lesson22_attributes") || file.contains("ani8_attributes") ||
        file.contains("natsu8_attributes") || file.contains("gyk8_attributes"))
    {
        std::optional<Image> channel_alpha;
        if (file.contains("channel_alpha"))
            channel_alpha = decode_standard(read_file(local_path(project,file.at("channel_alpha"))));
        if (file.contains("gyk8_attributes"))
            return gyk8_image(decode_standard(bytes),file.at("gyk8_attributes").get<std::array<uint32_t,8>>(),
                              channel_alpha ? &*channel_alpha : nullptr,top_down_bitmap_source(file));
        if (file.contains("natsu8_attributes"))
            return natsu8_image(decode_standard(bytes),file.at("natsu8_attributes").get<std::array<uint32_t,8>>(),
                                channel_alpha ? &*channel_alpha : nullptr,top_down_bitmap_source(file));
        if (file.contains("ani8_attributes"))
            return ani8_image(decode_standard(bytes),file.at("ani8_attributes").get<std::array<uint32_t,8>>(),
                              channel_alpha ? &*channel_alpha : nullptr,top_down_bitmap_source(file));
        if (file.contains("lesson22_attributes"))
            return lesson22_image(decode_standard(bytes),file.at("lesson22_attributes").get<std::array<uint32_t,8>>(),
                                  channel_alpha ? &*channel_alpha : nullptr,top_down_bitmap_source(file));
        if (file.contains("iji22_attributes"))
            return iji22_image(decode_standard(bytes),file.at("iji22_attributes").get<std::array<uint32_t,8>>(),
                               channel_alpha ? &*channel_alpha : nullptr,top_down_bitmap_source(file));
        return shuffled22_image(decode_standard(bytes),file.at("shuffled22_attributes").get<std::array<uint32_t,8>>(),
                                channel_alpha ? &*channel_alpha : nullptr);
    }
    if (file.contains("legacy8_attributes"))
    {
        std::optional<Image> channel_alpha;
        if (file.contains("channel_alpha"))
            channel_alpha = decode_standard(read_file(local_path(project, file.at("channel_alpha"))));
        return legacy8_image(decode_standard(bytes),
                             file.at("legacy8_attributes").get<std::array<uint32_t, 8>>(),
                             channel_alpha ? &*channel_alpha : nullptr, top_down_bitmap_source(file));
    }
    if (file.contains("legacy22_attributes"))
    {
        std::optional<Image> channel_alpha;
        if (file.contains("channel_alpha"))
        {
            channel_alpha =
                decode_standard(read_file(local_path(project, file.at("channel_alpha"))));
        }
        return legacy22_image(decode_standard(bytes),
                              file.at("legacy22_attributes").get<std::array<uint32_t, 8>>(),
                              channel_alpha ? &*channel_alpha : nullptr, top_down_bitmap_source(file));
    }
    if (file.contains("masked_attributes") || file.contains("harami_attributes") ||
        file.contains("oshioki_attributes") || file.contains("zettai_attributes") || file.contains("fuka_attributes"))
    {
        std::optional<Image> channel_alpha;
        if (file.contains("channel_alpha"))
            channel_alpha = decode_standard(read_file(local_path(project, file.at("channel_alpha"))));
        if (file.contains("fuka_attributes"))
            return fuka_image(decode_standard(bytes),
                              file.at("fuka_attributes").get<std::array<uint32_t, 20>>(),
                              channel_alpha ? &*channel_alpha : nullptr, top_down_bitmap_source(file));
        if (file.contains("zettai_attributes"))
            return zettai_image(decode_standard(bytes),
                                file.at("zettai_attributes").get<std::array<uint32_t, 20>>(),
                                channel_alpha ? &*channel_alpha : nullptr, top_down_bitmap_source(file));
        if (file.contains("oshioki_attributes"))
            return oshioki_image(decode_standard(bytes),
                                 file.at("oshioki_attributes").get<std::array<uint32_t, 20>>(),
                                 channel_alpha ? &*channel_alpha : nullptr, top_down_bitmap_source(file));
        if (file.contains("harami_attributes"))
            return harami_image(decode_standard(bytes),
                                file.at("harami_attributes").get<std::array<uint32_t, 20>>(),
                                channel_alpha ? &*channel_alpha : nullptr, top_down_bitmap_source(file));
        return masked_image(decode_standard(bytes),
                            file.at("masked_attributes").get<std::array<uint32_t, 20>>(),
                            channel_alpha ? &*channel_alpha : nullptr, top_down_bitmap_source(file));
    }
    if (file.value("image_encoding", "png") == "compact_bitmap" ||
        file.value("image_encoding", "png") == "legacy_bitmap")
    {
        std::optional<Image> channel_alpha;
        if (file.contains("channel_alpha"))
        {
            channel_alpha = decode_standard(read_file(local_path(project, file.at("channel_alpha"))));
        }
        if (file.at("image_encoding") == "legacy_bitmap")
            return legacy_bitmap(decode_standard(bytes),
                                 file.at("legacy_attributes").get<std::array<uint32_t, 20>>(),
                                 channel_alpha ? &*channel_alpha : nullptr);
        return compact_bitmap(decode_standard(bytes),
                              file.at("compact_attributes").get<std::array<uint32_t, 20>>(),
                              format, channel_alpha ? &*channel_alpha : nullptr);
    }
    if (file.value("image_encoding", "png") == "grayscale")
    {
        const auto image = decode_standard(bytes);
        if (file.contains("compact_attributes"))
        {
            return compact_grayscale(
                image, file.at("compact_attributes").get<std::array<uint32_t, 20>>(), format);
        }
        if (format == DataFormat::Cp932)
        {
            return legacy_grayscale(image);
        }
        Bytes output{'a'};
        put(output, file.at("image_base").get<uint32_t>());
        put(output, file.at("image_parameter").get<uint32_t>());
        put(output, image.width);
        put(output, image.height);
        put(output, 0);
        put(output, 0);
        put(output, 0);
        put(output, checked32(uint64_t(image.width) * image.height));
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
    auto wrapped = wrap_png(bytes, file.at("image_flag").get<uint8_t>());
    const auto wrapper_header = file.value("image_wrapper_header", default_wrapper_header);
    if (wrapper_header != 5 && wrapper_header != 6) throw Error("unsupported image_wrapper_header");
    if (wrapper_header == 5)
    {
        wrapped.erase(wrapped.begin() + 5);
    }
    return wrapped;
}

Json import_config_structure(const fs::path& project, const Json& structure)
{
    Json decoded = {{"format", "csystem-config-1"},
                    {"strings", Json::array()},
                    {"blocks", Json::array({""})},
                    {"settings", Json::array()}};
    if (structure.at("data_format") == "cp932")
    {
        decoded["data_format"] = "cp932";
    }
    if (structure.contains("settings_storage"))
    {
        decoded["settings_storage"] = structure.at("settings_storage");
    }
    if (structure.contains("compact_settings_terminated"))
    {
        decoded["compact_settings_terminated"] = structure.at("compact_settings_terminated");
    }
    for (size_t index = 0; index < structure.at("strings").size(); ++index)
    {
        const auto& item = structure.at("strings")[index];
        if (item.contains("title"))
        {
            decoded["strings"].push_back({{"text", ""}});
        }
        else if (item.contains("line_breaking"))
        {
            const auto field = std::find_if(std::begin(line_breaking_fields),
                                            std::end(line_breaking_fields),
                                            [&](const LineBreakingField& candidate)
                                            {
                                                return candidate.string_index == index &&
                                                       item.at("line_breaking") == candidate.name;
                                            });
            const auto suffix = item.at("suffix").get<std::string>();
            if (field == std::end(line_breaking_fields) ||
                suffix.find_first_not_of("\r\n") != std::string::npos)
            {
                throw Error("invalid line-breaking config metadata");
            }
            decoded["strings"].push_back({{"text", field->prefix + suffix}});
        }
        else if (item.at("encoding") == "utf8")
        {
            decoded["strings"].push_back(
                {{"text", read_text(local_path(project, item.at("path")))}});
        }
        else
        {
            decoded["strings"].push_back(
                {{"hex", hex(read_file(local_path(project, item.at("path"))))}});
        }
        if (item.contains("native_suffix"))
        {
            decoded["strings"].back()["native_suffix"] = item.at("native_suffix");
        }
    }
    for (const auto& name : structure.at("blocks"))
    {
        decoded["blocks"].push_back(hex(read_file(local_path(project, name))));
    }
    for (const auto& key : structure.at("setting_keys"))
    {
        decoded["settings"].push_back({{"key", key}, {"value", 0}});
    }
    return decoded;
}
}

ArchivePosition reserve_archive_space(ArchivePosition& cursor, uint8_t source_part, uint64_t size)
{
    constexpr uint64_t limit = INT32_MAX;
    if (source_part > 26 || cursor.part > 26 || cursor.offset > limit || size > limit)
    {
        throw Error("archive entry exceeds the engine content-part limits");
    }
    auto position = cursor;
    if (source_part > position.part)
    {
        position = {source_part, 0};
    }
    if (position.offset + size > limit)
    {
        if (position.part == 26)
        {
            throw Error("archive requires more than 27 content parts");
        }
        position = {uint8_t(position.part + 1), 0};
    }
    cursor = {position.part, position.offset + size};
    return position;
}

void export_build_metadata(const fs::path& game, const fs::path& project, Json& manifest)
{
    report_progress("Exporting archive structure and opaque data...");
    manifest["format"] = project_format;
    const auto format = parse_data_format(manifest.at("data_format"));
    const auto tunnel = format == DataFormat::Cp932 ? read_sjis_tunnel_table(game / "sjis_ext.bin")
                                                    : std::u16string{};
    if (tunnel.empty())
    {
        manifest.erase("sjis_tunnel");
    }
    else
    {
        manifest["sjis_tunnel"] = {{"characters", utf8(tunnel)}};
    }
    manifest["archives"] = Json::array();
    std::set<uint32_t> wrapper_headers;
    std::map<std::string, std::map<uint32_t, Entry>> indexes;
    std::set<std::string> preserved_buckets;
    std::vector<uint32_t> bitmap_ids;
    const auto pairs = game_archives(game);
    for (const auto& pair : pairs)
    {
        Json archive = {{"name", pair.key},
                        {"index", pair.index},
                        {"content", pair.content},
                        {"entries", Json::array()}};
        const auto index = read_arc_index(local_path(game, archive.at("index")),
                                          local_path(game, archive.at("content")));
        archive["index_layout"] = archive_index_layout_name(index.layout);
        if (std::any_of(index.entries.begin(), index.entries.end(), index_only_archive_entry))
        {
            preserved_buckets.insert(pair.key);
            // Preserve the backing bytes and every synthetic slot verbatim.
            // Modified real images are appended to a fresh part at build time.
            archive["preserved_sources"] = Json::array();
            std::set<uint8_t> parts{0};
            for (const auto& entry : index.entries) parts.insert(entry.part);
            const auto retain = [&](const fs::path& name)
            {
                check_cancelled();
                const auto path = fs::path("raw/archive_sources") / name;
                fs::create_directories((project / path).parent_path());
                fs::copy_file(game / name, project / path, fs::copy_options::overwrite_existing);
                archive["preserved_sources"].push_back({{"path", utf8(path.generic_u16string())},
                                                        {"sha256", file_hash(project / path)}});
            };
            retain(pair.index);
            for (const auto part : parts) retain(archive_part_path(pair.content, part));
        }
        for (const auto& entry : index.entries)
        {
            if (pair.key == "images" && entry.kind == 'b' &&
                (entry.subtype == '0' || opaque_archive_entry(entry) || index_only_archive_entry(entry)))
                bitmap_ids.push_back(entry.id);
            indexes[pair.key][entry.id] = entry;
            archive["entries"].push_back({{"id", entry.id},
                                          {"version", entry.version},
                                          {"kind", std::string(1, entry.kind)},
                                          {"subtype", std::string(1, entry.subtype)},
                                          {"reserved", entry.reserved},
                                          {"part", entry.part}});
            if (index_only_archive_entry(entry))
            {
                archive["entries"].back()["index_only"] = true;
            }
            else if (opaque_archive_entry(entry))
            {
                archive["entries"].back()["decoded_size"] = entry.size;
            }
            else
            {
                archive["entries"].back()["compression"] =
                    archive_entry_compression(entry);
                if (pair.key == "images" && entry.version == 22 && entry.subtype == '0' &&
                    entry.size == entry.packed_size && std::string_view("bno").find(entry.kind) != std::string_view::npos)
                    archive["entries"].back()["compression"] = archive_entry_compression(entry,
                        read_range(archive_part_path(local_path(game, archive.at("content")), entry.part),
                                   entry.offset, entry.packed_size));
            }
            if (entry.audio_prefix_size != -1)
                archive["entries"].back()["audio_prefix_size"] = entry.audio_prefix_size;
            if (!entry.leading.empty())
                archive["entries"].back()["leading_data"] = hex(entry.leading);
        }
        manifest["archives"].push_back(archive);
    }
    Resources resources(game / "Resources");
    std::vector<Entry> image_entries;
    for (const auto& [id, entry] : indexes.at("images")) image_entries.push_back(entry);
    const auto image_profile = manifest.value("media_exported", true)
        ? select_image_profile(image_entries, game / "Arc05.dat") : ImageProfileContext{};
    ImageCache bitmap_alpha([&](uint32_t id) {
        return canonical_image(entry_payload(indexes.at("images").at(id), game / "Arc05.dat"),
                               format, bitmap_ids, &image_profile, id);
    }, true);
    auto retain_native_packet = [&](Json& file)
    {
        if (file.at("archive") != "images") return;
        const auto& entry = indexes.at("images").at(file.at("entry").get<uint32_t>());
        if (opaque_archive_entry(entry)) return;
        if (format != DataFormat::Cp932 && archive_entry_compression(entry) != "none_zero_size" &&
            !preserved_buckets.contains("images")) return;
        const auto path = preserved_buckets.contains("images")
                              ? "raw/archive_sources/" + archive_part_path("Arc05.dat", entry.part).string()
                              : "raw/native_images/" + padded(entry.id) + ".bin";
        const auto stored = read_range(archive_part_path(game / "Arc05.dat", entry.part),
                                       entry.offset, entry.packed_size);
        if (!preserved_buckets.contains("images")) write_file(project / path, stored);
        file["native_source"] = {{"path", path}, {"size", entry.size ? entry.size : entry.packed_size},
                                  {"compression", archive_entry_compression(entry, stored)},
                                  {"editable_sha256", sha256(read_file(local_path(project, file.at("path"))))}};
        if (preserved_buckets.contains("images"))
        {
            file["native_source"]["offset"] = entry.offset;
            file["native_source"]["packed_size"] = entry.packed_size;
        }
        if (file.contains("channel_alpha"))
            file["native_source"]["alpha_sha256"] =
                sha256(read_file(local_path(project, file.at("channel_alpha"))));
        file["native_source"]["metadata_sha256"] = image_metadata_hash(file);
    };
    for (auto& file : manifest.at("files"))
    {
        check_cancelled();
        file.erase("sha256");
        file.erase("base_image");
        if (file.at("type") != "png" && file.at("type") != "ogg")
        {
            if (file.at("type") == "raw") retain_native_packet(file);
            continue;
        }
        const auto id = file.at("entry").get<uint32_t>();
        const auto bucket = file.at("archive").get<std::string>();
        Bytes payload;
        if (bucket == "resources")
        {
            payload = resources.payload(id);
        }
        else
        {
            const auto pair = std::find_if(pairs.begin(), pairs.end(),
                                          [&](const auto& value) { return value.key == bucket; });
            if (pair == pairs.end()) throw Error("unknown archive bucket " + bucket);
            payload = entry_payload(indexes.at(bucket).at(id), game / pair->content);
        }
        if (file.at("type") == "ogg")
        {
            const auto audio = decode_audio(payload);
            file["obfuscated"] = audio.obfuscated;
            file["loop_fields"] = Json::array();
            Cursor prefix{audio.prefix};
            while (!prefix.done())
            {
                file["loop_fields"].push_back(prefix.u32());
            }
            if (!audio.suffix.empty())
            {
                const auto path = "raw/audio/" + padded(id) + ".trailing.bin";
                write_file(project / path, audio.suffix);
                file["trailing_data"] = path;
            }
        }
        else
        {
            auto experimental = bgra_profile_metadata(payload);
            if (!experimental) experimental = !image_profile.id.empty() ? profile_image_metadata(payload, image_profile.id)
                : known_image_layout(payload) ? std::nullopt : profile_image_metadata(payload);
            if (experimental || is_tb1(payload))
            {
                if (experimental)
                {
                    file["image_encoding"] = "experimental_profile";
                    file["experimental_image"] = *experimental;
                    if (experimental->contains("alpha_shortfall"))
                    {
                        if (!manifest.contains("warnings")) manifest["warnings"] = Json::array();
                        manifest["warnings"].push_back({{"archive",bucket},{"entry",id},
                            {"message","Short native alpha plane: " + experimental->at("alpha_shortfall").dump() +
                                " missing bytes; preview uses opaque fallback. Original packet is retained."}});
                        report_progress("Warning: image " + std::to_string(id) + " has " +
                            experimental->at("alpha_shortfall").dump() + " missing alpha bytes; using opaque fallback.");
                    }
                    const auto opacity = bucket == "images" ? bitmap_alpha.get(id)
                        : decode_image_alpha(canonical_profile_image(payload,*experimental),{});
                    bool separate = false;
                    for (size_t pixel = 0; pixel < opacity.bgra.size(); pixel += 4)
                        separate = separate || opacity.bgra[pixel] != opacity.bgra[pixel+1] || opacity.bgra[pixel] != opacity.bgra[pixel+2];
                    if (separate)
                    {
                        const fs::path path(utf16(file.at("path").get<std::string>()));
                        const auto alpha_path = path.parent_path() / (path.stem().wstring() + L".alpha.png");
                        write_file(project / alpha_path, encode_png(opacity));
                        file["channel_alpha"] = utf8(alpha_path.generic_u16string());
                    }
                }
                else
                {
                    file["image_encoding"] = "tb1";
                    file["tb1_header"] = hex(std::span(payload).first(24));
                    file["image_validation"] = "UNTESTED_ON_GAME_DATA";
                }
                retain_native_packet(file);
                continue;
            }
            const auto shuffled = shuffled22_image_attributes(payload);
            const auto iji = shuffled ? std::nullopt : iji22_image_attributes(payload);
            const auto lesson = shuffled || iji ? std::nullopt : lesson22_image_attributes(payload);
            const auto ani = shuffled || iji || lesson ? std::nullopt : ani8_image_attributes(payload);
            const auto natsu = shuffled || iji || lesson || ani ? std::nullopt : natsu8_image_attributes(payload);
            const auto reordered22 = shuffled ? shuffled : iji ? iji : lesson ? lesson : ani ? ani : natsu ? natsu : gyk8_image_attributes(payload);
            if (reordered22)
            {
                file["image_encoding"] = shuffled ? "shuffled22_bitmap" : iji ? "iji22_bitmap" : lesson ? "lesson22_bitmap" : ani ? "ani8_bitmap" : natsu ? "natsu8_bitmap" : "gyk8_bitmap";
                file["image_orientation"] = "top_down";
                file[shuffled ? "shuffled22_attributes" : iji ? "iji22_attributes" : lesson ? "lesson22_attributes" : ani ? "ani8_attributes" : natsu ? "natsu8_attributes" : "gyk8_attributes"] = *reordered22;
                const auto opacity = decode_image_alpha(canonical_image(payload,format),{});
                bool separate = false;
                for (size_t pixel=0;pixel<opacity.bgra.size();pixel+=4)
                    separate = separate || opacity.bgra[pixel]!=opacity.bgra[pixel+1] || opacity.bgra[pixel]!=opacity.bgra[pixel+2];
                if (separate)
                {
                    const fs::path path(utf16(file.at("path").get<std::string>()));
                    const auto alpha_path = path.parent_path() / (path.stem().wstring()+L".alpha.png");
                    write_file(project/alpha_path,encode_png(opacity));
                    file["channel_alpha"] = utf8(std::u16string(alpha_path.u16string()));
                }
                retain_native_packet(file);
                continue;
            }
            if (!payload.empty() && payload[0]=='c')
            {
                const auto canonical = canonical_image(payload,format);
                file["image_wrapper_header"] = payload.size()==canonical.size() ? 6 : 5;
                if (bucket == "images") wrapper_headers.insert(file.at("image_wrapper_header").get<uint32_t>());
            }
            if (const auto attributes8 = legacy8_image_attributes(payload))
            {
                file["image_encoding"] = "legacy8_bitmap";
                file["image_orientation"] = "top_down";
                file["legacy8_attributes"] = *attributes8;
                const auto opacity = decode_image_alpha(canonical_image(payload, format), {});
                bool separate = false;
                for (size_t pixel = 0; pixel < opacity.bgra.size(); pixel += 4)
                    separate = separate || opacity.bgra[pixel] != opacity.bgra[pixel + 1] ||
                               opacity.bgra[pixel] != opacity.bgra[pixel + 2];
                if (separate)
                {
                    const fs::path path(utf16(file.at("path").get<std::string>()));
                    const auto alpha_path = path.parent_path() / (path.stem().wstring() + L".alpha.png");
                    write_file(project / alpha_path, encode_png(opacity));
                    file["channel_alpha"] = utf8(std::u16string(alpha_path.u16string()));
                }
                retain_native_packet(file);
                continue;
            }
            if (const auto attributes22 = legacy22_image_attributes(payload))
            {
                for (const auto* key : {"image_flag",
                                        "image_encoding",
                                        "image_base",
                                        "image_parameter",
                                        "compact_attributes"})
                {
                    file.erase(key);
                }
                file["legacy22_attributes"] = *attributes22;
                file["image_orientation"] = "top_down";
                retain_native_packet(file);
                continue;
            }
            const auto masked_attributes = masked_image_attributes(payload);
            const auto harami_attributes = masked_attributes ? std::nullopt : harami_image_attributes(payload);
            const auto oshioki_attributes = masked_attributes || harami_attributes ? std::nullopt : oshioki_image_attributes(payload);
            const auto zettai_attributes = masked_attributes || harami_attributes || oshioki_attributes ? std::nullopt : zettai_image_attributes(payload);
            const auto reordered_attributes = masked_attributes ? masked_attributes : harami_attributes ? harami_attributes
                                              : oshioki_attributes ? oshioki_attributes : zettai_attributes ? zettai_attributes : fuka_image_attributes(payload);
            if (reordered_attributes)
            {
                const std::string variant = masked_attributes ? "masked" : harami_attributes ? "harami"
                                             : oshioki_attributes ? "oshioki" : zettai_attributes ? "zettai" : "fuka";
                file["image_encoding"] = variant + "_bitmap";
                file["image_orientation"] = "top_down";
                file[variant + "_attributes"] = *reordered_attributes;
                const auto opacity = decode_image_alpha(canonical_image(payload, format), {});
                bool separate = false;
                for (size_t pixel = 0; pixel < opacity.bgra.size(); pixel += 4)
                    separate = separate || opacity.bgra[pixel] != opacity.bgra[pixel + 1] ||
                               opacity.bgra[pixel] != opacity.bgra[pixel + 2];
                if (separate)
                {
                    const fs::path path(utf16(file.at("path").get<std::string>()));
                    const auto alpha_path = path.parent_path() / (path.stem().wstring() + L".alpha.png");
                    write_file(project / alpha_path, encode_png(opacity));
                    file["channel_alpha"] = utf8(std::u16string(alpha_path.u16string()));
                }
                retain_native_packet(file);
                continue;
            }
            const auto attributes = compact_image_attributes(payload);
            const auto older_attributes = format == DataFormat::Cp932 && !attributes
                                              ? legacy_image_attributes(payload)
                                              : std::optional<std::array<uint32_t, 20>>{};
            std::optional<Image> native_alpha;
            if ((attributes || older_attributes) && format == DataFormat::Cp932 && bucket == "images")
            {
                auto opacity = bitmap_alpha.get(id);
                for (size_t pixel = 0; pixel < opacity.bgra.size(); pixel += 4)
                {
                    if (opacity.bgra[pixel] != opacity.bgra[pixel + 1] ||
                        opacity.bgra[pixel] != opacity.bgra[pixel + 2])
                    {
                        native_alpha = std::move(opacity);
                        break;
                    }
                }
            }
            payload = canonical_image(payload, format);
            file["image_flag"] = payload.size() > 5 && payload[0] == 'c' ? payload[5] : 0;
            if (older_attributes)
            {
                file["image_encoding"] = "legacy_bitmap";
                file["legacy_attributes"] = *older_attributes;
                if (native_alpha)
                {
                    const fs::path path(utf16(file.at("path").get<std::string>()));
                    const auto alpha_path = path.parent_path() / (path.stem().wstring() + L".alpha.png");
                    write_file(project / alpha_path, encode_png(*native_alpha));
                    file["channel_alpha"] = utf8(std::u16string(alpha_path.u16string()));
                }
                retain_native_packet(file);
                continue;
            }
            if (payload.size() >= 33 && (payload[0] == 'a' || payload[0] == 'd'))
            {
                Cursor reader{payload};
                reader.u8();
                const auto base = reader.u32();
                const auto parameter = reader.u32();
                const auto width = reader.u32();
                const auto height = reader.u32();
                const auto flags = reader.u32();
                const auto alpha = reader.u32();
                reader.take(alpha);
                const auto mask = reader.u32();
                reader.take(mask);
                const auto color = reader.u32();
                if (!flags && !alpha && !mask && color == uint64_t(width) * height * 4 && !attributes && !older_attributes)
                {
                    file["image_encoding"] = "native_bgra32";
                    file["image_validation"] = "UNTESTED_ON_GAME_DATA";
                    file["image_base"] = base;
                    file["image_parameter"] = parameter;
                }
                else if (!flags && !alpha && !mask && color == uint64_t(width) * height)
                {
                    file["image_encoding"] = "grayscale";
                    file["image_base"] = base;
                    file["image_parameter"] = parameter;
                    if (attributes)
                    {
                        file["compact_attributes"] = *attributes;
                    }
                }
                else if (attributes && format == DataFormat::Cp932)
                {
                    file["image_encoding"] = "compact_bitmap";
                    file["compact_attributes"] = *attributes;
                    if (native_alpha)
                    {
                        const auto path = fs::path(utf16(file.at("path").get<std::string>()));
                        const auto alpha_path = utf8(std::u16string(
                            (path.parent_path() / (path.stem().wstring() + L".alpha.png")).u16string()));
                        write_file(project / fs::path(utf16(alpha_path)), encode_png(*native_alpha));
                        file["channel_alpha"] = alpha_path;
                    }
                }
            }
            retain_native_packet(file);
        }
    }
    if (wrapper_headers.size()==1) manifest["image_wrapper_header"] = *wrapper_headers.begin();
    const auto config = decode_config(read_file(game / "Arc00.dat"));
    Json structure = {{"data_format", data_format_name(format)},
                      {"strings", Json::array()},
                      {"blocks", Json::array()},
                      {"setting_keys", Json::array()}};
    if (config.contains("settings_storage"))
    {
        structure["settings_storage"] = config.at("settings_storage");
    }
    if (config.contains("compact_settings_terminated"))
    {
        structure["compact_settings_terminated"] = config.at("compact_settings_terminated");
    }
    for (size_t index = 0; index < config.at("strings").size(); ++index)
    {
        const auto& string = config.at("strings")[index];
        if (index == 0)
        {
            structure["strings"].push_back({{"title", true}});
            continue;
        }
        bool mapped = false;
        for (const auto& field : line_breaking_fields)
        {
            if (field.string_index != index)
            {
                continue;
            }
            const auto value = decode_line_breaking_value(string, field);
            if (value)
            {
                structure["strings"].push_back(
                    {{"line_breaking", field.name}, {"suffix", value->suffix}});
                mapped = true;
            }
        }
        if (mapped)
        {
            continue;
        }
        const auto bytes = unhex(string.at("hex"));
        bool readable = string.contains("text");
        if (readable)
        {
            for (const char16_t value : utf16(string.at("text")))
            {
                readable =
                    readable && (value >= 32 || value == '\n' || value == '\r' || value == '\t');
            }
        }
        const auto path = "raw/config/string_" + padded(index, 2) + (readable ? ".txt" : ".bin");
        if (readable)
        {
            write_text(project / path, string.at("text"));
        }
        else
        {
            write_file(project / path, bytes);
        }
        structure["strings"].push_back(
            {{"path", path}, {"encoding", readable ? "utf8" : "binary"}});
    }
    for (size_t index = 1; index < config.at("blocks").size(); ++index)
    {
        const auto path = "raw/config/ui_" + padded(index, 2) + ".bin";
        write_file(project / path, unhex(config.at("blocks")[index]));
        structure["blocks"].push_back(path);
    }
    for (const auto& setting : config.at("settings"))
    {
        structure["setting_keys"].push_back(setting.at("key"));
    }
    for (size_t index = 0; index < config.at("strings").size(); ++index)
    {
        if (config["strings"][index].contains("native_suffix"))
        {
            structure["strings"][index]["native_suffix"] =
                config["strings"][index]["native_suffix"];
        }
    }
    manifest["config_structure"] = structure;
    manifest["resource_parts"] = Json::array();
    Cursor layout_reader{resources.index};
    layout_reader.u32();
    for (size_t number = 0; number < resources.parts.size(); ++number)
    {
        const auto& part = resources.parts[number];
        const auto count = layout_reader.count(16);
        layout_reader.take(size_t(count) * 16);
        const size_t start = layout_reader.pos;
        const auto groups = layout_reader.count(16);
        for (uint32_t group = 0; group < groups; ++group)
        {
            layout_reader.take(12);
            const auto records = layout_reader.u32();
            if (records > 1)
            {
                layout_reader.take(size_t(records - 1) * 4);
            }
            layout_reader.take(size_t(records) * 12);
        }
        if (layout_reader.u32() == 1)
        {
            layout_reader.u32();
            const auto pairs = layout_reader.count(8);
            layout_reader.take(size_t(pairs) * 8);
        }
        const size_t end = layout_reader.pos;
        const auto path = "raw/resources/layout_" + padded(number, 3) + ".bin";
        write_file(project / path, std::span(resources.index).subspan(start, end - start));
        Json ids = Json::array();
        for (const auto& entry : part.entries)
        {
            ids.push_back(entry.id);
        }
        manifest["resource_parts"].push_back(
            {{"number", number}, {"entries", ids}, {"layout", path}});
    }
    layout_reader.finish();
    auto scenarios = read_arc_index(game / "Arc01.dat", game / "Arc04.dat").entries;
    for (auto& entry : scenarios)
    {
        if (entry.kind == 'f' && entry.subtype == '0')
            entry.payload = entry_payload(entry, game / "Arc04.dat");
        if (entry.kind == 'a' && entry.subtype == '0')
        {
            entry.payload = entry_payload(entry, game / "Arc04.dat");
            entry.payload =
                canonical_scenario(entry.payload, entry.id, format, tunnel, entry.version);
        }
    }
    const auto group_tables = graphics_group_tables(scenarios);
    manifest.erase("voice_channels");
    write_text(project / "project.json", manifest.dump(2));
    const auto references = asset_references(project, manifest);
    for (const auto& file : manifest.at("files"))
    {
        if (file.at("type") == "timeline" || file.at("type") == "native_page")
        {
            const auto path = local_path(project, file.at("path"));
            const auto document = Json::parse(read_text(path));
            auto readable = file.at("type") == "timeline"
                                      ? readable_graphics_timeline(document, references.path)
                                      : readable_native_page(document, references.path);
            if (file.at("type") == "native_page")
            {
                const auto profile = manifest.at("page_parameters").at("id").get<std::string>();
                readable = name_page_parameters(readable, profile);
                if (!profile.empty() && !readable.contains("parameter_profile"))
                {
                    manifest["diagnostics"].push_back(
                        {{"archive", "scripts"}, {"entry", file.at("entry")},
                         {"message", "Page layout differs from the verified native parameter profile; names remain neutral."}});
                }
            }
            write_text(path, readable.dump(2));
        }
    }
    write_text(project / "config.json", readable_config(config, references.path).dump(2));
    for (const auto& entry : scenarios)
    {
        if ((entry.kind == 'a' || entry.kind == 'f') && entry.subtype == '0')
        {
            if (entry.kind == 'f')
            {
                const auto document = decode_graphics_group(entry.payload, entry.version, group_tables);
                const auto readable = readable_graphics_group(document, references.path);
                write_text(project / "graphics" / (padded(entry.id) + ".json"), readable.dump(2));
            }
            else
            {
                write_text(project / "scenarios" / (padded(entry.id) + ".cscript"),
                           decompile_script({entry}, references.path, references.resolve));
            }
        }
    }
    write_text(project / "project.json", manifest.dump(2));
}

Json build_standalone_project(const fs::path& project, const fs::path& destination)
{
    if (!fs::is_regular_file(project / "project.json"))
    {
        throw Error("project.json is required; decompile the game again with this toolkit");
    }
    const auto manifest = Json::parse(read_text(project / "project.json"));
    if (manifest.value("format", "") != project_format)
    {
        throw Error("unsupported project format; decompile the game again with this toolkit");
    }
    const auto format = parse_data_format(manifest.at("data_format"));
    const auto tunnel =
        format == DataFormat::Cp932 ? project_sjis_tunnel_table(manifest) : std::u16string{};
    const auto staging = fs::path(destination.wstring() + L".building");
    validate_output_directory(project, destination);
    validate_output_directory(project, staging);
    report_progress("Compiling independent source documents...");
    auto scripts = compile_script_documents(project, manifest);
    auto config = import_config_structure(project, manifest.at("config_structure"));
    const auto references = asset_references(project, manifest);
    config =
        merge_config(Json::parse(read_text(project / "config.json")), config, references.resolve);
    std::map<std::string, std::map<uint32_t, Json>> files;
    std::map<uint32_t, uint32_t> scenario_versions;
    for (const auto& archive : manifest.at("archives"))
    {
        if (archive.at("name") == "scripts")
        {
            for (const auto& entry : archive.at("entries"))
            {
                if (entry.at("kind") == "a" && entry.at("subtype") == "0")
                {
                    scenario_versions[entry.at("id").get<uint32_t>()] = entry.at("version");
                }
            }
        }
    }
    for (const auto& file : manifest.at("files"))
    {
        if (!files[file.at("archive")].emplace(file.at("entry"), file).second)
        {
            throw Error("duplicate project asset");
        }
    }
    std::map<std::pair<std::string, uint32_t>, std::pair<Bytes, std::string>> preserved_packets;
    auto payload = [&](const std::string& bucket, uint32_t id)
    {
        check_cancelled();
        if (bucket == "scripts" && scripts.contains(id))
        {
            auto bytes = std::move(scripts.at(id));
            scripts.erase(id);
            if (scenario_versions.contains(id))
            {
                return native_scenario(bytes, format, tunnel, scenario_versions.at(id));
            }
            return bytes;
        }
        if (!files[bucket].contains(id))
        {
            throw Error("missing " + bucket + " entry " + std::to_string(id));
        }
        const auto file = files[bucket].at(id);
        files[bucket].erase(id);
        if (auto packet = preserved_image_packet(project, file))
        {
            const auto& source = file.at("native_source");
            auto decoded = source.at("compression") == "lzss"
                               ? lzss_decode(*packet, source.at("size")) : *packet;
            if (decoded.size() != source.at("size")) throw Error("native image size differs");
            preserved_packets[{bucket, id}] = {std::move(*packet), source.at("compression")};
            return decoded;
        }
        return import_asset(project, file, format,
                            manifest.value("image_wrapper_header",format==DataFormat::Cp932 ? 5u : 6u),
                            references.resolve);
    };
    remove_output_directory(project, staging);
    fs::create_directories(staging);
    if (manifest.at("has_resources").get<bool>())
    {
        fs::create_directories(staging / "Resources");
    }
    write_file(staging / "Arc00.dat", encode_config(config));
    size_t built = 0;
    for (const auto& archive : manifest.at("archives"))
    {
        report_progress("Building " + archive.at("content").get<std::string>());
        const auto layout = parse_archive_index_layout(archive.at("index_layout"));
        std::map<uint8_t, std::ofstream> content_parts;
        ArchivePosition cursor;
        std::vector<Entry> index;
        std::map<uint32_t, Entry> originals;
        std::vector<uint32_t> original_order;
        std::set<uint8_t> preserved_parts;
        if (archive.contains("preserved_sources"))
        {
            const auto source_root = project / "raw/archive_sources";
            std::set<std::string> retained;
            for (const auto& source : archive.at("preserved_sources"))
            {
                check_cancelled();
                const auto path = utf8(fs::path(utf16(source.at("path"))).generic_u16string());
                if (!retained.insert(path).second ||
                    source.at("sha256") != file_hash(local_path(project, path)))
                    throw Error("preserved archive source is missing or changed: " + path);
            }
            const auto original = read_arc_index(local_path(source_root, archive.at("index")),
                                                  local_path(source_root, archive.at("content")));
            if (original.layout != layout || original.entries.size() != archive.at("entries").size() ||
                !retained.erase("raw/archive_sources/" + archive.at("index").get<std::string>()))
                throw Error("preserved archive index differs from project metadata");
            preserved_parts.insert(0);
            for (const auto& entry : original.entries)
            {
                originals[entry.id] = entry;
                original_order.push_back(entry.id);
                preserved_parts.insert(entry.part);
            }
            for (const auto part : preserved_parts)
            {
                const auto name = archive_part_path(archive.at("content").get<std::string>(), part);
                if (!retained.erase("raw/archive_sources/" + name.string()))
                    throw Error("preserved archive part is not registered");
                fs::copy_file(source_root / name, staging / name, fs::copy_options::overwrite_existing);
            }
            if (!retained.empty()) throw Error("unexpected preserved archive source");
            cursor = {*preserved_parts.rbegin(), INT32_MAX};
        }
        for (const auto& entry : archive.at("entries"))
        {
            const auto kind = entry.at("kind").get<std::string>();
            const auto subtype = entry.at("subtype").get<std::string>();
            const auto version = entry.at("version").get<unsigned>();
            if (kind.size() != 1 || subtype.size() != 1 || (version != 22 && version != 23) ||
                entry.at("part").get<unsigned>() > 26 || (version == 22 && entry.at("part") != 0))
            {
                throw Error("unsupported archive entry metadata");
            }
            if (!originals.empty())
            {
                const auto id = entry.at("id").get<uint32_t>();
                if (!originals.contains(id)) throw Error("entry is absent from preserved archive index");
                const auto& original = originals.at(id);
                // Slot order is observable to the engine, including b/NUL slots.
                if (original_order.at(index.size()) != id || original.version != version ||
                    original.kind != kind[0] || original.subtype != subtype[0] ||
                    original.reserved != entry.at("reserved") || original.part != entry.at("part"))
                    throw Error("preserved archive entry metadata differs");
                if (index_only_archive_entry(original))
                {
                    if (!entry.value("index_only", false)) throw Error("synthetic bitmap slot metadata is missing");
                    index.push_back(original);
                    ++built;
                    continue;
                }
            }
            else if (entry.value("index_only", false))
                throw Error("synthetic bitmap slots require preserved archive sources");
            const auto bytes = payload(archive.at("name"), entry.at("id"));
            const bool opaque = version == 22 && kind == "b" && subtype == std::string(1, '\0');
            const auto compression =
                !opaque ? entry.at("compression").get<std::string>() : "none";
            if (compression != "none" && compression != "lzss" && compression != "none_zero_size")
            {
                throw Error("unsupported archive compression");
            }
            Entry built_entry;
            built_entry.version = version;
            built_entry.kind = kind[0];
            built_entry.subtype = subtype[0];
            built_entry.audio_prefix_size = entry.value("audio_prefix_size", -1);
            built_entry.leading = unhex(entry.value("leading_data", ""));
            if (!built_entry.leading.empty() && built_entry.leading.size() != 44)
                throw Error("unsupported audio storage header");
            const size_t prefix = archive_entry_prefix_size(built_entry);
            if (bytes.size() < prefix) throw Error("music entry is missing loop fields");
            if (compression == "none_zero_size" && version != 23)
            {
                throw Error("zero-size stored entries require a version-23 archive");
            }
            if (prefix && compression == "lzss")
            {
                throw Error("compressed music entries are not supported by this format profile");
            }
            const auto key = std::pair(archive.at("name").get<std::string>(), entry.at("id").get<uint32_t>());
            const auto stored = preserved_packets.contains(key) &&
                                        preserved_packets.at(key).second == compression
                                    ? std::move(preserved_packets.at(key).first)
                                 : compression == "lzss" ? lzss_encode(bytes) : bytes;
            preserved_packets.erase(key);
            const auto size = checked32(bytes.size() - prefix);
            if (!originals.empty())
            {
                const auto& original = originals.at(entry.at("id").get<uint32_t>());
                if (original.size == (compression == "none_zero_size" ? 0 : size) &&
                    original.packed_size == stored.size() - prefix &&
                    read_range(archive_part_path(local_path(project / "raw/archive_sources", archive.at("content")),
                                                 original.part), original.offset, original.packed_size) == stored)
                {
                    index.push_back(original);
                    ++built;
                    continue;
                }
            }
            const auto position = reserve_archive_space(cursor, entry.at("part"), stored.size() + built_entry.leading.size());
            const auto part = position.part;
            if (version == 22 && part != 0)
            {
                throw Error("version-22 archives cannot split into additional content parts");
            }
            if (!content_parts.contains(part))
            {
                content_parts[part].open(
                    archive_part_path(local_path(staging, archive.at("content")), part),
                    std::ios::binary);
            }
            auto& content = content_parts.at(part);
            built_entry.id = entry.at("id");
            built_entry.size = size;
            built_entry.packed_size = checked32(stored.size() - prefix);
            if (compression == "none_zero_size")
            {
                if (!built_entry.packed_size) throw Error("zero-size stored entry is empty");
                built_entry.size = 0;
            }
            built_entry.offset = checked32(position.offset + built_entry.leading.size());
            built_entry.reserved = entry.at("reserved");
            built_entry.part = part;
            if (opaque_archive_entry(built_entry))
            {
                built_entry.size = entry.at("decoded_size");
            }
            index.push_back(built_entry);
            stream_bytes(content, built_entry.leading);
            stream_bytes(content, stored);
            ++built;
        }
        if (!content_parts.contains(0) && !preserved_parts.contains(0))
        {
            content_parts[0].open(local_path(staging, archive.at("content")), std::ios::binary);
        }
        for (auto& [part, content] : content_parts)
        {
            finish_stream(content);
        }
        write_file(local_path(staging, archive.at("index")), encode_arc_index(index, layout));
    }
    Bytes resource_index;
    put(resource_index, checked32(manifest.at("resource_parts").size()));
    unsigned expected_part = 0;
    for (const auto& part : manifest.at("resource_parts"))
    {
        const auto number = part.at("number").get<unsigned>();
        if (number != expected_part++)
        {
            throw Error("resource parts must be contiguous");
        }
        const auto name = "Data" + padded(number, 3) + ".dat";
        report_progress("Building Resources/" + name);
        std::ofstream content(staging / "Resources" / name, std::ios::binary);
        put(resource_index, checked32(part.at("entries").size()));
        uint64_t offset = 0;
        uint32_t ordinal = 0;
        for (const auto& entry : part.at("entries"))
        {
            if (entry.get<uint32_t>() != (number + 1) * 10000000 + ordinal++)
            {
                throw Error("resource IDs must match their part and position");
            }
            const auto bytes = payload("resources", entry);
            put(resource_index, 0);
            put(resource_index, checked32(bytes.size()));
            put(resource_index, offset, 8);
            stream_bytes(content, bytes);
            offset += bytes.size();
            ++built;
        }
        finish_stream(content);
        append(resource_index, read_file(local_path(project, part.at("layout"))));
    }
    if (!scripts.empty())
    {
        throw Error("source entry is absent from archive metadata");
    }
    for (const auto& [bucket, remaining] : files)
    {
        if (!remaining.empty())
        {
            throw Error("asset is absent from archive metadata: " + bucket);
        }
    }
    if (manifest.at("has_resources").get<bool>())
    {
        write_file(staging / "Resources/Data00.dat", resource_index);
    }
    else if (!manifest.at("resource_parts").empty())
    {
        throw Error("resource entries are present without a Resources archive");
    }
    for (const auto& file : manifest.at("files"))
    {
        if (file.at("type") == "external_audio")
        {
            auto path = file.at("engine_path").get<std::string>();
            std::replace(path.begin(), path.end(), '\\', '/');
            write_file(local_path(staging, path), read_file(local_path(project, file.at("path"))));
        }
    }
    const Json report = {{"standalone", true},
                         {"built_entries", built},
                         {"archive_checks", inspect_game(staging)},
                         {"runtime_tested", false}};
    write_text(staging / "build-report.json", report.dump(2));
    remove_output_directory(project, destination);
    fs::rename(staging, destination);
    return report;
}
}
