// Prospero Disk - Tests for the scanner, the layout, the cache and title lookup.
// Copyright (C) 2026 Leonardo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later

#include "disk/cache.hpp"
#include "disk/format.hpp"
#include "disk/png.hpp"
#include "disk/scanner.hpp"
#include "disk/sunburst.hpp"
#include "disk/titles.hpp"
#include "disk/volumes.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace
{

using namespace hui::disk;

std::uint32_t find_child(const Tree &tree, std::uint32_t parent, std::string_view name)
{
    for (const std::uint32_t c : tree.children(parent))
        if (tree.name(c) == name)
            return c;
    return kNoNode;
}

std::uint32_t find_path(const Tree &tree, std::initializer_list<std::string_view> names)
{
    std::uint32_t at = tree.root();
    for (const std::string_view n : names)
    {
        at = find_child(tree, at, n);
        if (at == kNoNode)
            return kNoNode;
    }
    return at;
}

std::unique_ptr<Tree> scan(const FileSystem &fs, const std::string &root, int threads = 4)
{
    Scanner scanner;
    ScanOptions options;
    options.root = root;
    options.threads = threads;
    EXPECT_TRUE(scanner.start(fs, options));
    scanner.join();
    EXPECT_TRUE(scanner.progress().finished);
    return scanner.take();
}

// Every node's size is the sum of its children plus its own blocks.
void expect_consistent(const Tree &tree, std::uint32_t index)
{
    const Node &n = tree.node(index);
    std::uint64_t files = 0;
    std::uint32_t folders = 0;
    std::uint64_t disk = 0;
    for (const std::uint32_t c : tree.children(index))
    {
        disk += tree.node(c).disk;
        files += tree.node(c).files;
        if (tree.node(c).kind == Kind::directory)
        {
            folders += 1 + tree.node(c).folders;
            expect_consistent(tree, c);
        }
    }
    if (n.kind == Kind::directory)
    {
        EXPECT_GE(n.disk, disk) << tree.path(index);
        EXPECT_EQ(n.files, files) << tree.path(index);
        EXPECT_EQ(n.folders, folders) << tree.path(index);
    }
}

TEST(DiskTree, PathsAndTotalsRollUp)
{
    Tree tree("/user");
    const std::uint32_t app = tree.add_child(tree.root(), "app", Kind::directory);
    const std::uint32_t game = tree.add_child(app, "PPSA00001", Kind::directory);
    tree.add_totals(game, 100, 90, 3, 0);
    EXPECT_EQ(tree.path(game), "/user/app/PPSA00001");
    EXPECT_EQ(tree.node(tree.root()).disk, 100u);
    EXPECT_EQ(tree.node(app).files, 3u);
    EXPECT_EQ(tree.depth(game), 2);
    EXPECT_EQ(tree.child_towards(tree.root(), game), app);
    EXPECT_EQ(tree.child_towards(game, app), kNoNode);
    Tree slash("/");
    EXPECT_EQ(slash.path(slash.add_child(0, "data", Kind::directory)), "/data");
}

TEST(DiskTree, AdoptRejectsLoopsAndBadOffsets)
{
    Tree tree("/r");
    const std::uint32_t a = tree.add_child(0, "a", Kind::directory);
    tree.add_child(a, "b", Kind::file);
    Tree copy;
    EXPECT_TRUE(copy.adopt(tree.nodes(), tree.pool()));
    EXPECT_EQ(copy.path(2), "/r/a/b");

    std::vector<Node> loop = tree.nodes();
    loop[1].next_sibling = 1;
    EXPECT_FALSE(copy.adopt(loop, tree.pool()));
    std::vector<Node> past = tree.nodes();
    past[2].name_size = 999;
    EXPECT_FALSE(copy.adopt(past, tree.pool()));
    std::vector<Node> orphan = tree.nodes();
    orphan[2].parent = 5;
    EXPECT_FALSE(copy.adopt(orphan, tree.pool()));
}

TEST(DiskScanner, FixtureFollowsTheRules)
{
    const auto fs = make_fixture_filesystem();
    const auto tree = scan(*fs, kFixtureRoot);
    ASSERT_TRUE(tree);
    expect_consistent(*tree, tree->root());

    // Another device under the root is not entered (F3).
    const std::uint32_t mnt = find_child(*tree, tree->root(), "mnt_other");
    EXPECT_EQ(mnt, kNoNode);

    // A folder that cannot be read is marked, not fatal (F6).
    const std::uint32_t tmp = find_child(*tree, tree->root(), "system_tmp");
    ASSERT_NE(tmp, kNoNode);
    EXPECT_TRUE(tree->node(tmp).flags & kUnreadable);

    // Two names of one file count once (F4).
    const std::uint32_t shared = find_child(*tree, tree->root(), "shared");
    ASSERT_NE(shared, kNoNode);
    EXPECT_EQ(tree->node(shared).files, 1u);
    EXPECT_LT(tree->node(shared).disk, 1600ull * 1000 * 1000);

    // Beyond the 32 largest, a folder's files roll up into one node.
    const std::uint32_t trophy = find_path(*tree, {"home", "10000000", "trophy"});
    ASSERT_NE(trophy, kNoNode);
    int kept = 0;
    std::uint32_t other = kNoNode;
    for (const std::uint32_t c : tree->children(trophy))
    {
        if (tree->node(c).kind == Kind::file)
            ++kept;
        if (tree->node(c).kind == Kind::other_files)
            other = c;
    }
    EXPECT_EQ(kept, 32);
    ASSERT_NE(other, kNoNode);
    EXPECT_EQ(tree->node(other).files, 368u);
    EXPECT_EQ(tree->node(trophy).files, 400u);
}

TEST(DiskScanner, SameTotalsWithOneOrManyThreads)
{
    const auto fs = make_fixture_filesystem();
    const auto one = scan(*fs, kFixtureRoot, 1);
    const auto many = scan(*fs, kFixtureRoot, 8);
    ASSERT_TRUE(one && many);
    EXPECT_EQ(one->node(0).disk, many->node(0).disk);
    EXPECT_EQ(one->node(0).files, many->node(0).files);
    EXPECT_EQ(one->size(), many->size());
}

TEST(DiskScanner, CancelLeavesNoTree)
{
    const auto fs = make_fixture_filesystem(1.0); // paced, never advanced
    Scanner scanner;
    ScanOptions options;
    options.root = kFixtureRoot;
    options.threads = 2;
    ASSERT_TRUE(scanner.start(*fs, options));
    scanner.cancel();
    scanner.join();
    const ScanProgress p = scanner.progress();
    EXPECT_FALSE(p.running);
    EXPECT_FALSE(p.finished);
    EXPECT_TRUE(p.cancelled);
    EXPECT_EQ(scanner.take(), nullptr);
}

TEST(DiskScanner, MissingRootDoesNotStart)
{
    const auto fs = make_fixture_filesystem();
    Scanner scanner;
    ScanOptions options;
    options.root = "/nowhere";
    EXPECT_FALSE(scanner.start(*fs, options));
}

class PosixScan : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        char tmpl[] = "/tmp/pd-scan-XXXXXX";
        ASSERT_NE(mkdtemp(tmpl), nullptr);
        root_ = tmpl;
    }
    void TearDown() override
    {
        chmod((root_ + "/locked").c_str(), 0755);
        const std::string command = "rm -rf '" + root_ + "'";
        EXPECT_EQ(std::system(command.c_str()), 0);
    }
    void write(const std::string &relative, std::size_t bytes)
    {
        std::FILE *f = std::fopen((root_ + "/" + relative).c_str(), "wb");
        ASSERT_NE(f, nullptr);
        const std::string data(bytes, 'x');
        std::fwrite(data.data(), 1, data.size(), f);
        std::fclose(f);
    }
    std::string root_;
};

TEST_F(PosixScan, HardLinksSymlinksAndLockedFolders)
{
    ASSERT_EQ(mkdir((root_ + "/a").c_str(), 0755), 0);
    ASSERT_EQ(mkdir((root_ + "/locked").c_str(), 0755), 0);
    write("a/big.bin", 1 << 20);
    write("locked/secret.bin", 1 << 20);
    ASSERT_EQ(link((root_ + "/a/big.bin").c_str(), (root_ + "/twin.bin").c_str()), 0);
    ASSERT_EQ(symlink((root_ + "/a").c_str(), (root_ + "/to_a").c_str()), 0);
    ASSERT_EQ(chmod((root_ + "/locked").c_str(), 0), 0);

    const auto fs = make_posix_filesystem();
    const auto tree = scan(*fs, root_);
    ASSERT_TRUE(tree);
    expect_consistent(*tree, tree->root());

    // The symlink is a file of its own, not a second copy of a/.
    const std::uint32_t link_node = find_child(*tree, tree->root(), "to_a");
    ASSERT_NE(link_node, kNoNode);
    EXPECT_EQ(tree->node(link_node).kind, Kind::file);
    EXPECT_EQ(tree->node(link_node).folders, 0u);

    // big.bin and twin.bin are one file: exactly one of them is counted.
    const bool a_counted = find_path(*tree, {"a", "big.bin"}) != kNoNode;
    const bool twin_counted = find_child(*tree, tree->root(), "twin.bin") != kNoNode;
    EXPECT_NE(a_counted, twin_counted);

    const std::uint32_t locked = find_child(*tree, tree->root(), "locked");
    ASSERT_NE(locked, kNoNode);
    if (geteuid() != 0) // root reads anything
    {
        EXPECT_TRUE(tree->node(locked).flags & kUnreadable);
    }
}

TEST(DiskSunburst, RingsFillTheCircleAndMergeSlivers)
{
    Tree tree("/v");
    const std::uint32_t big = tree.add_child(0, "big", Kind::directory);
    tree.add_totals(big, 900, 900, 1, 0);
    const std::uint32_t inner = tree.add_child(big, "inner", Kind::directory);
    tree.add_totals(inner, 400, 400, 1, 0);
    for (int i = 0; i < 20; ++i)
    {
        const std::uint32_t tiny = tree.add_child(0, "t" + std::to_string(i), Kind::file);
        tree.node(tiny).disk = 1;
        tree.add_totals(0, 1, 1, 1, 0);
    }

    SunburstOptions options;
    options.hidden = 100;
    const std::vector<Segment> segments = layout_sunburst(tree, 0, options);
    float ring1 = 0.0f;
    int smaller = 0;
    int hidden = 0;
    for (const Segment &s : segments)
        if (s.ring == 1)
        {
            ring1 += s.sweep;
            smaller += s.kind == SegmentKind::smaller ? 1 : 0;
            hidden += s.kind == SegmentKind::hidden ? 1 : 0;
        }
    EXPECT_NEAR(ring1, 6.2831853f, 1e-3f);
    EXPECT_EQ(smaller, 1);
    EXPECT_EQ(hidden, 1);
    EXPECT_EQ(segments.front().node, big); // largest first
    EXPECT_EQ(segments.front().start, 0.0f);

    // The second ring sits inside its parent's angle.
    for (const Segment &s : segments)
        if (s.ring == 2 && s.node == inner)
        {
            EXPECT_GE(s.start, segments.front().start);
            EXPECT_LE(s.start + s.sweep, segments.front().start + segments.front().sweep + 1e-4f);
        }
    EXPECT_EQ(segment_at(segments, 0.1f), 0);
    EXPECT_EQ(segment_at(segments, -0.01f), 2); // just before 12 o'clock: the hidden share
}

TEST(DiskSunburst, SortsBySizeOrName)
{
    Tree tree("/v");
    const std::uint32_t b = tree.add_child(0, "beta", Kind::file);
    const std::uint32_t a = tree.add_child(0, "Alpha", Kind::file);
    tree.node(b).disk = 10;
    tree.node(a).disk = 5;
    EXPECT_EQ(sorted_children(tree, 0, false).front(), b);
    EXPECT_EQ(sorted_children(tree, 0, true).front(), a);
}

TEST(DiskCache, RoundTripsAndRejectsDamage)
{
    const auto fs = make_fixture_filesystem();
    const auto tree = scan(*fs, kFixtureRoot);
    ASSERT_TRUE(tree);
    CacheInfo info;
    info.volume_key = "internal";
    info.space = {825, 602, 223};
    info.scanned_at = 1794931200;
    info.duration_ms = 4321;
    info.unreadable = 1;
    const std::string bytes = encode_cache(info, *tree);

    CachedScan back;
    ASSERT_TRUE(decode_cache(bytes, &back));
    EXPECT_EQ(back.info.volume_key, "internal");
    EXPECT_EQ(back.info.space.used, 602u);
    EXPECT_EQ(back.info.duration_ms, 4321u);
    EXPECT_EQ(back.tree->size(), tree->size());
    EXPECT_EQ(back.tree->node(0).disk, tree->node(0).disk);
    EXPECT_EQ(back.tree->path(static_cast<std::uint32_t>(tree->size() - 1)),
              tree->path(static_cast<std::uint32_t>(tree->size() - 1)));

    std::string flipped = bytes;
    flipped[bytes.size() / 2] ^= 0x40;
    EXPECT_FALSE(decode_cache(flipped, &back));
    std::string newer = bytes;
    newer[4] = 2; // schema version
    EXPECT_FALSE(decode_cache(newer, &back));
    EXPECT_FALSE(decode_cache(bytes.substr(0, bytes.size() - 1), &back));
    EXPECT_FALSE(decode_cache("", &back));

    char dir[] = "/tmp/pd-cache-XXXXXX";
    ASSERT_NE(mkdtemp(dir), nullptr);
    const std::string nested = std::string(dir) + "/a/b";
    EXPECT_EQ(save_cache(nested, info, *tree), "");
    CacheInfo header;
    ASSERT_TRUE(read_cache_info(cache_path(nested, "internal"), &header));
    EXPECT_EQ(header.scanned_at, info.scanned_at);
    EXPECT_EQ(header.volume_key, "internal");
    CachedScan loaded;
    ASSERT_TRUE(load_cache(cache_path(nested, "internal"), &loaded));
    EXPECT_EQ(loaded.tree->size(), tree->size());
    EXPECT_FALSE(read_cache_info(nested + "/missing.pdc", &header));
    EXPECT_EQ(std::system(("rm -rf '" + std::string(dir) + "'").c_str()), 0);

    EXPECT_EQ(cache_path("/data/pd", "usb0"), "/data/pd/usb0.pdc");
    EXPECT_EQ(cache_path("/c", "../x"), "/c/x.pdc");
}

TEST(DiskTitles, RecognisesIdsAndReadsParamJson)
{
    EXPECT_TRUE(is_title_id("PPSA01467"));
    EXPECT_TRUE(is_title_id("CUSA00001"));
    EXPECT_FALSE(is_title_id("ppsa01467"));
    EXPECT_FALSE(is_title_id("PPSA0146"));
    EXPECT_FALSE(is_title_id("download"));

    EXPECT_EQ(
        title_name_from_param_json(
            R"({"localizedParameters":{"defaultLanguage":"ja-JP","en-US":{"titleName":"English"},)"
            R"("ja-JP":{"titleName":"日本"}},"ageLevel":[1,2]})"),
        "\xe6\x97\xa5\xe6\x9c\xac");
    EXPECT_EQ(title_name_from_param_json(
                  R"({"localizedParameters":{"en-US":{"titleName":"Hi \"there\""}}})"),
              "Hi \"there\"");
    EXPECT_EQ(title_name_from_param_json(R"({"titleName":"Flat"})"), "Flat");
    EXPECT_EQ(title_name_from_param_json("{broken"), "");
}

TEST(DiskTitles, LoaderReadsNamesAndIconsInTheBackground)
{
    const auto fs = make_fixture_filesystem();
    TitleLoader loader(*fs, {std::string(kFixtureRoot) + "/appmeta"}, 32);
    loader.request("PPSA10021");
    loader.request("PPSA10021"); // asked twice, answered once
    loader.request("PPSA99999");
    loader.wait_idle();
    const std::vector<TitleInfo> done = loader.poll();
    ASSERT_EQ(done.size(), 2u);
    EXPECT_EQ(done[0].name, "Starlight Courier");
    EXPECT_EQ(done[0].icon.width, 32);
    EXPECT_TRUE(done[1].name.empty());
    EXPECT_TRUE(loader.poll().empty());
}

TEST(DiskPng, StoredPngDecodesAndShrinks)
{
    std::vector<std::uint8_t> rgba(8 * 8 * 4);
    for (std::size_t i = 0; i < rgba.size(); i += 4)
    {
        rgba[i] = 200;
        rgba[i + 1] = 100;
        rgba[i + 2] = 50;
        rgba[i + 3] = 255;
    }
    const std::string png = encode_png(8, 8, rgba.data());
    Image image;
    ASSERT_TRUE(decode_png(png, 4, &image));
    EXPECT_EQ(image.width, 4);
    EXPECT_EQ(image.height, 4);
    EXPECT_EQ(image.rgba[0], 200);
    EXPECT_EQ(image.rgba[1], 100);
    EXPECT_FALSE(decode_png("not a png", 4, &image));
}

TEST(DiskFormat, ReadableNumbers)
{
    EXPECT_EQ(format_bytes(0), "0 B");
    EXPECT_EQ(format_bytes(48200000000ull), "48.2 GB");
    EXPECT_EQ(format_bytes(110400000000ull), "110 GB");
    EXPECT_EQ(format_bytes(1500000), "1.50 MB");
    EXPECT_EQ(format_count(1234567), "1,234,567");
    EXPECT_EQ(format_duration(4.25), "4.2 s");
    EXPECT_EQ(format_duration(72.0), "1 min 12 s");
    EXPECT_EQ(format_age(30), "just now");
    EXPECT_EQ(format_age(7200), "2 h ago");
    EXPECT_EQ(format_age(86400), "1 day ago");
    EXPECT_EQ(format_date(1794931200), "2026-11-17 16:00");
    EXPECT_EQ(format_date(1794873600), "2026-11-17 00:00");
    EXPECT_EQ(format_share(18, 100), "18%");
    EXPECT_EQ(format_share(1, 1000), "<1%");
}

TEST(DiskVolumes, FixtureVolumesAreDetected)
{
    const auto fs = make_fixture_filesystem();
    const std::vector<Volume> volumes =
        detect_volumes(*fs, {{"internal", "Internal", kFixtureRoot, VolumeKind::internal},
                             {"usb0", "USB 1", kFixtureUsbRoot, VolumeKind::usb},
                             {"usb1", "USB 2", "/fixture/usb1", VolumeKind::usb}});
    ASSERT_EQ(volumes.size(), 2u);
    EXPECT_TRUE(volumes[0].has_space);
    EXPECT_EQ(volumes[0].space.total, kFixtureTotal);
    EXPECT_EQ(default_threads(VolumeKind::usb), 2);
    EXPECT_EQ(default_threads(VolumeKind::internal), 8);
}

} // namespace
