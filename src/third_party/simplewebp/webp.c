// Prospero Disk - The single compilation unit for the vendored SimpleWebP.
// Copyright (C) 2026 Leonardo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The console keeps the home-screen copy of a retail game's icon as WebP
// (named icon0.png all the same, under /user/catalog_downloader), and the
// app reads it into memory, so the stdio loaders are left out.

#define SIMPLEWEBP_IMPLEMENTATION 1
#define SIMPLEWEBP_DISABLE_STDIO 1
#include "simplewebp.h"
