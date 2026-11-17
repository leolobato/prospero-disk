// Prospero Disk - The single compilation unit for the vendored stb_image.
// Copyright (C) 2026 Leonardo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Title icons are PNG files read into memory, so only the PNG decoder and the
// memory reader are built.

#define STB_IMAGE_IMPLEMENTATION 1
#define STBI_ONLY_PNG 1
#define STBI_NO_STDIO 1
#define STBI_NO_LINEAR 1
#define STBI_NO_HDR 1
#include "stb_image.h"
