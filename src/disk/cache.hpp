// Prospero Disk - The per-volume scan cache: a versioned binary file.
// Copyright (C) 2026 Leonardo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "disk/filesystem.hpp"
#include "disk/tree.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace hui::disk
{

inline constexpr std::uint32_t kCacheMagic = 0x4b534450; // "PDSK", little endian
inline constexpr std::uint32_t kCacheVersion = 1;

struct CacheInfo
{
    std::string volume_key;
    SpaceInfo space;             // statfs at scan time
    std::int64_t scanned_at = 0; // seconds since the epoch
    std::uint64_t duration_ms = 0;
    std::uint64_t unreadable = 0; // folders that could not be read
};

struct CachedScan
{
    CacheInfo info;
    std::unique_ptr<Tree> tree;
};

// The file's bytes: header, node array, string pool, then a CRC-32 of all of it.
std::string encode_cache(const CacheInfo &info, const Tree &tree);
// False for a wrong magic, another schema version, a bad CRC or a malformed
// tree: the caller treats the volume as not scanned (PRD 7.4).
bool decode_cache(std::string_view bytes, CachedScan *out);

// "<dir>/<key>.pdc"; the key keeps letters, digits, '-' and '_' only.
std::string cache_path(const std::string &dir, std::string_view volume_key);

} // namespace hui::disk
