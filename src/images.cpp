#include "tool.hpp"
#include "jpeg.hpp"
#include "image_profiles.hpp"
#include <algorithm>
#include <objidl.h>
#include <wincodec.h>
#include <windows.h>

namespace csystem
{
namespace
{
void hr(HRESULT h)
{
    if (FAILED(h))
    {
        throw Error("Windows image codec failed: " + std::to_string(uint32_t(h)));
    }
}

template <class T> struct Com
{
    T* p = nullptr;

    ~Com()
    {
        if (p)
        {
            p->Release();
        }
    }

    T** address()
    {
        return &p;
    }

    T* operator->()
    {
        return p;
    }
};

void init_com()
{
    static thread_local bool done = false;
    if (!done)
    {
        auto h = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(h) && h != RPC_E_CHANGED_MODE)
        {
            hr(h);
        }
        done = true;
    }
}

uint32_t be32(std::span<const uint8_t> b)
{
    if (b.size() < 4)
    {
        throw Error("truncated image size");
    }
    return uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3];
}

void validate_dimensions(uint32_t w, uint32_t h)
{
    if (!w || !h || uint64_t(w) * h > 64 * 1024 * 1024)
    {
        throw Error("invalid or oversized image dimensions");
    }
}
}

Image decode_standard(std::span<const uint8_t> b)
{
    init_com();
    Com<IWICImagingFactory> factory;
    hr(CoCreateInstance(CLSID_WICImagingFactory,
                        nullptr,
                        CLSCTX_INPROC_SERVER,
                        IID_IWICImagingFactory,
                        reinterpret_cast<void**>(factory.address())));
    Com<IWICStream> stream;
    hr(factory->CreateStream(stream.address()));
    hr(stream->InitializeFromMemory(const_cast<BYTE*>(b.data()), checked32(b.size())));
    Com<IWICBitmapDecoder> dec;
    hr(factory->CreateDecoderFromStream(
        stream.p, nullptr, WICDecodeMetadataCacheOnLoad, dec.address()));
    Com<IWICBitmapFrameDecode> frame;
    hr(dec->GetFrame(0, frame.address()));
    Image out;
    hr(frame->GetSize(&out.width, &out.height));
    validate_dimensions(out.width, out.height);
    Com<IWICFormatConverter> convert;
    hr(factory->CreateFormatConverter(convert.address()));
    hr(convert->Initialize(frame.p,
                           GUID_WICPixelFormat32bppBGRA,
                           WICBitmapDitherTypeNone,
                           nullptr,
                           0,
                           WICBitmapPaletteTypeCustom));
    out.bgra.resize(size_t(out.width) * out.height * 4);
    hr(convert->CopyPixels(nullptr, out.width * 4, checked32(out.bgra.size()), out.bgra.data()));
    return out;
}

Bytes encode_png(const Image& image)
{
    init_com();
    validate_dimensions(image.width, image.height);
    if (image.bgra.size() != uint64_t(image.width) * image.height * 4)
    {
        throw Error("pixel buffer size mismatch");
    }
    Com<IWICImagingFactory> factory;
    hr(CoCreateInstance(CLSID_WICImagingFactory,
                        nullptr,
                        CLSCTX_INPROC_SERVER,
                        IID_IWICImagingFactory,
                        reinterpret_cast<void**>(factory.address())));
    Com<IStream> stream;
    hr(CreateStreamOnHGlobal(nullptr, TRUE, stream.address()));
    Com<IWICBitmapEncoder> encoder;
    hr(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.address()));
    hr(encoder->Initialize(stream.p, WICBitmapEncoderNoCache));
    Com<IWICBitmapFrameEncode> frame;
    Com<IPropertyBag2> properties;
    hr(encoder->CreateNewFrame(frame.address(), properties.address()));
    hr(frame->Initialize(properties.p));
    hr(frame->SetSize(image.width, image.height));
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    hr(frame->SetPixelFormat(&format));
    if (format != GUID_WICPixelFormat32bppBGRA)
    {
        throw Error("PNG encoder changed pixel format");
    }
    hr(frame->WritePixels(image.height,
                          image.width * 4,
                          checked32(image.bgra.size()),
                          const_cast<BYTE*>(image.bgra.data())));
    hr(frame->Commit());
    hr(encoder->Commit());
    STATSTG stat{};
    hr(stream->Stat(&stat, STATFLAG_NONAME));
    LARGE_INTEGER zero{};
    hr(stream->Seek(zero, STREAM_SEEK_SET, nullptr));
    Bytes result(size_t(stat.cbSize.QuadPart));
    ULONG read = 0;
    hr(stream->Read(result.data(), checked32(result.size()), &read));
    if (read != result.size())
    {
        throw Error("PNG stream truncated");
    }
    return result;
}

Bytes wrap_png(std::span<const uint8_t> b, uint8_t flag)
{
    static constexpr uint8_t sig[] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (b.size() < 8 || !std::equal(std::begin(sig), std::end(sig), b.begin()))
    {
        throw Error("replacement must be PNG");
    }
    Bytes out{'c'};
    auto size = checked32(b.size());
    for (int shift : {24, 16, 8, 0})
    {
        out.push_back(uint8_t(size >> shift));
    }
    out.push_back(flag);
    append(out, b);
    return out;
}

std::optional<std::span<const uint8_t>> unwrap_jpeg(std::span<const uint8_t> bytes)
{
    if (bytes.size() < 8 || bytes[0] != 'b' ||
        be32(bytes.subspan(1, 4)) != bytes.size() - 5 ||
        bytes[5] != 0xff || bytes[6] != 0xd8 || bytes[7] != 0xff)
        return std::nullopt;
    return bytes.subspan(5);
}

Bytes wrap_jpeg(std::span<const uint8_t> bytes)
{
    if (bytes.size() < 3 || bytes[0] != 0xff || bytes[1] != 0xd8 || bytes[2] != 0xff)
        throw Error("replacement must be JPEG");
    Bytes output{'b'};
    const auto size = checked32(bytes.size());
    for (int shift : {24, 16, 8, 0}) output.push_back(uint8_t(size >> shift));
    append(output, bytes);
    return output;
}

Image decode_image(std::span<const uint8_t> b, const std::function<Image(uint32_t)>& base)
{
    if (is_tb1(b)) return decode_tb1(b);
    if (b.empty())
    {
        throw Error("empty image");
    }
    if (b[0] == 'b' || b[0] == 'c')
    {
        size_t start = b[0] == 'b' ? 5 : 6;
        if (b.size() < start)
        {
            throw Error("truncated image wrapper");
        }
        auto n = be32(b.subspan(1, 4));
        if (n != b.size() - start)
        {
            throw Error("image wrapper size mismatch");
        }
        return decode_standard(b.subspan(start, n));
    }
    if (b[0] != 'a' && b[0] != 'd')
    {
        throw Error("unrecognized image marker");
    }
    Cursor r{b};
    r.u8();
    auto base_id = r.u32();
    r.u32();
    auto width = r.u32(), height = r.u32();
    auto flags = r.u32();
    auto alpha_size = r.u32();
    std::span<const uint8_t> alpha;
    if (flags & 4)
    {
        alpha = r.take(alpha_size);
    }
    auto mask_size = r.u32();
    auto mask = r.take(mask_size);
    auto color_size = r.u32();
    auto color = r.take(color_size);
    r.finish();
    Image out;
    if (!mask.empty())
    {
        out = base(base_id);
        if (b[0] == 'a' && width && width != out.width)
        {
            throw Error("delta width differs from base");
        }
        if (b[0] == 'a' && height && height != out.height)
        {
            throw Error("delta height differs from base");
        }
        size_t pixels = size_t(out.width) * out.height;
        if (mask.size() < (pixels + 7) / 8)
        {
            throw Error("delta mask truncated");
        }
        size_t changed = 0;
        for (size_t i = 0; i < pixels; i++)
        {
            if (mask[i / 8] & (1 << (i % 8)))
            {
                changed++;
            }
        }
        if (color.size() != changed * 3)
        {
            throw Error("delta color length mismatch");
        }
        if (!alpha.empty() && alpha.size() != changed && alpha.size() != changed * 3)
        {
            throw Error("delta alpha length mismatch");
        }
        size_t at = 0;
        for (size_t i = 0; i < pixels; i++)
        {
            if (mask[i / 8] & (1 << (i % 8)))
            {
                out.bgra[i * 4] = color[at * 3];
                out.bgra[i * 4 + 1] = color[at * 3 + 1];
                out.bgra[i * 4 + 2] = color[at * 3 + 2];
                if (!alpha.empty())
                {
                    out.bgra[i * 4 + 3] = alpha[alpha.size() == changed ? at : at * 3];
                }
                at++;
            }
        }
        return out;
    }
    validate_dimensions(width, height);
    out.width = width;
    out.height = height;
    size_t pixels = size_t(width) * height, stride = size_t(width) * 3 + (width & 3);
    out.bgra.resize(pixels * 4, 255);
    if (color.size() == pixels * 4 && alpha.empty())
    {
        out.bgra.assign(color.begin(), color.end());
        return out;
    }
    bool gray = color.size() == pixels;
    if (!gray && color.size() != stride * height && color.size() != pixels * 3)
    {
        throw Error("unsupported full-image color layout");
    }
    if (color.size() == pixels * 3)
    {
        stride = size_t(width) * 3;
    }
    bool alpha_pixel = alpha.size() == pixels;
    bool alpha_rgb = alpha.size() == stride * height;
    if (!alpha.empty() && !alpha_pixel && !alpha_rgb)
    {
        throw Error("unsupported full-image alpha layout");
    }
    for (uint32_t y = 0; y < height; y++)
    {
        for (uint32_t x = 0; x < width; x++)
        {
            size_t pixel = size_t(y) * width + x, at = size_t(y) * stride + x * 3;
            for (size_t c = 0; c < 3; c++)
            {
                out.bgra[pixel * 4 + c] = color[gray ? pixel : at + c];
            }
            if (!alpha.empty())
            {
                out.bgra[pixel * 4 + 3] = alpha[alpha_pixel ? pixel : at];
            }
        }
    }
    return out;
}
Image decode_image_alpha(std::span<const uint8_t> bytes, const std::function<Image(uint32_t)>& base)
{
    if (bytes.empty()) throw Error("empty alpha image");
    if (bytes[0] != 'a' && bytes[0] != 'd')
    {
        auto image = decode_image(bytes, base);
        for (size_t pixel = 0; pixel < image.bgra.size(); pixel += 4)
        {
            std::fill_n(image.bgra.begin() + pixel, 3, image.bgra[pixel + 3]);
            image.bgra[pixel + 3] = 255;
        }
        return image;
    }
    Cursor reader{bytes};
    const auto kind = reader.u8();
    const auto reference = reader.u32();
    reader.u32();
    const auto width = reader.u32(), height = reader.u32();
    reader.u32();
    const auto alpha = reader.take(reader.u32());
    const auto mask = reader.take(reader.u32());
    const auto color = reader.take(reader.u32());
    reader.finish();
    if (!mask.empty())
    {
        auto image = base(reference);
        const auto pixels = size_t(image.width) * image.height;
        if (mask.size() < (pixels + 7) / 8 ||
            (kind == 'a' && ((width && width != image.width) || (height && height != image.height))))
            throw Error("invalid delta alpha dimensions or mask");
        size_t changed = 0;
        size_t count = 0;
        for (size_t i = 0; i < pixels; ++i)
            count += (mask[i / 8] & (1 << (i % 8))) != 0;
        for (size_t pixel = 0; pixel < pixels; ++pixel)
        {
            if (mask[pixel / 8] & (1 << (pixel % 8)))
            {
                if (!alpha.empty())
                {
                    const size_t step = alpha.size() == count ? 1 : 3;
                    if (changed * step + step > alpha.size()) throw Error("delta alpha truncated");
                    for (size_t c = 0; c < 3; ++c)
                        image.bgra[pixel * 4 + c] = alpha[changed * step + (step == 1 ? 0 : c)];
                }
                ++changed;
            }
        }
        if ((!alpha.empty() && alpha.size() != changed && alpha.size() != changed * 3) || color.size() != changed * 3)
            throw Error("delta alpha or color length mismatch");
        return image;
    }
    validate_dimensions(width, height);
    Image image{width, height, Bytes(size_t(width) * height * 4, 255)};
    const size_t pixels = size_t(width) * height;
    const size_t color_stride = size_t(width) * 3 + (width & 3);
    const size_t stride = color.size() == pixels * 3 ? size_t(width) * 3 : color_stride;
    if (alpha.empty())
    {
        if (color.size() == pixels * 4)
            for (size_t i = 0; i < pixels; ++i)
                std::fill_n(image.bgra.begin() + i * 4, 3, color[i * 4 + 3]);
        return image;
    }
    if (alpha.size() != pixels && alpha.size() != stride * height)
        throw Error("unsupported full-image alpha layout");
    for (uint32_t y = 0; y < height; ++y)
    {
        for (uint32_t x = 0; x < width; ++x)
        {
            const auto pixel = size_t(y) * width + x;
            const auto at = size_t(y) * stride + x * 3;
            for (size_t channel = 0; channel < 3; ++channel)
                image.bgra[pixel * 4 + channel] = alpha[alpha.size() == pixels ? pixel : at + channel];
        }
    }
    return image;
}
}
