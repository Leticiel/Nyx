#include "page_parameters.hpp"
#include <algorithm>
#include <cctype>

namespace csystem
{
namespace
{
struct Profile
{
    const char* id;
    const char* format;
    std::vector<const char*> element_names;
    std::vector<const char*> option_names;
};

const std::vector<Profile>& profiles()
{
    static const std::vector<Profile> values = {
        {"native-page-1-4-options", "csystem-native-page-1",
         {"result_value", "activation_scenario", "snapshot_on_activate",
          "block_during_selection", "initially_selected", "hover_sound"},
         {"right_click_result", "wheel_up_result", "wheel_down_result", "input_map_image"}},
        {"native-page-5-8-options", "csystem-native-page-5",
         {"result_value", "activation_scenario", "snapshot_on_activate",
          "block_during_selection", "initially_selected", "hover_sound", "auto_mode_filter"},
         {"pointer_bounds_top", "pointer_bounds_left", "pointer_bounds_bottom", "pointer_bounds_right",
          "right_click_result", "wheel_up_result", "wheel_down_result", "input_map_image"}},
        {"native-page-3-9-options", "csystem-native-page-3",
         {"result_value", "activation_scenario", "snapshot_on_activate",
          "block_during_selection", "exclude_when_page_filter_active", "hover_sound", "auto_mode_filter",
          "input_code"},
         {"filter_marked_elements", "pointer_bounds_top", "pointer_bounds_left", "pointer_bounds_bottom", "pointer_bounds_right",
          "right_click_result", "wheel_up_result", "wheel_down_result", "input_map_image"}},
        {"native-page-4-9-options", "csystem-native-page-4",
         {"result_value", "activation_scenario", "snapshot_on_activate",
          "block_during_selection", "exclude_when_page_filter_active", "hover_sound", "auto_mode_filter",
          "input_code"},
         {"filter_marked_elements", "pointer_bounds_top", "pointer_bounds_left", "pointer_bounds_bottom", "pointer_bounds_right",
          "right_click_result", "wheel_up_result", "wheel_down_result", "input_map_image"}},
        {"native-page-4-10-options", "csystem-native-page-4",
         {"result_value", "activation_scenario", "snapshot_on_activate",
          "block_during_selection", "exclude_when_page_filter_active", "hover_sound", "auto_mode_filter",
          "input_code"},
         {"filter_marked_elements", "hover_hand_cursor", "pointer_bounds_top", "pointer_bounds_left",
          "pointer_bounds_bottom", "pointer_bounds_right", "right_click_result", "wheel_up_result",
          "wheel_down_result", "input_map_image"}},
    };
    return values;
}

const Profile& profile_by_id(const std::string& id)
{
    const auto& values = profiles();
    const auto found = std::find_if(values.begin(), values.end(),
                                    [&](const auto& value) { return value.id == id; });
    if (found == values.end())
    {
        throw Error("unknown native page parameter profile: " + id);
    }
    return *found;
}

constexpr const char* element_wire_names[] = {
    "result_value", "parameter_1", "parameter_2", "parameter_3", "parameter_4", "hover_sound",
    "parameter_6", "input_code"};

void rename_field(Json& object, const std::string& from, const std::string& to)
{
    if (from == to || !object.contains(from))
    {
        return;
    }
    if (object.contains(to))
    {
        throw Error("native page contains both " + from + " and " + to);
    }
    // Preserve the original wire ordering in the readable document.
    Json renamed = Json::object();
    for (const auto& [key, value] : object.items())
    {
        renamed[key == from ? to : key] = value;
    }
    object = std::move(renamed);
}

void check_shape(const Json& document, const Profile& profile)
{
    if (document.at("format") != profile.format ||
        !document.at("options").is_object() ||
        document.at("options").size() != profile.option_names.size())
    {
        throw Error("native page layout does not match parameter profile " + std::string(profile.id));
    }
}
}

std::string page_parameter_profile_for_hash(const std::string& digest)
{
    std::string lower = digest;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char value) { return char(std::tolower(value)); });
    if (lower == "6b38ebaaf28598e3d8e3d1e3bee55eb1390f1aaa47f98568dbee7afaa7bc0657")
    {
        return "native-page-3-9-options";
    }
    if (lower == "31baa847ef93811bc60b4c96b4abb9836ff043bbac0ac3810103e5ed3343940e")
    {
        return "native-page-4-9-options";
    }
    if (lower == "7c140cf388185ced60cef1c2e39a14fdcd950bfb9f1826baf8611b00ab17ccea")
    {
        return "native-page-1-4-options";
    }
    if (lower == "9bcb7075979aa78f711f4d85160cfef48cab66d634e080320c35d31f08964a2e" ||
        lower == "26c85e929a8fc7a0b6e844e8405d8dbde9b944c73fd0d92b370582bcd9475585")
    {
        return "native-page-5-8-options";
    }
    if (lower == "87e54e9c1eaf8bc0efdec36d3c712a483c4adaa28ba94274289bb4d9dfadf6f6" ||
        lower == "f9e9d2e8ad5b1800d729eac9ff061de400eec7438967b6dce6f4e3681cc58191" ||
        lower == "978184c069e7a5004ec72778d36ab1594c74c479912df0e8066f5381275b5df5")
    {
        return "native-page-4-10-options";
    }
    return {};
}

PageParameterLayout page_parameter_layout(const std::string& id)
{
    const auto& profile = profile_by_id(id);
    return {profile.format, profile.option_names.size()};
}

std::string page_parameter_image_fingerprint(std::span<const uint8_t> image)
{
    const auto range = [&](size_t offset, size_t size)
    {
        if (offset > image.size() || size > image.size() - offset)
        {
            throw Error("truncated PE image while identifying native UI parameters");
        }
        return image.subspan(offset, size);
    };
    const auto word = [&](size_t offset)
    {
        Cursor reader{range(offset, 2)};
        return reader.u16();
    };
    const auto dword = [&](size_t offset)
    {
        Cursor reader{range(offset, 4)};
        return reader.u32();
    };
    if (word(0) != 0x5a4d)
    {
        return {};
    }
    const size_t pe = dword(60);
    if (dword(pe) != 0x4550)
    {
        return {};
    }
    const auto count = word(pe + 6);
    const auto optional_size = word(pe + 20);
    if (count == 0 || count > 96 || optional_size < 96)
    {
        return {};
    }
    const size_t optional = pe + 24;
    const auto magic = word(optional);
    if (magic != 0x10b && magic != 0x20b)
    {
        return {};
    }
    Bytes header(range(optional, optional_size).begin(), range(optional, optional_size).end());
    patch(header, 64, 0); // File checksum does not change mapped engine behavior.
    const size_t security_directory = magic == 0x10b ? 128 : 144;
    if (security_directory + 8 <= header.size())
    {
        patch(header, security_directory, 0, 8); // Authenticode is file-only metadata.
    }
    Bytes material;
    constexpr char prefix[] = "csystem-native-page-image-2";
    append(material, std::span(reinterpret_cast<const uint8_t*>(prefix), sizeof(prefix)));
    put(material, word(pe + 4), 2);
    put(material, word(pe + 22), 2);
    put(material, optional_size, 2);
    append(material, header);
    size_t selected = 0;
    for (size_t index = 0; index < count; ++index)
    {
        const size_t section = optional + optional_size + index * 40;
        range(section, 40);
        const auto flags = dword(section + 36);
        ++selected;
        append(material, range(section, 8));
        append(material, range(section + 8, 12)); // Virtual size, RVA, initialized size.
        put(material, flags);
        append(material, range(dword(section + 20), dword(section + 16)));
    }
    return selected ? sha256(material) : std::string{};
}

namespace
{
std::string profile_for_image(const std::string& fingerprint)
{
    if (fingerprint == "21cdc1ee84e96e2a4129330103fd5baa65920b536f2cd9d715f621afba2a39c1")
    {
        return "native-page-3-9-options";
    }
    if (fingerprint == "1fc2f4626a3cfab17fd0b9d7dfa44f44631e6f8d578ee3cc8b11fd13f1f3424a")
    {
        return "native-page-4-9-options";
    }
    if (fingerprint == "83dac3adb56f98288a866f6010dd19cc77713197d92dfe9fd8716cb5364554e7")
    {
        return "native-page-1-4-options";
    }
    if (fingerprint == "88be3463b2e316ac7cce7318b04dcb13bc2fab9ecb58e13e05570baca0944917" ||
        fingerprint == "39df57ad10f83ca966c30c986cc96585ff190d84c0768947f177d0dfe70b6a40")
    {
        return "native-page-5-8-options";
    }
    if (fingerprint == "97b7e81596f7e1108f8083d4622d120f395fa3fcf90cb9c7d6ad75b5f8e7dc33")
    {
        return "native-page-4-10-options";
    }
    // Do not fingerprint Mujina's launcher: its embedded engine lives in an overlay.
    return {};
}
}

Json detect_page_parameter_profile(const fs::path& game)
{
    Json result = {{"status", "unrecognized"}, {"id", ""}, {"executables", Json::array()}};
    std::set<std::string> matches;
    for (const auto& entry : fs::directory_iterator(game))
    {
        auto extension = entry.path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char value) { return char(std::tolower(value)); });
        if (extension != ".exe" || !entry.is_regular_file())
        {
            continue;
        }
        Json executable = {{"path", utf8(entry.path().filename().u16string())}};
        try
        {
            const auto digest = file_hash(entry.path());
            auto id = page_parameter_profile_for_hash(digest);
            executable["sha256"] = digest;
            if (id.empty() && entry.file_size() <= 64 * 1024 * 1024)
            {
                const auto fingerprint = page_parameter_image_fingerprint(read_file(entry.path()));
                executable["image_fingerprint"] = fingerprint;
                id = profile_for_image(fingerprint);
                if (!id.empty())
                {
                    executable["recognition"] = "verified_engine_image";
                }
            }
            else if (!id.empty())
            {
                executable["recognition"] = "verified_executable";
            }
            executable["profile"] = id;
            if (!id.empty())
            {
                matches.insert(id);
            }
        }
        catch (const std::exception& error)
        {
            // Missing EXE access must not prevent decoding independent archive files.
            executable["diagnostic"] = error.what();
        }
        result["executables"].push_back(executable);
    }
    if (matches.size() == 1)
    {
        result["status"] = "recognized";
        result["id"] = *matches.begin();
    }
    else if (matches.size() > 1)
    {
        result["status"] = "ambiguous";
    }
    return result;
}

Json name_page_parameters(const Json& document, const std::string& id)
{
    if (id.empty())
    {
        return document;
    }
    const auto& profile = profile_by_id(id);
    // A recognized executable is insufficient if this particular page has another layout.
    if (document.at("format") != profile.format ||
        document.at("options").size() != profile.option_names.size())
    {
        return document;
    }
    auto result = document;
    result["parameter_profile"] = id;
    for (auto& element : result.at("elements"))
    {
        for (size_t index = 0; index < profile.element_names.size(); ++index)
        {
            rename_field(element, element_wire_names[index], profile.element_names[index]);
        }
    }
    for (size_t index = 0; index < profile.option_names.size(); ++index)
    {
        rename_field(result.at("options"), "parameter_" + std::to_string(index),
                     profile.option_names[index]);
    }
    return result;
}

Json neutral_page_parameters(const Json& document)
{
    if (!document.contains("parameter_profile"))
    {
        return document;
    }
    const auto id = document.at("parameter_profile").get<std::string>();
    const auto& profile = profile_by_id(id);
    check_shape(document, profile);
    auto result = document;
    for (auto& element : result.at("elements"))
    {
        for (size_t index = 0; index < profile.element_names.size(); ++index)
        {
            rename_field(element, profile.element_names[index], element_wire_names[index]);
        }
    }
    for (size_t index = 0; index < profile.option_names.size(); ++index)
    {
        rename_field(result.at("options"), profile.option_names[index],
                     "parameter_" + std::to_string(index));
    }
    result.erase("parameter_profile");
    return result;
}
}
