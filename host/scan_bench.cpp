// Prospero Disk - Host benchmark: scan a folder with the app's scanner and print the result.
// Copyright (C) 2026 Leonardo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later
//
//   tools/scan-bench.sh /usr 8        # path, threads
//
// Compare its total with `du -s --block-size=1 -x <path>`: both count blocks
// on one device and each hard-linked file once.

#include "disk/cache.hpp"
#include "disk/format.hpp"
#include "disk/scanner.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char **argv)
{
    using namespace hui::disk;
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s <path> [threads]\n", argv[0]);
        return 2;
    }
    const auto fs = make_posix_filesystem();
    ScanOptions options;
    options.root = argv[1];
    options.threads = argc > 2 ? std::atoi(argv[2]) : 8;

    Scanner scanner;
    if (!scanner.start(*fs, options))
    {
        std::fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }
    scanner.join();
    const ScanProgress p = scanner.progress();
    const auto tree = scanner.take();
    if (!tree)
        return 1;
    const std::string cache = encode_cache(CacheInfo{}, *tree);
    std::printf("path       %s\n", argv[1]);
    std::printf("threads    %d\n", options.threads);
    std::printf("time       %s\n", format_duration(p.seconds).c_str());
    std::printf("files      %s\n", format_count(p.files).c_str());
    std::printf("folders    %s\n", format_count(p.folders).c_str());
    std::printf("unreadable %s\n", format_count(p.unreadable).c_str());
    std::printf("on disk    %llu bytes (%s)\n", static_cast<unsigned long long>(tree->node(0).disk),
                format_bytes(tree->node(0).disk).c_str());
    std::printf("apparent   %s\n", format_bytes(tree->node(0).apparent).c_str());
    std::printf("nodes      %s (%s in memory, %s cached)\n", format_count(tree->size()).c_str(),
                format_bytes(tree->size() * sizeof(Node) + tree->pool().size()).c_str(),
                format_bytes(cache.size()).c_str());
    return 0;
}
