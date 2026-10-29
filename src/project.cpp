#include "audio.hpp"
#include "documents.hpp"
#include "standalone.hpp"
#include "progress.hpp"
#include "tool.hpp"
#include "legacy.hpp"
#include "image_profiles.hpp"
#include "graphics.hpp"
#include "page_parameters.hpp"
#include "image_cache.hpp"
#include "jpeg.hpp"
#include <algorithm>
#include <chrono>
#include <deque>
#include <iostream>

namespace csystem
{
namespace
{
std::vector<std::string> game_files(const fs::path& game, const Resources& resources)
{
    std::vector<std::string> files = {"Arc00.dat"};
    for (const auto& pair : game_archives(game))
    {
        files.push_back(pair.index);
        files.push_back(pair.content);
        std::set<uint8_t> parts;
        for (const auto& entry : read_arc_index(game / pair.index, game / pair.content).entries)
        {
            if (entry.part && parts.insert(entry.part).second)
            {
                files.push_back(archive_part_path(pair.content, entry.part).string());
            }
        }
    }
    if (resources.present)
    {
        files.push_back("Resources/Data00.dat");
    }
    for (const auto& part : resources.parts)
    {
        files.push_back("Resources/Data" + padded(part.number, 3) + ".dat");
    }
    return files;
}

bool editable_script_entry(const Entry& entry)
{
    return entry.subtype == '0' && (entry.kind == 'a' || entry.kind == 's' || entry.kind == 'B' ||
                                    entry.kind == 'C' || entry.kind == 'f');
}

fs::path project_path(const fs::path& root, const std::string& relative)
{
    const fs::path path(utf16(relative));
    if (path.empty() || path.is_absolute() || path.has_root_name())
    {
        throw Error("project path must be relative");
    }
    for (const auto& part : path)
    {
        if (part == "..")
        {
            throw Error("project path cannot escape the project");
        }
    }
    return root / path;
}

uint32_t image_base(std::span<const uint8_t> payload)
{
    if (payload.empty() || (payload[0] != 'a' && payload[0] != 'd'))
    {
        return 0;
    }
    Cursor reader{payload};
    reader.u8();
    const auto base = reader.u32();
    reader.take(12);
    const auto flags = reader.u32();
    const auto alpha_size = reader.u32();
    if (flags & 4)
    {
        reader.take(alpha_size);
    }
    return reader.u32() ? base : 0;
}

std::vector<uint32_t> bitmap_entry_ids(const std::vector<Entry>& entries)
{
    std::vector<uint32_t> ids;
    for (const auto& entry : entries)
    {
        if (entry.kind == 'b' && (entry.subtype == '0' || opaque_archive_entry(entry) ||
                                  index_only_archive_entry(entry)))
        {
            ids.push_back(entry.id);
        }
    }
    return ids;
}

void add_file(Json& manifest,
              const fs::path& project,
              const std::string& path,
              std::span<const uint8_t> content,
              const std::string& bucket,
              uint32_t id,
              const std::string& type,
              uint32_t base = 0)
{
    write_file(project_path(project, path), content);
    manifest["files"].push_back({{"path", path},
                                 {"sha256", sha256(content)},
                                 {"archive", bucket},
                                 {"entry", id},
                                 {"type", type},
                                 {"base_image", base}});
}

}

Json inspect_game(const fs::path& game)
{
    Resources resources(game / "Resources");
    const auto config = decode_config(read_file(game / "Arc00.dat"));
    Json result = {{"engine", "Cyberworks C,system"},
                   {"archive_format", nullptr},
                   {"data_format", config.value("data_format", "utf16")},
                   {"archives", Json::array()},
                   {"resources", Json::object()}};
    for (const auto& pair : game_archives(game))
    {
        const auto index = read_arc_index(game / pair.index, game / pair.content);
        const auto& entries = index.entries;
        const auto version = entries.empty() ? 23 : entries.front().version;
        if (result.at("archives").empty())
        {
            result["archive_format"] = version;
        }
        else if (result.at("archive_format") != version)
        {
            result["archive_format"] = nullptr;
        }
        Json types = Json::object();
        for (const auto& entry : entries)
        {
            const std::string type = std::string(1, entry.kind) + entry.subtype;
            types[type] = types.value(type, 0) + 1;
        }
        result["archives"].push_back({{"index", pair.index},
                                      {"content", pair.content},
                                      {"index_layout", archive_index_layout_name(index.layout)},
                                      {"index_version", version},
                                      {"record_size", version == 22 ? 26 : 27},
                                      {"entries", entries.size()},
                                      {"types", types}});
    }
    size_t count = 0;
    for (const auto& part : resources.parts)
    {
        count += part.entries.size();
    }
    result["resources"] = {
        {"present", resources.present}, {"parts", resources.parts.size()}, {"entries", count}};
    result["page_parameters"] = detect_page_parameter_profile(game);
    return result;
}

Json export_project(const fs::path& game, const fs::path& project, bool media)
{
    const auto archives = game_archives(game);
    Resources resources(game / "Resources");
    const auto config = decode_config(read_file(game / "Arc00.dat"));
    const auto format = parse_data_format(config.value("data_format", "utf16"));
    const auto tunnel = format == DataFormat::Cp932 ? read_sjis_tunnel_table(game / "sjis_ext.bin")
                                                    : std::u16string{};
    auto script_entries = read_arc_index(game / "Arc01.dat", game / "Arc04.dat").entries;
    for (auto& entry : script_entries)
    {
        entry.payload = entry_payload(entry, game / "Arc04.dat");
        if (entry.kind == 'a' && entry.subtype == '0')
        {
            entry.payload =
                canonical_scenario(entry.payload, entry.id, format, tunnel, entry.version);
        }
    }
    report_progress("Decompiling scripts...");
    remove_output_directory(game, project);
    fs::create_directories(project);
    Json manifest = {{"format", project_format},
                     {"engine", "Cyberworks C,system"},
                     {"data_format", data_format_name(format)},
                     {"has_resources", resources.present},
                     {"files", Json::array()},
                     {"media_exported", media},
                     {"diagnostics", Json::array()}};
    manifest["page_parameters"] = detect_page_parameter_profile(game);
    manifest["sources"] = export_script_documents(project, script_entries);
    write_text(project / "config.json", readable_config(config).dump(2));
    for (const auto& entry : script_entries)
    {
        if (!editable_script_entry(entry))
        {
            if (entry.subtype == '0' && std::string("mer").find(entry.kind) != std::string::npos)
            {
                try
                {
                    const auto document = entry.kind == 'm' ? decode_graphics_timeline(entry.payload, entry.version)
                                          : entry.kind == 'e' ? decode_native_page(entry.payload, entry.version,
                                                                                  manifest.at("page_parameters").at("id"))
                                                              : decode_motion_preset(entry.payload);
                    const std::string directory = entry.kind == 'm' ? "graphics/timelines/"
                                                  : entry.kind == 'e' ? "ui/pages/"
                                                                      : "graphics/motions/";
                    const std::string type = entry.kind == 'm' ? "timeline"
                                             : entry.kind == 'e' ? "native_page" : "motion";
                    const auto path = directory + padded(entry.id) + ".json";
                    const auto source = document.dump(2);
                    add_file(manifest, project, path,
                             std::span(reinterpret_cast<const uint8_t*>(source.data()), source.size()),
                             "scripts", entry.id, type);
                    continue;
                }
                catch (const Error& error)
                {
                    manifest["diagnostics"].push_back(
                        {{"archive", "scripts"}, {"entry", entry.id}, {"message", error.what()}});
                }
            }
            const auto path = "raw/scripts/" + padded(entry.id) + "." + entry.kind + entry.subtype;
            add_file(manifest, project, path, entry.payload, "scripts", entry.id, "raw");
        }
    }

    auto image_entries = read_arc_index(game / "Arc02.dat", game / "Arc05.dat").entries;
    const auto bitmap_ids = bitmap_entry_ids(image_entries);
    const auto image_profile = media ? select_image_profile(image_entries, game / "Arc05.dat") : ImageProfileContext{};
    if (!image_profile.id.empty())
    {
        manifest["experimental_image_profile"] = {{"id", image_profile.id},
            {"validation", "UNTESTED_ON_GAME_DATA"}, {"runtime_tested", false}};
        report_progress("Experimental image profile: UNTESTED on game data/runtime.");
    }
    std::map<uint32_t, Entry> image_lookup;
    for (const auto& entry : image_entries)
    {
        image_lookup[entry.id] = entry;
    }
    ImageCache archive_images(
        [&](uint32_t id)
        {
            if (!image_lookup.contains(id))
            {
                throw Error("missing Arc05 image base " + std::to_string(id));
            }
            return canonical_image(
                entry_payload(image_lookup.at(id), game / "Arc05.dat"), format, bitmap_ids, &image_profile, id);
        });
    size_t exported_images = 0;
    for (const auto& entry : image_entries)
    {
        if (index_only_archive_entry(entry)) continue;
        auto payload = entry_payload(entry, game / "Arc05.dat");
        check_cancelled();
        bool decoded = false;
        if (media && !opaque_archive_entry(entry) && !payload.empty() &&
            (std::string("abcd").find(char(payload[0])) != std::string::npos || is_tb1(payload) ||
             profile_image_metadata(payload, image_profile.id)))
        {
            try
            {
                const auto jpeg = unwrap_jpeg(payload);
                const auto image = archive_images.get(entry.id);
                const auto bytes = jpeg ? Bytes(jpeg->begin(), jpeg->end()) : encode_png(image);
                add_file(manifest,
                         project,
                         "images/arc05/" + padded(entry.id) + (jpeg ? ".jpg" : ".png"),
                         bytes,
                         "images",
                         entry.id,
                         jpeg ? "jpg" : "png",
                         image_base(canonical_image(payload, format, bitmap_ids, &image_profile, entry.id)));
                decoded = true;
                ++exported_images;
                if (exported_images % 100 == 0)
                    report_progress("Images exported: " + std::to_string(exported_images));
                if (const auto alpha = legacy22_channel_alpha(payload))
                {
                    const auto path = "images/arc05/" + padded(entry.id) + ".alpha.png";
                    write_file(project_path(project, path), encode_png(*alpha));
                    manifest["files"].back()["channel_alpha"] = path;
                }
            }
            catch (const Error& error)
            {
                manifest["diagnostics"].push_back(
                    {{"archive", "images"}, {"entry", entry.id}, {"message", error.what()}});
            }
        }
        if (!decoded)
        {
            add_file(manifest,
                     project,
                     "raw/images/" + padded(entry.id) + "." + entry.kind +
                         (entry.subtype == '\0' ? ".bin" : std::string(1, entry.subtype)),
                     payload,
                     "images",
                     entry.id,
                     "raw");
        }
    }

    size_t exported_audio = 0;
    for (const auto& entry : read_arc_index(game / "Arc03.dat", game / "Arc06.dat").entries)
    {
        check_cancelled();
        const auto payload = entry_payload(entry, game / "Arc06.dat");
        bool decoded = false;
        if (media && entry.version == 22 && entry.kind == 'h' && entry.subtype == '0')
        {
            const auto native_path = decode_cp932(payload);
            auto normalized = native_path;
            std::replace(normalized.begin(), normalized.end(), '\\', '/');
            const auto external = project_path(game, normalized);
            if (fs::is_regular_file(external) && encode_cp932(native_path) == payload)
            {
                add_file(manifest,
                         project,
                         "audio/music/" + padded(entry.id) + "_h0" + external.extension().string(),
                         read_file(external),
                         "audio",
                         entry.id,
                         "external_audio");
                manifest["files"].back()["engine_path"] = native_path;
                decoded = true;
                ++exported_audio;
            }
        }
        if (!decoded && (entry.kind == 'i' || entry.kind == 'h') && entry.subtype == '0')
        {
            add_file(manifest,
                     project,
                     "raw/audio/" + padded(entry.id) + "." + entry.kind + "0",
                     payload,
                     "audio",
                     entry.id,
                     "raw");
            decoded = true;
        }
        if (media && !decoded)
        {
            try
            {
                const bool wave =
                    payload.size() >= 4 && std::equal(payload.begin(), payload.begin() + 4, "RIFF");
                if (wave)
                {
                    validate_wave(payload);
                    add_file(manifest,
                             project,
                             "audio/sfx/" + padded(entry.id) + "_" + entry.kind + entry.subtype +
                                 ".wav",
                             payload,
                             "audio",
                             entry.id,
                             "wav");
                    decoded = true;
                    ++exported_audio;
                }
                else
                {
                    const auto audio = decode_audio(payload);
                    if (encode_audio(audio.ogg, audio) != payload)
                    {
                        throw Error("audio codec round-trip mismatch");
                    }
                    add_file(manifest,
                             project,
                             "audio/" +
                                 std::string(entry.kind == 'j'   ? "voice/"
                                             : entry.kind == 'u' ? "music/"
                                                                 : "sfx/") +
                                 padded(entry.id) + "_" + entry.kind + entry.subtype + ".ogg",
                             audio.ogg,
                             "audio",
                             entry.id,
                             "ogg");
                    decoded = true;
                    ++exported_audio;
                }
            }
            catch (const Error& error)
            {
                manifest["diagnostics"].push_back(
                    {{"archive", "audio"}, {"entry", entry.id}, {"message", error.what()}});
            }
        }
        if (!decoded)
        {
            add_file(manifest,
                     project,
                     "raw/audio/" + padded(entry.id) + "." + entry.kind + entry.subtype,
                     payload,
                     "audio",
                     entry.id,
                     "raw");
        }
        if (entry.id % 500 == 0)
        {
            report_progress("Audio entries processed through ID: " + std::to_string(entry.id));
        }
    }

    ImageCache resource_images(
        [&](uint32_t id)
        {
            return resources.payload(id);
        });
    size_t processed = 0;
    for (const auto& part : resources.parts)
    {
        for (const auto& entry : part.entries)
        {
            check_cancelled();
            if (media)
            {
                try
                {
                    const auto payload = resources.payload(entry.id);
                    const auto jpeg = unwrap_jpeg(payload);
                    const auto image = resource_images.get(entry.id);
                    const auto bytes = jpeg ? Bytes(jpeg->begin(), jpeg->end()) : encode_png(image);
                    add_file(manifest,
                             project,
                             "images/resources/" + std::to_string(entry.id) + (jpeg ? ".jpg" : ".png"),
                             bytes,
                             "resources",
                             entry.id,
                             jpeg ? "jpg" : "png",
                             image_base(payload));
                    ++exported_images;
                }
                catch (const Error& error)
                {
                    manifest["diagnostics"].push_back(
                        {{"archive", "resources"}, {"entry", entry.id}, {"message", error.what()}});
                    add_file(manifest,
                             project,
                             "raw/resources/" + std::to_string(entry.id) + ".bin",
                             resources.payload(entry.id),
                             "resources",
                             entry.id,
                             "raw");
                }
            }
            if (!media)
            {
                add_file(manifest,
                         project,
                         "raw/resources/" + std::to_string(entry.id) + ".bin",
                         resources.payload(entry.id),
                         "resources",
                         entry.id,
                         "raw");
            }
            ++processed;
            if (processed % 100 == 0)
            {
                report_progress("Resources processed: " + std::to_string(processed));
            }
        }
    }
    size_t exported_videos = 0;
    size_t processed_videos = 0;
    for (const auto& archive : archives)
    {
        if (archive.key != "videos") continue;
        report_progress("Exporting archived videos...");
        for (const auto& entry : read_arc_index(game / archive.index, game / archive.content).entries)
        {
            check_cancelled();
            auto payload = entry_payload(entry, game / archive.content);
            if (media && entry.kind == 'y' && entry.subtype == '0')
            {
                transform_archive_video(payload);
                add_file(manifest, project, "videos/" + padded(entry.id) + archive_video_extension(payload),
                         payload, "videos", entry.id, "video");
                ++exported_videos;
            }
            else
                add_file(manifest, project, "raw/videos/" + padded(entry.id) + ".bin",
                         payload, "videos", entry.id, "raw");
            report_progress("Video entries processed: " + std::to_string(++processed_videos));
        }
    }
    export_build_metadata(game, project, manifest);
    const Json report = {{"scenarios",
                          std::count_if(script_entries.begin(),
                                        script_entries.end(),
                                        [](const Entry& entry)
                                        {
                                            return entry.kind == 'a';
                                        })},
                         {"images", exported_images},
                         {"audio", exported_audio},
                         {"videos", exported_videos},
                         {"resource_entries", processed},
                         {"diagnostics", manifest["diagnostics"]},
                         {"warnings", manifest.value("warnings",Json::array())},
                         {"experimental_image_profile",manifest.value("experimental_image_profile",Json::object())},
                         {"scenario_roundtrip_verified", true},
                         {"standalone_project", true},
                         {"page_parameters", manifest.at("page_parameters")},
                         {"runtime_tested", false}};
    write_text(project / "export-report.json", report.dump(2));
    return report;
}

Json build_project(const fs::path& project, const fs::path& destination)
{
    const auto manifest = Json::parse(read_text(project / "project.json"));
    if (manifest.value("mode", "full") == "scenario")
        return build_scenario_project(project, destination);
    return build_standalone_project(project, destination);
}

Json verify_project(const fs::path& original, const fs::path& rebuilt)
{
    Resources resources(original / "Resources");
    Resources rebuilt_resources(rebuilt / "Resources");
    Json files = Json::array();
    bool identical = true;
    const auto original_files = game_files(original, resources);
    const auto rebuilt_files = game_files(rebuilt, rebuilt_resources);
    std::set<std::string> names(original_files.begin(), original_files.end());
    names.insert(rebuilt_files.begin(), rebuilt_files.end());
    for (const auto& name : names)
    {
        const auto original_path = project_path(original, name);
        const auto rebuilt_path = project_path(rebuilt, name);
        const auto before = fs::is_regular_file(original_path) ? file_hash(original_path) : "";
        const auto after = fs::is_regular_file(rebuilt_path) ? file_hash(rebuilt_path) : "";
        identical = identical && before == after;
        files.push_back({{"path", name}, {"identical", before == after}, {"sha256", after}});
    }
    const auto original_config = decode_config(read_file(original / "Arc00.dat"));
    const auto rebuilt_config = decode_config(read_file(rebuilt / "Arc00.dat"));
    bool equivalent = original_config == rebuilt_config;
    const auto original_format = parse_data_format(original_config.value("data_format", "utf16"));
    const auto rebuilt_format = parse_data_format(rebuilt_config.value("data_format", "utf16"));
    equivalent = equivalent && resources.present == rebuilt_resources.present;
    Json archives = Json::array();
    auto pairs = game_archives(original);
    for (const auto& pair : game_archives(rebuilt))
        if (std::none_of(pairs.begin(), pairs.end(), [&](const auto& value) { return value.key == pair.key; }))
            pairs.push_back(pair);
    for (const auto& pair : pairs)
    {
        report_progress("Verifying decoded " + pair.content);
        if (!fs::is_regular_file(original / pair.index) || !fs::is_regular_file(rebuilt / pair.index))
        {
            equivalent = false;
            archives.push_back({{"content", pair.content}, {"equivalent", false},
                                {"mismatch", {{"reason", "archive pair is missing"}}}});
            continue;
        }
        const auto before_index = read_arc_index(original / pair.index, original / pair.content);
        const auto after_index = read_arc_index(rebuilt / pair.index, rebuilt / pair.content);
        const auto& before = before_index.entries;
        const auto& after = after_index.entries;
        const bool layout_matches = before_index.layout == after_index.layout;
        bool matches = before.size() == after.size() && layout_matches;
        Json mismatch = nullptr;
        bool preserved_content = true;
        if (std::any_of(before.begin(), before.end(), index_only_archive_entry))
        {
            std::set<uint8_t> parts{0};
            for (const auto& entry : before) parts.insert(entry.part);
            for (const auto part : parts)
            {
                const auto source = archive_part_path(original / pair.content, part);
                const auto target = archive_part_path(rebuilt / pair.content, part);
                if (!fs::is_regular_file(target) || fs::file_size(source) != fs::file_size(target))
                {
                    preserved_content = false;
                    break;
                }
                for (uint64_t offset = 0, size = fs::file_size(source); offset < size; )
                {
                    check_cancelled();
                    const auto count = size_t(std::min(size - offset, uint64_t(1024 * 1024)));
                    if (read_range(source, offset, count) != read_range(target, offset, count))
                    {
                        preserved_content = false;
                        break;
                    }
                    offset += count;
                }
                if (!preserved_content) break;
            }
            if (!preserved_content)
            {
                matches = false;
                mismatch = {{"reason", "synthetic bitmap backing content differs"}};
            }
        }
        std::map<uint32_t, Entry> original_entries;
        std::map<uint32_t, Entry> rebuilt_entries;
        for (const auto& entry : before)
        {
            original_entries[entry.id] = entry;
        }
        for (const auto& entry : after)
        {
            rebuilt_entries[entry.id] = entry;
        }
        const auto original_bitmap_ids = bitmap_entry_ids(before);
        const auto rebuilt_bitmap_ids = bitmap_entry_ids(after);
        const auto original_profile = pair.key == "images" ? select_image_profile(before, original / pair.content) : ImageProfileContext{};
        const auto rebuilt_profile = pair.key == "images" ? select_image_profile(after, rebuilt / pair.content) : ImageProfileContext{};
        ImageCache original_images(
            [&](uint32_t id)
            {
                return canonical_image(
                    entry_payload(original_entries.at(id), original / pair.content),
                    original_format,
                    original_bitmap_ids, &original_profile, id);
            });
        ImageCache rebuilt_images(
            [&](uint32_t id)
            {
                return canonical_image(
                    entry_payload(rebuilt_entries.at(id), rebuilt / pair.content),
                    rebuilt_format,
                    rebuilt_bitmap_ids, &rebuilt_profile, id);
            });
        ImageCache original_alpha([&](uint32_t id) {
            return canonical_image(entry_payload(original_entries.at(id), original / pair.content),
                                   original_format, original_bitmap_ids, &original_profile, id);
        }, true);
        ImageCache rebuilt_alpha([&](uint32_t id) {
            return canonical_image(entry_payload(rebuilt_entries.at(id), rebuilt / pair.content),
                                   rebuilt_format, rebuilt_bitmap_ids, &rebuilt_profile, id);
        }, true);
        for (size_t index = 0; matches && index < before.size(); ++index)
        {
            check_cancelled();
            const auto& left = before[index];
            const auto& right = after[index];
            if (left.id != right.id || left.kind != right.kind || left.subtype != right.subtype ||
                left.version != right.version || left.reserved != right.reserved ||
                archive_entry_prefix_size(left) != archive_entry_prefix_size(right) || left.leading != right.leading ||
                (!opaque_archive_entry(left) &&
                 archive_entry_compression(left) != archive_entry_compression(right)) ||
                (opaque_archive_entry(left) &&
                 (left.size != right.size || left.packed_size != right.packed_size)))
            {
                matches = false;
                mismatch = {{"id", left.id}, {"reason", "entry metadata differs"}};
                break;
            }
            if (index_only_archive_entry(left))
            {
                if (left.size != right.size || left.packed_size != right.packed_size ||
                    left.offset != right.offset || left.part != right.part)
                {
                    matches = false;
                    mismatch = {{"id", left.id}, {"reason", "synthetic bitmap slot differs"}};
                }
                continue;
            }
            const auto left_bytes = entry_payload(left, original / pair.content);
            const auto right_bytes = entry_payload(right, rebuilt / pair.content);
            if (left_bytes == right_bytes)
            {
                continue;
            }
            if (pair.key == "scripts" && left.kind == 'a' && left.subtype == '0')
            {
                matches =
                    canonical_scenario(left_bytes, left.id, original_format, {}, left.version) ==
                    canonical_scenario(right_bytes, right.id, rebuilt_format, {}, right.version);
            }
            else if (pair.key == "scripts" && left.kind == 's' && left.subtype == '0')
            {
                auto left_script = decode_javascript(left_bytes);
                auto right_script = decode_javascript(right_bytes);
                left_script.source = utf16(indent_javascript(utf8(left_script.source)));
                right_script.source = utf16(indent_javascript(utf8(right_script.source)));
                matches = encode_javascript(left_script) == encode_javascript(right_script);
            }
            else if (pair.key == "images")
            {
                const auto left_image = original_images.get(left.id);
                const auto right_image = rebuilt_images.get(right.id);
                matches = left_image.width == right_image.width &&
                          left_image.height == right_image.height &&
                          left_image.bgra == right_image.bgra;
                if (matches && (left.version == 22 || original_format == DataFormat::Cp932))
                {
                    const auto original_opacity = original_alpha.get(left.id);
                    const auto rebuilt_opacity = rebuilt_alpha.get(right.id);
                    matches = original_opacity.bgra == rebuilt_opacity.bgra;
                    if (!matches)
                    {
                        mismatch = {{"id", left.id}, {"reason", "RGB alpha differs"}};
                        if (original_opacity.bgra.size() == rebuilt_opacity.bgra.size())
                        {
                            for (size_t at = 0; at < original_opacity.bgra.size(); ++at)
                                if (original_opacity.bgra[at] != rebuilt_opacity.bgra[at])
                                {
                                    mismatch["pixel"] = at / 4;
                                    mismatch["channel"] = at % 4;
                                    mismatch["original"] = original_opacity.bgra[at];
                                    mismatch["rebuilt"] = rebuilt_opacity.bgra[at];
                                    break;
                                }
                        }
                    }
                }
            }
            else
            {
                matches = false;
            }
            if (!matches && mismatch.is_null())
                mismatch = {{"id", left.id}, {"reason", "decoded content differs"}};
        }
        equivalent = equivalent && matches;
        archives.push_back({{"content", pair.content},
                            {"index_layout", archive_index_layout_name(after_index.layout)},
                            {"index_layout_equivalent", layout_matches},
                            {"equivalent", matches},
                            {"mismatch", mismatch},
                            {"entries", before.size()}});
    }
    ImageCache original_images(
        [&](uint32_t id)
        {
            return resources.payload(id);
        });
    ImageCache rebuilt_images(
        [&](uint32_t id)
        {
            return rebuilt_resources.payload(id);
        });
    bool resource_match = resources.parts.size() == rebuilt_resources.parts.size();
    size_t checked = 0;
    for (size_t part = 0; resource_match && part < resources.parts.size(); ++part)
    {
        report_progress("Verifying resource pixels, part " + std::to_string(part));
        resource_match =
            resources.parts[part].entries.size() == rebuilt_resources.parts[part].entries.size();
        for (const auto& entry : resources.parts[part].entries)
        {
            check_cancelled();
            if (!resource_match)
            {
                break;
            }
            if (resources.payload(entry.id) != rebuilt_resources.payload(entry.id))
            {
                const auto left = original_images.get(entry.id);
                const auto right = rebuilt_images.get(entry.id);
                resource_match = left.width == right.width && left.height == right.height &&
                                 left.bgra == right.bgra;
            }
            ++checked;
        }
    }
    equivalent = equivalent && resource_match;
    auto original_layout = resources.index;
    auto rebuilt_layout = rebuilt_resources.index;
    for (const auto& part : resources.parts)
    {
        for (const auto& entry : part.entries)
        {
            std::fill_n(original_layout.begin() + entry.index_position, 16, uint8_t{0});
        }
    }
    for (const auto& part : rebuilt_resources.parts)
    {
        for (const auto& entry : part.entries)
        {
            std::fill_n(rebuilt_layout.begin() + entry.index_position, 16, uint8_t{0});
        }
    }
    const bool layout_match = original_layout == rebuilt_layout;
    equivalent = equivalent && layout_match;
    return {{"all_identical", identical},
            {"all_equivalent", equivalent},
            {"files", files},
            {"archives", archives},
            {"resource_images_checked", checked},
            {"resource_images_equivalent", resource_match},
            {"resource_layout_equivalent", layout_match},
            {"runtime_tested", false}};
}
}
