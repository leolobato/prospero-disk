// Prospero Disk - Title icons: PNG and WebP decoding, and a tiny stored-PNG writer.
// Copyright (C) 2026 Leonardo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hui::disk
{

struct Image
{
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
};

// Decodes a PNG or a WebP (told apart by content, not by name: the console
// keeps WebP icons named icon0.png) and shrinks it (box filter) to at most
// max_side pixels a side.
bool decode_image(std::string_view bytes, int max_side, Image *out);

// An uncompressed PNG (deflate "stored" blocks): only for the fixture's icons.
std::string encode_png(int width, int height, const std::uint8_t *rgba);

} // namespace hui::disk
