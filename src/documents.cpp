#include "documents.hpp"
#include "graphics.hpp"
#include "progress.hpp"
#include "references.hpp"
#include <algorithm>
#include <cctype>

namespace csystem
{
namespace
{
struct ConfigField
{
    const char* group;
    const char* name;
    const char* key;
    const char* resource_role = nullptr;
};

constexpr ConfigField config_fields[] = {
    {"display", "width", "W:"},
    {"display", "height", "H:"},
    {"dialogue", "font_size", "F:"},
    {"dialogue", "character_spacing", "X:"},
    {"dialogue", "line_spacing", "Y:"},
    {"dialogue", "max_lines", "M:"},
    {"dialogue", "window_resource", "T:"},
    {"dialogue", "mode", "GM:"},
    {"text_area", "left", "AL:"},
    {"text_area", "top", "AT:"},
    {"text_area", "right", "AR:"},
    {"text_area", "bottom", "AB:"},
    {"narration_area", "left", "NL:"},
    {"narration_area", "top", "NT:"},
    {"narration_area", "right", "NR:"},
    {"narration_area", "bottom", "NB:"},
    {"narration", "max_lines", "NM:"},
    {"events", "right_click_scenario", "RC:", "scene"},
    {"events", "wheel_up_scenario", "WH:", "scene"},
    {"events", "default_scenario", "DS:", "scene"},
    {"variables", "global_count", "GV:"},
    {"variables", "local_last_index", "NV:"},
    {"variables", "array_count", "AV:"},
    {"save", "format_version", "VR:"},
    {"dialogue", "glyph_padding", "N:"},
    {"continue_indicator", "animation", "C:", "indicator_animation"},
    {"continue_indicator", "dialogue_position_mode", "TC:"},
    {"continue_indicator", "dialogue_x", "TX:"},
    {"continue_indicator", "dialogue_y", "TY:"},
    {"continue_indicator", "narration_position_mode", "NC:"},
    {"continue_indicator", "narration_x", "NX:"},
    {"continue_indicator", "narration_y", "NY:"},
    {"text_colors", "primary_red", "FR:"},
    {"text_colors", "primary_green", "FG:"},
    {"text_colors", "primary_blue", "FB:"},
    {"text_colors", "secondary_red", "BR:"},
    {"text_colors", "secondary_green", "BG:"},
    {"text_colors", "secondary_blue", "BB:"},
    {"graphics", "layer_count", "L:"},
    {"graphics", "image_flag_extension_base", "BV:"},
    {"choice", "panel_image", "P:", "resource"},
    {"choice", "header_image", "SH:", "resource"},
    {"choice", "centered_bottom_limit", "SY:"},
    {"choice", "hover_image", "S:", "resource"},
    {"choice", "normal_image", "US:", "resource"},
    {"choice", "hover_sound", "SS:", "sfx"}};

bool has_readable_name(const std::string& key)
{
    return std::any_of(std::begin(config_fields),
                       std::end(config_fields),
                       [&](const ConfigField& field)
                       {
                           return field.key == key;
                       });
}

Json config_document(const Json& decoded, const ResourcePathLookup& paths)
{
    Json result = {{"format", config_format},
                   {"game", {{"title", decoded.at("strings").at(0).at("text")}}}};
    for (const auto& field : line_breaking_fields)
    {
        if (field.string_index >= decoded.at("strings").size())
        {
            continue;
        }
        const auto value =
            decode_line_breaking_value(decoded.at("strings")[field.string_index], field);
        if (value)
        {
            result["line_breaking"][field.name] = value->characters;
        }
    }
    for (const auto& field : config_fields)
    {
        for (const auto& setting : decoded.at("settings"))
        {
            if (setting.at("key") != field.key)
            {
                continue;
            }
            auto value = setting.at("value");
            if (paths && field.resource_role)
            {
                const auto path = paths(field.resource_role, -1, value.get<int32_t>());
                if (!path.empty())
                {
                    value = path;
                }
            }
            result[field.group][field.name] = value;
        }
    }
    result["engine_parameters"] = Json::object();
    for (const auto& setting : decoded.at("settings"))
    {
        const auto key = setting.at("key").get<std::string>();
        if (!has_readable_name(key))
        {
            result["engine_parameters"][key] = setting.at("value");
        }
    }
    return result;
}

fs::path source_path(const fs::path& project, const std::string& relative)
{
    const fs::path path(utf16(relative));
    if (path.empty() || path.is_absolute() || path.has_root_name())
    {
        throw Error("invalid source document path");
    }
    for (const auto& segment : path)
    {
        if (segment == "..")
        {
            throw Error("source document path escapes the project");
        }
    }
    return project / path;
}

void check_keys(const Json& edited, const Json& expected, const std::string& path)
{
    if (!expected.is_object())
    {
        return;
    }
    if (!edited.is_object() || edited.size() != expected.size())
    {
        throw Error("configuration fields do not match at " + path);
    }
    for (const auto& [key, value] : expected.items())
    {
        if (!edited.contains(key))
        {
            throw Error("configuration is missing " + path + "." + key);
        }
        check_keys(edited.at(key), value, path + "." + key);
    }
}

std::string javascript_filename(const Entry& entry, const JavaScript& script)
{
    std::string name;
    for (const unsigned char character : utf8(script.name))
    {
        if (std::isalnum(character) && character < 128)
        {
            name += static_cast<char>(character);
        }
        else if (character == '_' || character == '-')
        {
            name += static_cast<char>(character);
        }
        else
        {
            name += '_';
        }
    }
    name.resize(std::min(name.size(), size_t(80)));
    return padded(entry.id) + (name.empty() ? "" : "_" + name) + ".js";
}
}

std::optional<LineBreakingValue> decode_line_breaking_value(const Json& string,
                                                            const LineBreakingField& field)
{
    if (!string.contains("text") || !string.at("text").is_string())
    {
        return std::nullopt;
    }
    const auto text = string.at("text").get<std::string>();
    const std::string prefix = field.prefix;
    if (!text.starts_with(prefix))
    {
        return std::nullopt;
    }
    size_t end = text.size();
    while (end > prefix.size() && (text[end - 1] == '\r' || text[end - 1] == '\n'))
    {
        --end;
    }
    const auto characters = text.substr(prefix.size(), end - prefix.size());
    for (const auto character : utf16(characters))
    {
        if (character < 32 || (character >= 127 && character <= 159))
        {
            return std::nullopt;
        }
    }
    return LineBreakingValue{characters, text.substr(end)};
}

std::string indent_javascript(const std::string& source)
{
    if (source.find('`') != std::string::npos)
    {
        return source;
    }
    enum class State
    {
        Code,
        SingleQuote,
        DoubleQuote,
        BlockComment
    };
    State state = State::Code;
    bool escaped = false;
    char previous = '\0';
    size_t depth = 0;
    size_t start = 0;
    std::string result;
    while (start < source.size())
    {
        const auto end = source.find('\n', start);
        const auto length = (end == std::string::npos ? source.size() : end + 1) - start;
        const auto line = source.substr(start, length);
        const auto first = line.find_first_not_of(" \t\r\n");
        if (state == State::Code && first != std::string::npos)
        {
            const auto indent = line[first] == '}' && depth > 0 ? depth - 1 : depth;
            result.append(std::min(indent, size_t(128)) * 4, ' ');
            result += line.substr(first);
        }
        else
        {
            result += line;
        }
        for (size_t index = 0; index < line.size(); ++index)
        {
            const char character = line[index];
            const char next = index + 1 < line.size() ? line[index + 1] : '\0';
            if (state == State::BlockComment)
            {
                if (character == '*' && next == '/')
                {
                    state = State::Code;
                    ++index;
                }
                continue;
            }
            if (state == State::SingleQuote || state == State::DoubleQuote)
            {
                if (escaped)
                {
                    if (character != '\r')
                    {
                        escaped = false;
                    }
                }
                else if (character == '\\')
                {
                    escaped = true;
                }
                else if ((state == State::SingleQuote && character == '\'') ||
                         (state == State::DoubleQuote && character == '"'))
                {
                    state = State::Code;
                    previous = character;
                }
                continue;
            }
            if (character == '/' && next == '/')
            {
                break;
            }
            if (character == '/' && next == '*')
            {
                state = State::BlockComment;
                ++index;
            }
            else if (character == '/')
            {
                if (!std::isalnum(static_cast<unsigned char>(previous)) && previous != ')' &&
                    previous != ']' && previous != '\'' && previous != '"')
                {
                    return source;
                }
            }
            else if (character == '\'')
            {
                state = State::SingleQuote;
            }
            else if (character == '"')
            {
                state = State::DoubleQuote;
            }
            else if (character == '{')
            {
                ++depth;
            }
            else if (character == '}' && depth > 0)
            {
                --depth;
            }
            if (!std::isspace(static_cast<unsigned char>(character)))
            {
                previous = character;
            }
        }
        start += length;
    }
    return result;
}

Json export_script_documents(const fs::path& project, const std::vector<Entry>& entries)
{
    const auto group_tables = graphics_group_tables(entries);
    Json documents = Json::array();
    for (const auto& entry : entries)
    {
        check_cancelled();
        if (entry.subtype != '0')
        {
            continue;
        }
        std::string directory;
        switch (entry.kind)
        {
        case 'a':
        case 'B':
        case 'C':
            directory = "scenarios";
            break;
        case 'f':
        {
            const auto document = decode_graphics_group(entry.payload, entry.version, group_tables);
            if (encode_graphics_group(document) != entry.payload)
            {
                throw Error("graphics round-trip mismatch at entry " + std::to_string(entry.id));
            }
            const auto relative = "graphics/" + padded(entry.id) + ".json";
            write_text(source_path(project, relative), document.dump(2));
            documents.push_back({{"path", relative}, {"entry", entry.id}, {"type", "graphics"}});
            continue;
        }
        case 's':
        {
            const auto script = decode_javascript(entry.payload);
            if (encode_javascript(script) != entry.payload)
            {
                throw Error("JavaScript round-trip mismatch at entry " + std::to_string(entry.id));
            }
            const auto relative = "scripts/javascript/" + javascript_filename(entry, script);
            const auto source = indent_javascript(utf8(script.source));
            write_text(source_path(project, relative), source);
            documents.push_back({{"path", relative},
                                 {"entry", entry.id},
                                 {"type", "javascript"},
                                 {"id", script.id},
                                 {"name", utf8(script.name)},
                                 {"dependencies", script.dependencies}});
            continue;
        }
        default:
            continue;
        }
        const auto relative = directory + "/" + padded(entry.id) + ".cscript";
        const auto source = decompile_script({entry});
        write_text(source_path(project, relative), source);
        documents.push_back({{"path", relative}, {"entry", entry.id}});
    }
    return documents;
}

PayloadMap compile_script_documents(const fs::path& project, const Json& manifest)
{
    if (manifest.value("format", "") != project_format)
    {
        throw Error("unsupported project format; decompile the game again with this toolkit");
    }
    PayloadMap result;
    const auto references = asset_references(project, manifest);
    for (const auto& document : manifest.at("sources"))
    {
        check_cancelled();
        const auto relative = document.at("path").get<std::string>();
        const auto expected_id = document.at("entry").get<uint32_t>();
        try
        {
            if (document.value("type", "cscript") == "graphics")
            {
                const auto graphics = Json::parse(read_text(source_path(project, relative)));
                if (!result.emplace(expected_id, encode_graphics_group(graphics, references.resolve))
                         .second)
                {
                    throw Error("duplicate entry across source documents");
                }
                continue;
            }
            if (document.value("type", "cscript") == "javascript")
            {
                const JavaScript script{document.at("id").get<uint32_t>(),
                                        utf16(document.at("name").get<std::string>()),
                                        utf16(read_text(source_path(project, relative))),
                                        document.at("dependencies").get<std::vector<uint32_t>>()};
                if (!result.emplace(expected_id, encode_javascript(script)).second)
                {
                    throw Error("duplicate entry across source documents");
                }
                continue;
            }
            if (document.value("type", "cscript") != "cscript")
            {
                throw Error("unsupported source document type");
            }
            auto compiled =
                compile_script(read_text(source_path(project, relative)), references.resolve);
            if (compiled.size() != 1 || !compiled.contains(expected_id))
            {
                throw Error("document must declare only its assigned entry " +
                            std::to_string(expected_id));
            }
            if (!result.emplace(expected_id, std::move(compiled.at(expected_id))).second)
            {
                throw Error("duplicate entry across source documents");
            }
        }
        catch (const Error& error)
        {
            throw Error(relative + ": " + error.what());
        }
        catch (const Json::exception& error)
        {
            throw Error(relative + ": " + error.what());
        }
    }
    return result;
}

Json readable_config(const Json& decoded, const ResourcePathLookup& paths)
{
    return config_document(decoded, paths);
}

Json merge_config(const Json& readable, const Json& original, const ResourceResolver& resolver)
{
    if (readable.value("format", "") != config_format)
    {
        throw Error("unsupported config format; decompile the game again with this toolkit");
    }
    check_keys(readable, readable_config(original), "config.json");
    Json result = original;
    result.at("strings").at(0)["text"] = readable.at("game").at("title").get<std::string>();
    for (const auto& field : line_breaking_fields)
    {
        if (field.string_index >= result.at("strings").size())
        {
            continue;
        }
        auto& string = result.at("strings")[field.string_index];
        const auto original_value = decode_line_breaking_value(string, field);
        if (!original_value)
        {
            continue;
        }
        const auto& value = readable.at("line_breaking").at(field.name);
        if (!value.is_string())
        {
            throw Error("config.json.line_breaking." + std::string(field.name) +
                        " must be a string");
        }
        const auto characters = value.get<std::string>();
        const std::string text = field.prefix + characters;
        const auto checked = decode_line_breaking_value(Json{{"text", text}}, field);
        if (!checked || checked->characters != characters)
        {
            throw Error("config.json.line_breaking." + std::string(field.name) +
                        " must contain characters without line breaks or control codes");
        }
        string["text"] = text + original_value->suffix;
    }
    for (auto& setting : result.at("settings"))
    {
        const auto key = setting.at("key").get<std::string>();
        const auto field = std::find_if(std::begin(config_fields),
                                        std::end(config_fields),
                                        [&](const ConfigField& item)
                                        {
                                            return item.key == key;
                                        });
        Json value = field == std::end(config_fields) ? readable.at("engine_parameters").at(key)
                                                      : readable.at(field->group).at(field->name);
        if (value.is_string() && field != std::end(config_fields) && field->resource_role)
        {
            if (!resolver)
            {
                throw Error("configuration resource path requires project metadata: " + key);
            }
            value = resolver(field->resource_role, -1, value.get<std::string>());
        }
        if (!value.is_number_integer() ||
            (value.is_number_unsigned() && value.get<uint64_t>() > INT32_MAX) ||
            value.get<int64_t>() < INT32_MIN || value.get<int64_t>() > INT32_MAX)
        {
            throw Error("configuration setting " + key + " must be a signed 32-bit integer");
        }
        setting["value"] = value;
    }
    return result;
}

}
