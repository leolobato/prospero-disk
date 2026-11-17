// Prospero Disk - The per-volume scan cache: a versioned binary file.
// Copyright (C) 2026 Leonardo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later

#include "disk/cache.hpp"

#include "core/save_file.hpp"

#include <cctype>
#include <cstring>

namespace hui::disk
{

namespace
{

// Fixed-size header; all fields little endian (the console and the PC are both x86-64).
struct Header
{
    std::uint32_t magic;
    std::uint32_t version;
    std::uint64_t node_count;
    std::uint64_t pool_size;
    std::uint64_t total;
    std::uint64_t used;
    std::uint64_t free;
    std::int64_t scanned_at;
    std::uint64_t duration_ms;
    std::uint64_t unreadable;
    std::uint32_t key_size;
    std::uint32_t node_size;
};
static_assert(sizeof(Header) == 80, "the cache header has no padding");

constexpr std::uint64_t kMaxNodes = 64ull << 20; // far above any real volume

template <typename T> void append(std::string &out, const T &value)
{
    out.append(reinterpret_cast<const char *>(&value), sizeof value);
}

} // namespace

std::string encode_cache(const CacheInfo &info, const Tree &tree)
{
    Header h{};
    h.magic = kCacheMagic;
    h.version = kCacheVersion;
    h.node_count = tree.nodes().size();
    h.pool_size = tree.pool().size();
    h.total = info.space.total;
    h.used = info.space.used;
    h.free = info.space.free;
    h.scanned_at = info.scanned_at;
    h.duration_ms = info.duration_ms;
    h.unreadable = info.unreadable;
    h.key_size = static_cast<std::uint32_t>(info.volume_key.size());
    h.node_size = sizeof(Node);

    std::string out;
    out.reserve(sizeof h + info.volume_key.size() + tree.nodes().size() * sizeof(Node) +
                tree.pool().size() + 4);
    append(out, h);
    out += info.volume_key;
    out.append(reinterpret_cast<const char *>(tree.nodes().data()),
               tree.nodes().size() * sizeof(Node));
    out += tree.pool();
    append(out, save::crc32(out));
    return out;
}

bool decode_cache(std::string_view bytes, CachedScan *out)
{
    Header h{};
    if (bytes.size() < sizeof h + 4)
        return false;
    std::memcpy(&h, bytes.data(), sizeof h);
    if (h.magic != kCacheMagic || h.version != kCacheVersion || h.node_size != sizeof(Node))
        return false;
    if (h.node_count == 0 || h.node_count > kMaxNodes || h.pool_size > (1ull << 32) ||
        h.key_size > 256)
        return false;
    const std::uint64_t expected =
        sizeof h + h.key_size + h.node_count * sizeof(Node) + h.pool_size + 4;
    if (bytes.size() != expected)
        return false;
    std::uint32_t crc = 0;
    std::memcpy(&crc, bytes.data() + bytes.size() - 4, 4);
    if (crc != save::crc32(bytes.substr(0, bytes.size() - 4)))
        return false;

    std::size_t at = sizeof h;
    CachedScan scan;
    scan.info.volume_key.assign(bytes.substr(at, h.key_size));
    at += h.key_size;
    std::vector<Node> nodes(h.node_count);
    std::memcpy(nodes.data(), bytes.data() + at, h.node_count * sizeof(Node));
    at += h.node_count * sizeof(Node);
    std::string pool(bytes.substr(at, h.pool_size));

    auto tree = std::make_unique<Tree>();
    if (!tree->adopt(std::move(nodes), std::move(pool)))
        return false;
    scan.info.space = {h.total, h.used, h.free};
    scan.info.scanned_at = h.scanned_at;
    scan.info.duration_ms = h.duration_ms;
    scan.info.unreadable = h.unreadable;
    scan.tree = std::move(tree);
    *out = std::move(scan);
    return true;
}

std::string cache_path(const std::string &dir, std::string_view volume_key)
{
    std::string name;
    for (const char c : volume_key)
        if (std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_')
            name.push_back(c);
    if (name.empty())
        name = "volume";
    return dir + "/" + name + ".pdc";
}

} // namespace hui::disk
