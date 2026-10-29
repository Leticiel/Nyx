#include "references.hpp"
#include "graphics.hpp"
#include <algorithm>

namespace csystem
{
namespace
{
using Key = std::pair<std::string, int32_t>;
using Slots = std::map<int32_t, std::string>;
using Paths = std::map<Key, Slots>;

std::string normalize(std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}

}

AssetReferences asset_references(const fs::path& project, const Json& manifest)
{
    if (manifest.value("format", "") != project_format)
    {
        throw Error("unsupported project format; decompile the game again with this toolkit");
    }
    auto paths = std::make_shared<Paths>();
    std::map<std::string, std::map<uint32_t, std::string>> files;
    for (const auto& file : manifest.at("files"))
    {
        files[file.at("archive")][file.at("entry")] = normalize(file.at("path"));
    }
    std::map<uint32_t, Json> sources;
    for (const auto& document : manifest.at("sources"))
    {
        sources[document.at("entry")] = document;
    }
    for (const auto& [id, path] : files["resources"])
    {
        (*paths)[{"resource", -1}][int32_t(id)] = path;
    }
    for (const auto& archive : manifest.at("archives"))
    {
        if (archive.at("name") != "videos") continue;
        int32_t ordinal = 0;
        for (const auto& entry : archive.at("entries"))
            if (entry.at("kind") == "y" && entry.at("subtype") == "0")
                (*paths)[{"archive_video", -1}][ordinal++] =
                    files.at("videos").at(entry.at("id").get<uint32_t>());
    }
    if (!manifest.at("has_resources").get<bool>())
    {
        int32_t ordinal = 0;
        for (const auto& archive : manifest.at("archives"))
        {
            if (archive.at("name") == "images")
            {
                for (const auto& entry : archive.at("entries"))
                {
                    if (entry.at("kind") == "b" && (entry.at("subtype") == "0" ||
                                                     entry.at("subtype") == std::string(1, '\0')))
                    {
                        const auto id = entry.at("id").get<uint32_t>();
                        if (files["images"].contains(id))
                            (*paths)[{"resource", -1}][ordinal] = files.at("images").at(id);
                        ++ordinal;
                    }
                }
            }
        }
    }
    const ResourceResolver resource_resolver = [paths](const std::string& role,
                                                       int32_t,
                                                       const std::string& name)
    {
        const Key key{role, -1};
        if ((role != "resource" && role != "archive_video") || !paths->contains(key))
            throw Error("resource is not registered: " + name);
        const auto& resources = paths->at(key);
        const auto path = normalize(name);
        auto found = resources.end();
        for (auto slot = resources.begin(); slot != resources.end(); ++slot)
        {
            if (slot->second == path)
            {
                if (found != resources.end())
                {
                    throw Error("resource has multiple indices; use its numeric index: " + name);
                }
                found = slot;
            }
        }
        if (found == resources.end())
        {
            throw Error("resource is not registered: " + name);
        }
        return found->first;
    };
    for (const auto& archive : manifest.at("archives"))
    {
        int32_t group = 0;
        int32_t script = 0;
        int32_t movie = 0;
        int32_t page = 0;
        int32_t video = 0;
        int32_t transition = 101;
        int32_t indicator_animation = 0;
        for (const auto& entry : archive.at("entries"))
        {
            const auto id = entry.at("id").get<uint32_t>();
            const auto kind = entry.at("kind").get<std::string>();
            if (archive.at("name") == "images" && kind == "c" && entry.at("subtype") == "0")
            {
                (*paths)[{"indicator_animation", -1}][indicator_animation++] =
                    files.at("images").at(id);
            }
            if (archive.at("name") == "images" && kind == "n" && entry.at("subtype") == "0")
            {
                auto& masks = (*paths)[{"transition", -1}];
                if (transition < 200)
                {
                    masks[transition++] = files.at("images").at(id);
                }
            }
            if (archive.at("name") == "audio" && entry.at("subtype") == "0")
            {
                if (kind == "i")
                {
                    (*paths)[{"video", -1}][video++] = files.at("audio").at(id);
                }
                const std::string role = kind == "j" ? "voice"
                                         : kind == "u" || (kind == "h" && entry.at("version") == 22)
                                             ? "music"
                                         : kind == "k" ? "sfx"
                                                       : "";
                if (!role.empty())
                {
                    auto& audio = (*paths)[{role, -1}];
                    audio[int32_t(audio.size())] = files.at("audio").at(id);
                }
            }
            if (archive.at("name") == "scripts" && entry.at("subtype") == "0")
            {
                if (kind == "a")
                {
                    (*paths)[{"scene", -1}][int32_t(id)] = normalize(sources.at(id).at("path"));
                }
                else if (kind == "s")
                {
                    (*paths)[{"script", -1}][script++] = normalize(sources.at(id).at("path"));
                }
                else if (kind == "m")
                {
                    (*paths)[{"movie", -1}][movie++] = files.at("scripts").at(id);
                }
                else if (kind == "e")
                {
                    (*paths)[{"page", -1}][page++] = files.at("scripts").at(id);
                }
            }
            if (archive.at("name") == "scripts" && kind == "f" && entry.at("subtype") == "0")
            {
                (*paths)[{"group", -1}][group] = normalize(sources.at(id).at("path"));
                const fs::path relative(utf16(sources.at(id).at("path")));
                if (relative.is_absolute() || relative.has_root_name() ||
                    std::find(relative.begin(), relative.end(), fs::path("..")) != relative.end())
                {
                    throw Error("graphics source path escapes project");
                }
                if (sources.at(id).value("type", "") != "graphics")
                {
                    throw Error("graphics group requires a JSON source document");
                }
                Bytes payload;
                try
                {
                    payload = encode_graphics_group(Json::parse(read_text(project / relative)),
                                                    resource_resolver);
                }
                catch (const Error& error)
                {
                    throw Error(utf8(relative.u16string()) + ": " + error.what());
                }
                catch (const Json::exception& error)
                {
                    throw Error(utf8(relative.u16string()) + ": " + error.what());
                }
                Cursor reader{payload};
                const auto count = reader.count(4);
                auto& images = (*paths)[{"image", group++}];
                for (uint32_t image = 0; image < count; ++image)
                {
                    const auto resource = reader.u32();
                    const auto& resources = (*paths)[{"resource", -1}];
                    images[int32_t(image) + 1] = resources.contains(int32_t(resource))
                                                     ? resources.at(int32_t(resource))
                                                     : "";
                }
            }
        }
    }
    ResourceResolver resolve =
        [paths](const std::string& role, int32_t group, const std::string& path)
    {
        const Key key{role, group};
        const auto normalized = normalize(path);
        if (!paths->contains(key) || normalized.empty())
        {
            throw Error("no resource group for " + role + ": " + path);
        }
        const auto& candidates = paths->at(key);
        auto matches = [&](const auto& slot)
        {
            return slot.second == normalized;
        };
        const auto found = std::find_if(candidates.begin(), candidates.end(), matches);
        if (found == candidates.end())
        {
            throw Error("resource is not registered in this " + role + " group: " + path);
        }
        if (std::count_if(candidates.begin(), candidates.end(), matches) != 1)
        {
            throw Error("resource has multiple slots in this group; use its numeric index: " +
                        path);
        }
        return found->first;
    };
    ResourcePathLookup lookup = [paths](const std::string& role, int32_t group, int32_t index)
    {
        const Key key{role, group};
        if (!paths->contains(key) || !paths->at(key).contains(index))
        {
            return std::string();
        }
        const auto& candidates = paths->at(key);
        const auto& path = candidates.at(index);
        return !path.empty() && std::count_if(candidates.begin(),
                                              candidates.end(),
                                              [&](const auto& slot)
                                              {
                                                  return slot.second == path;
                                              }) == 1
                   ? path
                   : std::string();
    };
    return {resolve, lookup};
}
}
