#include "graphics.hpp"
#include "page_parameters.hpp"
#include <bit>
#include <cmath>

namespace csystem
{
namespace
{
int32_t integer(const Json& value)
{
    if (!value.is_number_integer() ||
        (value.is_number_unsigned() && value.get<uint64_t>() > INT32_MAX))
    {
        throw Error("graphics field must be a signed 32-bit integer");
    }
    const auto number = value.get<int64_t>();
    if (number < INT32_MIN || number > INT32_MAX)
    {
        throw Error("graphics integer is out of range");
    }
    return int32_t(number);
}

uint32_t reference(const Json& value, const ResourceResolver& resolve = {},
                   const std::string& role = "resource")
{
    if (value.is_string() && resolve)
    {
        return uint32_t(resolve(role, -1, value.get<std::string>()));
    }
    if (!value.is_number_integer() ||
        (!value.is_number_unsigned() && value.get<int64_t>() < 0) ||
        value.get<uint64_t>() > UINT32_MAX)
    {
        throw Error("graphics reference must be a resource path or unsigned 32-bit integer");
    }
    return value.get<uint32_t>();
}

int32_t selector(const Json& value, const ResourceResolver& resolve,
                 const std::string& role, int32_t group = -1)
{
    if (value.is_string() && resolve)
    {
        return resolve(role, group, value.get<std::string>());
    }
    return integer(value);
}

void require_array(const Json& value, const std::string& name)
{
    if (!value.is_array())
    {
        throw Error(name + " must be an array");
    }
}

Json list(Cursor& reader)
{
    Json values = Json::array();
    const auto count = reader.count(4);
    for (uint32_t index = 0; index < count; ++index)
    {
        values.push_back(reader.u32());
    }
    return values;
}

void write_list(Bytes& bytes, const Json& values, const ResourceResolver& resolve = {})
{
    if (!values.is_array())
    {
        throw Error("graphics list must be an array");
    }
    put(bytes, checked32(values.size()));
    for (const auto& value : values)
    {
        put(bytes, reference(value, resolve));
    }
}

Json pairs(Cursor& reader)
{
    Json values = Json::array();
    const auto count = reader.count(8);
    for (uint32_t index = 0; index < count; ++index)
    {
        values.push_back({{"source", reader.u32()},
                          {"parameter", int32_t(reader.u32())}});
    }
    return values;
}

void write_pairs(Bytes& bytes, const Json& values, const ResourceResolver& resolve = {},
                 const std::string& role = "resource")
{
    if (!values.is_array())
    {
        throw Error("graphics source list must be an array");
    }
    put(bytes, checked32(values.size()));
    for (const auto& value : values)
    {
        put(bytes, reference(value.at("source"), resolve, role));
        put(bytes, uint32_t(integer(value.at("parameter"))));
    }
}

constexpr const char* option_names[] = {
    "reserved_0", "mask_mode", "hide_with_dialogue", "hide_on_dialogue_end", "reserved_1"};

Json group_tail(Cursor reader, unsigned tables)
{
    Json result = {{"animations", list(reader)}};
    if (tables >= 2)
    {
        result["videos"] = pairs(reader);
    }
    if (tables >= 3)
    {
        result["additional_sources"] = pairs(reader);
    }
    result["layouts"] = Json::array();
    const auto count = reader.count(16);
    for (uint32_t index = 0; index < count; ++index)
    {
        result["layouts"].push_back({{"x", int32_t(reader.u32())},
                                    {"y", int32_t(reader.u32())},
                                    {"layer", int32_t(reader.u32())},
                                    {"reserved", int32_t(reader.u32())}});
    }
    const auto remaining = reader.data.size() - reader.pos;
    if (remaining % 4 || remaining > std::size(option_names) * 4 ||
        (tables == 1 && remaining != 0) || (tables == 2 && remaining < 8) ||
        (tables == 3 && remaining < 12))
    {
        throw Error("unsupported graphics group tail");
    }
    if (remaining)
    {
        result["options"] = Json::object();
        for (size_t index = 0; !reader.done(); ++index)
        {
            result["options"][option_names[index]] = int32_t(reader.u32());
        }
    }
    reader.finish();
    return result;
}

constexpr const char* timeline_item_fields[] = {
    "group", "image", "opacity", "x", "y", "parameter", "image_mode", "layer"};
constexpr const char* classic_timeline_item_fields[] = {
    "group", "image", "opacity", "x", "y", "parameter", "layer"};
constexpr const char* timeline_final_fields[] = {
    "group", "image", "layout", "operation", "parameter"};

Json fields(Cursor& reader, std::span<const char* const> names)
{
    Json result = Json::object();
    for (const auto* name : names)
    {
        result[name] = int32_t(reader.u32());
    }
    return result;
}

void write_fields(Bytes& bytes, const Json& object, std::span<const char* const> names)
{
    for (const auto* name : names)
    {
        put(bytes, uint32_t(integer(object.at(name))));
    }
}

double floating(Cursor& reader)
{
    const auto value = std::bit_cast<double>(reader.u64());
    if (!std::isfinite(value))
    {
        throw Error("non-finite motion coefficient");
    }
    return value;
}

void write_floating(Bytes& bytes, const Json& field)
{
    if (!field.is_number() || !std::isfinite(field.get<double>()))
    {
        throw Error("motion coefficient must be finite");
    }
    put(bytes, std::bit_cast<uint64_t>(field.get<double>()), 8);
}

constexpr const char* curve_names[] = {"x", "y", "opacity"};
constexpr const char* coefficient_names[] = {"start", "constant", "linear", "quadratic", "cubic"};
constexpr const char* motion_option_names[] = {
    "duration", "x_scale", "y_scale", "parameter", "time_scale"};
}

Json decode_graphics_group(std::span<const uint8_t> payload, uint32_t archive_version, unsigned preferred_tables)
{
    Cursor reader{payload};
    Json result = {{"images", list(reader)}};
    std::vector<Json> candidates;
    for (const unsigned tables : {3U, 2U, 1U})
    {
        if ((archive_version == 22 && tables != 1) || (preferred_tables && preferred_tables != tables))
        {
            continue;
        }
        try
        {
            candidates.push_back(group_tail(reader, tables));
        }
        catch (const Error&)
        {
        }
    }
    if (candidates.size() != 1)
    {
        throw Error(candidates.empty() ? "unsupported f0 graphics group structure"
                                       : "ambiguous f0 graphics group structure");
    }
    result.update(candidates.front());
    return result;
}

unsigned graphics_group_tables(const std::vector<Entry>& entries)
{
    std::set<unsigned> common{1, 2, 3};
    bool present = false;
    for (const auto& entry : entries)
    {
        if (entry.kind != 'f' || entry.subtype != '0') continue;
        present = true;
        for (auto candidate = common.begin(); candidate != common.end(); )
        {
            try
            {
                decode_graphics_group(entry.payload, entry.version, *candidate);
                ++candidate;
            }
            catch (const Error&) { candidate = common.erase(candidate); }
        }
        if (common.empty()) return 0;
    }
    return present && common.size() == 1 ? *common.begin() : 0;
}

Bytes encode_graphics_group(const Json& document, const ResourceResolver& resolve)
{
    if (document.contains("remaining_hex"))
    {
        throw Error("remaining_hex is no longer supported; decompile the game again");
    }
    Bytes bytes;
    write_list(bytes, document.at("images"), resolve);
    write_list(bytes, document.at("animations"));
    if (document.contains("videos"))
    {
        write_pairs(bytes, document.at("videos"), resolve, "archive_video");
    }
    if (document.contains("additional_sources"))
    {
        if (!document.contains("videos"))
        {
            throw Error("additional_sources requires the video table");
        }
        write_pairs(bytes, document.at("additional_sources"));
    }
    const auto& layouts = document.at("layouts");
    if (!layouts.is_array())
    {
        throw Error("graphics layouts must be an array");
    }
    put(bytes, checked32(layouts.size()));
    for (const auto& layout : layouts)
    {
        write_fields(bytes, layout, std::array<const char*, 4>{"x", "y", "layer", "reserved"});
    }
    if (document.contains("options"))
    {
        const auto& options = document.at("options");
        if (!options.is_object() || options.size() > std::size(option_names))
        {
            throw Error("invalid graphics options");
        }
        for (size_t index = 0; index < options.size(); ++index)
        {
            put(bytes, uint32_t(integer(options.at(option_names[index]))));
        }
    }
    // The document already identifies its table layout. Re-detecting it here
    // can mistake layouts in a two-table group for a third source table.
    const unsigned tables = document.contains("additional_sources") ? 3U
                            : document.contains("videos") ? 2U : 1U;
    decode_graphics_group(bytes, tables == 1 ? 22 : 23, tables);
    return bytes;
}

Json readable_graphics_group(const Json& document, const ResourcePathLookup& paths)
{
    Json result = document;
    if (paths)
    {
        if (result.contains("videos"))
            for (auto& video : result.at("videos"))
            {
                const auto path = paths("archive_video", -1, int32_t(video.at("source").get<uint32_t>()));
                if (!path.empty()) video["source"] = path;
            }
        for (auto& image : result.at("images"))
        {
            const auto path = paths("resource", -1, int32_t(image.get<uint32_t>()));
            if (!path.empty())
            {
                image = path;
            }
        }
    }
    return result;
}

namespace
{
Json timeline_variant(std::span<const uint8_t> payload, bool classic)
{
    Cursor reader{payload};
    const std::span<const char* const> names = classic
        ? std::span<const char* const>(classic_timeline_item_fields)
        : std::span<const char* const>(timeline_item_fields);
    Json result = {{"format", classic ? "csystem-timeline-0" : "csystem-timeline-1"},
                   {"duration_ms", reader.u32()}, {"frames", Json::array()}};
    const auto frames = reader.count(8);
    for (uint32_t frame = 0; frame < frames; ++frame)
    {
        Json item = {{"timestamp_ms", reader.u32()}, {"items", Json::array()}};
        const auto count = reader.count(names.size() * 4);
        for (uint32_t index = 0; index < count; ++index)
        {
            item["items"].push_back(fields(reader, names));
        }
        result["frames"].push_back(item);
    }
    result["final_states"] = Json::array();
    const auto count = reader.count(20);
    for (uint32_t index = 0; index < count; ++index)
    {
        result["final_states"].push_back(fields(reader, timeline_final_fields));
    }
    reader.finish();
    return result;
}
}

Json decode_graphics_timeline(std::span<const uint8_t> payload, uint32_t archive_version)
{
    if (archive_version == 22)
    {
        return timeline_variant(payload, true);
    }
    try
    {
        return timeline_variant(payload, false);
    }
    catch (const Error&)
    {
        return timeline_variant(payload, true);
    }
}

Bytes encode_graphics_timeline(const Json& document, const ResourceResolver& resolve)
{
    const bool classic = document.at("format") == "csystem-timeline-0";
    if (!classic && document.at("format") != "csystem-timeline-1")
    {
        throw Error("unsupported timeline format");
    }
    Bytes bytes;
    put(bytes, reference(document.at("duration_ms")));
    const auto& frames = document.at("frames");
    require_array(frames, "timeline frames");
    put(bytes, checked32(frames.size()));
    for (const auto& frame : frames)
    {
        put(bytes, reference(frame.at("timestamp_ms")));
        const auto& items = frame.at("items");
        require_array(items, "timeline items");
        put(bytes, checked32(items.size()));
        for (const auto& item : items)
        {
            const std::span<const char* const> names = classic
                ? std::span<const char* const>(classic_timeline_item_fields)
                : std::span<const char* const>(timeline_item_fields);
            Json native = item;
            const auto group = selector(item.at("group"), resolve, "group");
            native["group"] = group;
            native["image"] = selector(item.at("image"), resolve, "image", group);
            write_fields(bytes, native, names);
        }
    }
    const auto& states = document.at("final_states");
    require_array(states, "timeline final states");
    put(bytes, checked32(states.size()));
    for (const auto& state : states)
    {
        Json native = state;
        const auto group = selector(state.at("group"), resolve, "group");
        native["group"] = group;
        native["image"] = selector(state.at("image"), resolve, "image", group);
        write_fields(bytes, native, timeline_final_fields);
    }
    timeline_variant(bytes, classic);
    return bytes;
}

Json readable_graphics_timeline(const Json& document, const ResourcePathLookup& paths)
{
    Json result = document;
    if (!paths)
    {
        return result;
    }
    const auto map_item = [&](Json& item)
    {
        const auto group = integer(item.at("group"));
        const auto group_path = paths("group", -1, group);
        if (!group_path.empty())
        {
            item["group"] = group_path;
        }
        if (item.value("image_mode", 0) == 0)
        {
            const auto image_path = paths("image", group, integer(item.at("image")));
            if (!image_path.empty())
            {
                item["image"] = image_path;
            }
        }
    };
    for (auto& frame : result.at("frames"))
    {
        for (auto& item : frame.at("items"))
        {
            map_item(item);
        }
    }
    for (auto& state : result.at("final_states"))
    {
        map_item(state);
    }
    return result;
}

Json decode_motion_preset(std::span<const uint8_t> payload)
{
    Cursor reader{payload};
    if (payload.size() == 4)
    {
        return {{"format", "csystem-motion-0"}, {"value", reader.u32()}};
    }
    Json result = {{"format", "csystem-motion-1"}, {"type", reader.u32()}};
    for (const auto* name : curve_names)
    {
        result[name] = Json::array();
        const auto count = reader.count(40);
        for (uint32_t index = 0; index < count; ++index)
        {
            Json segment = Json::object();
            for (const auto* coefficient : coefficient_names)
            {
                segment[coefficient] = floating(reader);
            }
            result[name].push_back(segment);
        }
    }
    for (const auto* name : motion_option_names)
    {
        result[name] = floating(reader);
    }
    reader.finish();
    return result;
}

Bytes encode_motion_preset(const Json& document)
{
    if (document.at("format") == "csystem-motion-0")
    {
        Bytes bytes;
        put(bytes, reference(document.at("value")));
        return bytes;
    }
    if (document.at("format") != "csystem-motion-1")
    {
        throw Error("unsupported motion format");
    }
    Bytes bytes;
    put(bytes, reference(document.at("type")));
    for (const auto* name : curve_names)
    {
        const auto& curve = document.at(name);
        require_array(curve, "motion curve");
        put(bytes, checked32(curve.size()));
        for (const auto& segment : curve)
        {
            for (const auto* coefficient : coefficient_names)
            {
                write_floating(bytes, segment.at(coefficient));
            }
        }
    }
    for (const auto* name : motion_option_names)
    {
        write_floating(bytes, document.at(name));
    }
    decode_motion_preset(bytes);
    return bytes;
}

namespace
{
constexpr const char* page_element_fields[] = {
    "result_value", "parameter_1", "parameter_2", "parameter_3", "parameter_4", "hover_sound",
    "parameter_6", "input_code"};
constexpr const char* page6_element_fields[] = {
    "result_value", "parameter_1", "parameter_2", "parameter_3", "parameter_4", "parameter_5",
    "parameter_6", "parameter_7"};
constexpr const char* page_option_names[] = {
    "parameter_0", "parameter_1", "parameter_2", "parameter_3", "parameter_4",
    "parameter_5", "parameter_6", "parameter_7", "parameter_8", "parameter_9"};
struct PageLayout
{
    const char* format;
    unsigned state_tables;
    unsigned fixed_fields;
    bool page_parameters;
    unsigned footer_lists;
    unsigned minimum_options;
    unsigned maximum_options;
};
constexpr PageLayout page_layouts[] = {
    {"csystem-native-page-4", 4, 8, true, 2, 9, 10},
    {"csystem-native-page-3", 4, 8, false, 2, 9, 9},
    {"csystem-native-page-5", 4, 7, false, 1, 8, 8},
    {"csystem-native-page-1", 3, 6, false, 1, 4, 4},
    {"csystem-native-page-6", 3, 8, false, 1, 4, 4}};
}

namespace
{
Json page_variant(std::span<const uint8_t> payload, const PageLayout& layout)
{
    Cursor reader{payload};
    Json result = {{"format", layout.format}, {"background_group", int32_t(reader.u32())},
                   {"list_0", list(reader)}, {"list_1", list(reader)}, {"list_2", list(reader)},
                   {"elements", Json::array()}};
    if (std::string_view(layout.format) == "csystem-native-page-6")
        result["layout_validation"] = "STRUCTURAL_ROUNDTRIP_ONLY; parameter semantics unverified";
    const auto count = reader.count(20 + 4 * layout.state_tables + 4 * layout.fixed_fields);
    for (uint32_t index = 0; index < count; ++index)
    {
        Json element = {{"group", int32_t(reader.u32())}};
        element["bounds"] = fields(reader, std::array<const char*, 4>{"top", "left", "bottom", "right"});
        element["state_tables"] = Json::array();
        for (unsigned state = 0; state < layout.state_tables; ++state)
        {
            Json state_items = Json::array();
            const auto item_count = reader.count(8);
            for (uint32_t item = 0; item < item_count; ++item)
            {
                state_items.push_back({{"image", int32_t(reader.u32())},
                                       {"layout", int32_t(reader.u32())}});
            }
            element["state_tables"].push_back(state_items);
        }
        element.update(fields(reader, std::string_view(layout.format) == "csystem-native-page-6"
            ? std::span<const char* const>(page6_element_fields)
            : std::span<const char* const>(page_element_fields).first(layout.fixed_fields)));
        if (layout.page_parameters)
        {
            const auto parameter = int32_t(reader.u32());
            if (parameter == -1)
            {
                element["page_parameters"] = nullptr;
            }
            else
            {
                element["page_parameters"] = Json::array({parameter, int32_t(reader.u32()),
                                                           int32_t(reader.u32()), int32_t(reader.u32())});
            }
        }
        result["elements"].push_back(element);
    }
    result["list_3"] = list(reader);
    if (layout.footer_lists == 2)
    {
        result["list_4"] = list(reader);
    }
    result["options"] = Json::object();
    for (const auto* name : page_option_names)
    {
        if (reader.done())
        {
            break;
        }
        result["options"][name] = int32_t(reader.u32());
    }
    reader.finish();
    return result;
}
}

Json decode_native_page(std::span<const uint8_t> payload, uint32_t archive_version,
                        const std::string& parameter_profile)
{
    if (!parameter_profile.empty())
    {
        const auto verified = page_parameter_layout(parameter_profile);
        for (const auto& layout : page_layouts)
        {
            if (verified.format == layout.format)
            {
                if (archive_version == 22 && layout.state_tables != 3)
                {
                    throw Error("native page profile does not match archive version");
                }
                auto result = page_variant(payload, layout);
                if (result.at("options").size() != verified.option_count)
                {
                    throw Error("native page option count does not match verified executable");
                }
                return result;
            }
        }
        throw Error("unsupported verified native page layout");
    }
    std::vector<Json> candidates;
    for (const auto& layout : page_layouts)
    {
        if (archive_version == 22 && layout.state_tables != 3)
        {
            continue;
        }
        try
        {
            auto result = page_variant(payload, layout);
            const auto options = result.at("options").size();
            if (options >= layout.minimum_options && options <= layout.maximum_options)
            {
                candidates.push_back(std::move(result));
            }
        }
        catch (const Error&)
        {
        }
    }
    if (candidates.size() == 1)
    {
        return std::move(candidates.front());
    }
    if (candidates.size() > 1)
    {
        if (candidates.size() == 2 && candidates[0].at("elements").empty() && candidates[1].at("elements").empty() &&
            candidates[0].at("format") == "csystem-native-page-1" && candidates[1].at("format") == "csystem-native-page-6")
            return std::move(candidates.front()); // Identical empty-page wire format.
        throw Error("ambiguous e0 native UI page structure; executable analysis is required");
    }
    throw Error("unsupported e0 native UI page structure");
}

Bytes encode_native_page(const Json& source, const ResourceResolver& resolve)
{
    const auto document = neutral_page_parameters(source);
    const auto found = std::find_if(std::begin(page_layouts), std::end(page_layouts),
                                    [&](const auto& candidate)
                                    {
                                        return document.at("format") == candidate.format;
                                    });
    if (found == std::end(page_layouts))
    {
        throw Error("unsupported native UI page format");
    }
    Bytes bytes;
    put(bytes, uint32_t(selector(document.at("background_group"), resolve, "group")));
    for (const auto* key : {"list_0", "list_1", "list_2"})
    {
        write_list(bytes, document.at(key));
    }
    const auto& elements = document.at("elements");
    require_array(elements, "native UI elements");
    put(bytes, checked32(elements.size()));
    for (const auto& element : elements)
    {
        const auto group = selector(element.at("group"), resolve, "group");
        put(bytes, uint32_t(group));
        write_fields(bytes, element.at("bounds"), std::array<const char*, 4>{"top", "left", "bottom", "right"});
        const auto& states = element.at("state_tables");
        if (!states.is_array() || states.size() != found->state_tables)
        {
            throw Error("native UI page state table count does not match its format");
        }
        for (const auto& state : states)
        {
            require_array(state, "native UI state items");
            put(bytes, checked32(state.size()));
            for (const auto& item : state)
            {
                put(bytes, uint32_t(selector(item.at("image"), resolve, "image", group)));
                put(bytes, uint32_t(integer(item.at("layout"))));
            }
        }
        const auto names = std::string_view(found->format) == "csystem-native-page-6"
            ? std::span<const char* const>(page6_element_fields)
            : std::span<const char* const>(page_element_fields).first(found->fixed_fields);
        for (const auto* name : names)
        {
            const auto value = std::string_view(name) == "hover_sound"
                                   ? selector(element.at(name), resolve, "sfx")
                                   : integer(element.at(name));
            put(bytes, uint32_t(value));
        }
        if (found->page_parameters)
        {
            const auto& parameters = element.at("page_parameters");
            if (parameters.is_null())
            {
                put(bytes, UINT32_MAX);
            }
            else
            {
                if (!parameters.is_array() || parameters.size() != 4 || integer(parameters[0]) == -1)
                {
                    throw Error("invalid native UI page parameters");
                }
                for (const auto& parameter : parameters)
                {
                    put(bytes, uint32_t(integer(parameter)));
                }
            }
        }
    }
    write_list(bytes, document.at("list_3"));
    if (found->footer_lists == 2)
    {
        write_list(bytes, document.at("list_4"));
    }
    const auto& options = document.at("options");
    if (!options.is_object() || options.size() < found->minimum_options ||
        options.size() > found->maximum_options)
    {
        throw Error("unsupported native UI page options");
    }
    for (size_t index = 0; index < options.size(); ++index)
    {
        put(bytes, uint32_t(integer(options.at(page_option_names[index]))));
    }
    page_variant(bytes, *found);
    return bytes;
}

Json readable_native_page(const Json& document, const ResourcePathLookup& paths)
{
    Json result = document;
    if (!paths)
    {
        return result;
    }
    const auto map_group = [&](Json& value)
    {
        const auto index = integer(value);
        const auto path = paths("group", -1, index);
        if (!path.empty())
        {
            value = path;
        }
        return index;
    };
    map_group(result.at("background_group"));
    for (auto& element : result.at("elements"))
    {
        const auto group = map_group(element.at("group"));
        for (auto& state : element.at("state_tables"))
        {
            for (auto& item : state)
            {
                const auto path = paths("image", group, integer(item.at("image")));
                if (!path.empty())
                {
                    item["image"] = path;
                }
            }
        }
        if (element.contains("hover_sound"))
        {
            const auto sound = paths("sfx", -1, integer(element.at("hover_sound")));
            if (!sound.empty()) element["hover_sound"] = sound;
        }
    }
    return result;
}
}
