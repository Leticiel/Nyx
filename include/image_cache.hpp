#pragma once
#include "tool.hpp"
#include <deque>

namespace csystem
{
class ImageCache
{
    std::function<Bytes(uint32_t)> read_payload;
    std::map<uint32_t, Image> cache;
    std::deque<uint32_t> order;
    std::set<uint32_t> active;
    size_t used_bytes = 0;
    bool alpha_only;

  public:
    explicit ImageCache(std::function<Bytes(uint32_t)> reader, bool alpha = false)
        : read_payload(std::move(reader)), alpha_only(alpha) {}

    Image get(uint32_t id)
    {
        if (cache.contains(id)) return cache.at(id);
        if (!active.insert(id).second || active.size() > 128)
            throw Error("cyclic or excessive image dependency at " + std::to_string(id));
        try
        {
            const auto base = [this](uint32_t reference) { return get(reference); };
            const auto payload = read_payload(id);
            auto image = alpha_only ? decode_image_alpha(payload, base) : decode_image(payload, base);
            active.erase(id);
            while (!order.empty() && used_bytes + image.bgra.size() > 128 * 1024 * 1024)
            {
                used_bytes -= cache.at(order.front()).bgra.size();
                cache.erase(order.front());
                order.pop_front();
            }
            used_bytes += image.bgra.size();
            cache[id] = image;
            order.push_back(id);
            return image;
        }
        catch (...)
        {
            active.erase(id);
            throw;
        }
    }
};
}
