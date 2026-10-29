#include "tool.hpp"
#include "documents.hpp"
#include "legacy.hpp"
#include "progress.hpp"
#include "standalone.hpp"
#include <algorithm>

namespace csystem
{
namespace
{
bool scenario_entry(const Entry& entry)
{
    return entry.kind == 'a' && entry.subtype == '0';
}

fs::path scenario_path(uint32_t id)
{
    return fs::path("scenarios") / (padded(id) + ".cscript");
}

void copy_native(const fs::path& source, const fs::path& destination)
{
    check_cancelled();
    fs::create_directories(destination.parent_path());
    fs::copy_file(source, destination, fs::copy_options::overwrite_existing);
}

std::set<uint8_t> script_parts(const std::vector<Entry>& entries)
{
    std::set<uint8_t> parts{0};
    for (const auto& entry : entries) parts.insert(entry.part);
    return parts;
}
}

Json export_scenario_project(const fs::path& game, const fs::path& project)
{
    const auto config = decode_config(read_file(game / "Arc00.dat"));
    const auto format = parse_data_format(config.value("data_format", "utf16"));
    const auto tunnel = format == DataFormat::Cp932 ? read_sjis_tunnel_table(game / "sjis_ext.bin")
                                                   : std::u16string{};
    const auto index = read_arc_index(game / "Arc01.dat", game / "Arc04.dat");
    report_progress("Decompiling scenarios and config only...");
    remove_output_directory(game, project);
    fs::create_directories(project);
    Json manifest = {{"format", project_format}, {"mode", "scenario"},
                     {"data_format", data_format_name(format)},
                     {"sources", Json::array()}, {"native_files", Json::array()}};
    if (!tunnel.empty()) manifest["sjis_tunnel"] = {{"characters", utf8(tunnel)}};
    auto retain = [&](const fs::path& name)
    {
        copy_native(game / name, project / "native" / name);
        manifest["native_files"].push_back({{"path", utf8(name.u16string())},
                                             {"sha256", sha256(read_file(project / "native" / name))}});
    };
    retain("Arc00.dat");
    retain("Arc01.dat");
    for (const auto part : script_parts(index.entries)) retain(archive_part_path("Arc04.dat", part));
    for (auto entry : index.entries)
    {
        check_cancelled();
        if (!scenario_entry(entry)) continue;
        entry.payload = canonical_scenario(entry_payload(entry, game / "Arc04.dat"),
                                          entry.id, format, tunnel, entry.version);
        const auto source = decompile_script({entry});
        const auto compiled = compile_script(source);
        if (compiled.size() != 1 || compiled.at(entry.id) != entry.payload)
            throw Error("scenario round-trip failed: " + std::to_string(entry.id));
        write_text(project / scenario_path(entry.id), source);
        manifest["sources"].push_back({{"entry", entry.id},
                                      {"path", utf8(scenario_path(entry.id).u16string())}});
    }
    write_text(project / "config.json", readable_config(config).dump(2));
    write_text(project / "project.json", manifest.dump(2));
    const Json report = {{"mode", "scenario"}, {"scenarios", manifest["sources"].size()},
                         {"images", 0}, {"audio", 0}, {"diagnostics", Json::array()},
                         {"scenario_roundtrip_verified", true}, {"independent_project", true},
                         {"media_exported", false}, {"runtime_tested", false}};
    write_text(project / "export-report.json", report.dump(2));
    return report;
}

Json build_scenario_project(const fs::path& project, const fs::path& destination)
{
    const auto manifest = Json::parse(read_text(project / "project.json"));
    if (manifest.value("format", "") != project_format || manifest.value("mode", "") != "scenario")
        throw Error("unsupported scenario project; decompile again with this toolkit");
    const auto native = project / "native";
    auto staging = destination;
    staging += L".building";
    for (const auto& target : {destination, staging})
    {
        validate_output_directory(project, target);
        validate_output_directory(native, target);
    }
    const auto index = read_arc_index(native / "Arc01.dat", native / "Arc04.dat");
    const auto parts = script_parts(index.entries);
    std::set<std::string> expected{"Arc00.dat", "Arc01.dat"};
    for (const auto part : parts) expected.insert(archive_part_path("Arc04.dat", part).string());
    for (const auto& file : manifest.at("native_files"))
    {
        const auto name = file.at("path").get<std::string>();
        if (!expected.erase(name) || file.at("sha256") != sha256(read_file(native / name)))
            throw Error("original script data is missing or changed: " + name);
    }
    if (!expected.empty()) throw Error("scenario project is missing original script data");
    const auto original_config_bytes = read_file(native / "Arc00.dat");
    const auto original_config = decode_config(original_config_bytes);
    const auto format = parse_data_format(manifest.at("data_format"));
    if (format != parse_data_format(original_config.value("data_format", "utf16")))
        throw Error("scenario encoding differs from the original configuration");
    const auto tunnel = project_sjis_tunnel_table(manifest);
    const auto config = merge_config(Json::parse(read_text(project / "config.json")), original_config);
    std::set<uint32_t> declared;
    for (const auto& source : manifest.at("sources"))
    {
        const auto id = source.at("entry").get<uint32_t>();
        if (!declared.insert(id).second || source.at("path") != utf8(scenario_path(id).u16string()))
            throw Error("invalid scenario source metadata");
    }
    size_t count = 0, changed = 0;
    auto entries = index.entries;
    remove_output_directory(project, staging);
    fs::create_directories(staging);
    for (const auto part : parts)
        copy_native(archive_part_path(native / "Arc04.dat", part),
                    archive_part_path(staging / "Arc04.dat", part));
    ArchivePosition cursor{*parts.rbegin(), fs::file_size(archive_part_path(native / "Arc04.dat", *parts.rbegin()))};
    report_progress("Compiling scenarios and config only...");
    for (auto& entry : entries)
    {
        check_cancelled();
        if (!scenario_entry(entry)) continue;
        if (!declared.erase(entry.id)) throw Error("missing scenario source metadata");
        const auto scripts = compile_script(read_text(project / scenario_path(entry.id)));
        if (scripts.size() != 1 || !scripts.contains(entry.id))
            throw Error("scenario source must contain its original ID: " + std::to_string(entry.id));
        const auto original = entry_payload(entry, native / "Arc04.dat");
        ++count;
        if (scripts.at(entry.id) == canonical_scenario(original, entry.id, format, tunnel, entry.version))
            continue;
        const auto bytes = native_scenario(scripts.at(entry.id), format, tunnel, entry.version);
        const auto compression = archive_entry_compression(entry);
        const auto stored = compression == "lzss" ? lzss_encode(bytes) : bytes;
        const auto position = reserve_archive_space(cursor, entry.part, stored.size());
        if (entry.version == 22 && position.part != 0)
            throw Error("version-22 script archive exceeds its single-part limit");
        std::ofstream stream(archive_part_path(staging / "Arc04.dat", position.part),
                             std::ios::binary | std::ios::app);
        if (!stream.write(reinterpret_cast<const char*>(stored.data()), std::streamsize(stored.size())))
            throw Error("scenario archive write failed");
        stream.close();
        if (!stream) throw Error("scenario archive close failed");
        entry.size = compression == "none_zero_size" ? 0 : checked32(bytes.size());
        entry.packed_size = checked32(stored.size());
        entry.offset = checked32(position.offset);
        entry.part = position.part;
        ++changed;
    }
    if (!declared.empty()) throw Error("scenario source is absent from the original archive");
    write_file(staging / "Arc00.dat", config == original_config ? original_config_bytes : encode_config(config));
    if (changed) write_file(staging / "Arc01.dat", encode_arc_index(entries, index.layout));
    else copy_native(native / "Arc01.dat", staging / "Arc01.dat");
    const Json report = {{"mode", "scenario"}, {"patch_only", true},
                         {"scenarios", count}, {"changed_scenarios", changed},
                         {"config_changed", config != original_config}, {"runtime_tested", false}};
    write_text(staging / "build-report.json", report.dump(2));
    remove_output_directory(project, destination);
    fs::rename(staging, destination);
    return report;
}
}
