// Prospero Disk - Title icons: PNG decoding (stb_image) and a tiny stored-PNG writer.
// Copyright (C) 2026 Leonardo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later

#include "disk/png.hpp"

#include "core/save_file.hpp"

#include <algorithm>
#include <climits>

extern "C"
{
    unsigned char *stbi_load_from_memory(const unsigned char *buffer, int len, int *x, int *y,
                                         int *channels_in_file, int desired_channels);
    void stbi_image_free(void *retval_from_stbi_load);
}

namespace hui::disk
{

bool decode_png(std::string_view bytes, int max_side, Image *out)
{
    if (bytes.empty() || bytes.size() > static_cast<std::size_t>(INT_MAX))
        return false;
    int w = 0;
    int h = 0;
    int channels = 0;
    unsigned char *pixels =
        stbi_load_from_memory(reinterpret_cast<const unsigned char *>(bytes.data()),
                              static_cast<int>(bytes.size()), &w, &h, &channels, 4);
    if (pixels == nullptr)
        return false;
    // Whole-number shrink factor: a 512 icon becomes 128 with a 4 x 4 box.
    const int factor = std::max(1, (std::max(w, h) + max_side - 1) / max_side);
    out->width = std::max(1, w / factor);
    out->height = std::max(1, h / factor);
    out->rgba.assign(static_cast<std::size_t>(out->width * out->height * 4), 0);
    for (int y = 0; y < out->height; ++y)
        for (int x = 0; x < out->width; ++x)
        {
            unsigned sum[4] = {0, 0, 0, 0};
            for (int sy = 0; sy < factor; ++sy)
                for (int sx = 0; sx < factor; ++sx)
                {
                    const unsigned char *p =
                        pixels +
                        (static_cast<std::size_t>((y * factor + sy) * w + x * factor + sx) * 4);
                    for (int c = 0; c < 4; ++c)
                        sum[c] += p[c];
                }
            std::uint8_t *o = &out->rgba[static_cast<std::size_t>((y * out->width + x) * 4)];
            for (int c = 0; c < 4; ++c)
                o[c] = static_cast<std::uint8_t>(sum[c] / static_cast<unsigned>(factor * factor));
        }
    stbi_image_free(pixels);
    return true;
}

namespace
{

void put32(std::string &out, std::uint32_t v)
{
    out.push_back(static_cast<char>(v >> 24));
    out.push_back(static_cast<char>((v >> 16) & 0xff));
    out.push_back(static_cast<char>((v >> 8) & 0xff));
    out.push_back(static_cast<char>(v & 0xff));
}

void chunk(std::string &out, const char *type, const std::string &data)
{
    put32(out, static_cast<std::uint32_t>(data.size()));
    std::string body(type, 4);
    body += data;
    out += body;
    put32(out, save::crc32(body));
}

} // namespace

std::string encode_png(int width, int height, const std::uint8_t *rgba)
{
    std::string raw;
    const std::size_t row = static_cast<std::size_t>(width) * 4;
    for (int y = 0; y < height; ++y)
    {
        raw.push_back(0); // filter: none
        raw.append(reinterpret_cast<const char *>(rgba) + row * static_cast<std::size_t>(y), row);
    }

    std::string z;
    z.push_back(0x78);
    z.push_back(0x01);
    std::size_t at = 0;
    do
    {
        const std::size_t n = std::min<std::size_t>(65535, raw.size() - at);
        const bool last = at + n == raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(static_cast<char>(n & 0xff));
        z.push_back(static_cast<char>(n >> 8));
        z.push_back(static_cast<char>(~n & 0xff));
        z.push_back(static_cast<char>((~n >> 8) & 0xff));
        z.append(raw, at, n);
        at += n;
    } while (at < raw.size());
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (const char c : raw)
    {
        a = (a + static_cast<std::uint8_t>(c)) % 65521u;
        b = (b + a) % 65521u;
    }
    put32(z, (b << 16) | a);

    std::string out("\x89PNG\r\n\x1a\n", 8);
    std::string header;
    put32(header, static_cast<std::uint32_t>(width));
    put32(header, static_cast<std::uint32_t>(height));
    header.push_back(8); // bit depth
    header.push_back(6); // RGBA
    header.push_back(0);
    header.push_back(0);
    header.push_back(0);
    chunk(out, "IHDR", header);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", std::string());
    return out;
}

} // namespace hui::disk
