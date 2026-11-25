// Prospero Disk - Tests for the scanner, the layout, the cache and title lookup.
// Copyright (C) 2026 Leonardo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later

#include "disk/cache.hpp"
#include "disk/format.hpp"
#include "disk/png.hpp"
#include "disk/scanner.hpp"
#include "disk/sqlite.hpp"
#include "disk/sunburst.hpp"
#include "disk/titles.hpp"
#include "disk/volumes.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <map>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

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
    EXPECT_EQ(title_id_of_folder("PPSA01467"), "PPSA01467");
    EXPECT_EQ(title_id_of_folder("PPSA28416-app0"), "PPSA28416");
    EXPECT_EQ(title_id_of_folder("PPSA28416-patch0"), "PPSA28416");
    EXPECT_EQ(title_id_of_folder("CUSA00001_00"), "CUSA00001");
    EXPECT_EQ(title_id_of_folder("PPSA284160"), "");
    EXPECT_EQ(title_id_of_folder("PPSA2841-app0"), "");
    EXPECT_EQ(title_id_of_folder("download0"), "");

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

// The console's app.db, in miniature: one table per user, as on a PS5.
TEST(DiskTitles, AppDbNamesWhatParamJsonDoesNot)
{
    char dir[] = "/tmp/pd-appdb-XXXXXX";
    ASSERT_NE(mkdtemp(dir), nullptr);
    const std::string path = std::string(dir) + "/app.db";
    sqlite3 *db = nullptr;
    ASSERT_EQ(sqlite3_open_v2(path.c_str(), &db,
                              hui::disk::sqlite::kOpenReadWrite | hui::disk::sqlite::kOpenCreate,
                              nullptr),
              hui::disk::sqlite::kOk);
    ASSERT_EQ(sqlite3_exec(db,
                           "CREATE TABLE tbl_appbrowse_0507646227 (titleId TEXT, titleName TEXT);"
                           "CREATE TABLE tbl_other (titleId TEXT, titleName TEXT);"
                           "INSERT INTO tbl_other VALUES ('PPSA55555', 'Wrong table');"
                           "INSERT INTO tbl_appbrowse_0507646227 VALUES "
                           "('PPSA55555', 'Orchard \"Two\"');",
                           nullptr, nullptr, nullptr),
              hui::disk::sqlite::kOk);
    sqlite3_close(db);

    EXPECT_EQ(title_name_from_app_db(path, "PPSA55555"), "Orchard \"Two\"");
    EXPECT_EQ(title_name_from_app_db(path, "PPSA00000"), "");
    EXPECT_EQ(title_name_from_app_db(std::string(dir) + "/missing.db", "PPSA55555"), "");

    // The loader asks app.db only when no param.json names the title.
    const auto fs = make_fixture_filesystem();
    TitleLoader loader(*fs, {std::string(kFixtureRoot) + "/appmeta"}, 32, path);
    loader.request("PPSA55555");
    loader.request("PPSA10021");
    loader.wait_idle();
    const std::vector<TitleInfo> done = loader.poll();
    ASSERT_EQ(done.size(), 2u);
    EXPECT_EQ(done[0].name, "Orchard \"Two\"");
    EXPECT_EQ(done[1].name, "Starlight Courier");
    EXPECT_EQ(std::system(("rm -rf '" + std::string(dir) + "'").c_str()), 0);
}

// app.db as PS5 firmware 13.60 lays it out: no tbl_appbrowse_* tables, the
// name and an icon path in tbl_contentinfo. Retail games' appmeta folders are
// 0700, so for them this is the only source the app can read.
TEST(DiskTitles, AppDbContentInfoNamesAndFindsTheIcon)
{
    char dir[] = "/tmp/pd-appdb5-XXXXXX";
    ASSERT_NE(mkdtemp(dir), nullptr);
    const std::string root = dir;
    const std::string path = root + "/app.db";
    const std::string icon_dir = root + "/catalog/PPSA77777";
    ASSERT_EQ(std::system(("mkdir -p '" + icon_dir + "'").c_str()), 0);
    std::vector<std::uint8_t> rgba(8 * 8 * 4, 255);
    const std::string png = encode_png(8, 8, rgba.data());
    FILE *file = std::fopen((icon_dir + "/icon0.png").c_str(), "wb");
    ASSERT_NE(file, nullptr);
    ASSERT_EQ(std::fwrite(png.data(), 1, png.size(), file), png.size());
    std::fclose(file);

    sqlite3 *db = nullptr;
    ASSERT_EQ(sqlite3_open_v2(path.c_str(), &db,
                              hui::disk::sqlite::kOpenReadWrite | hui::disk::sqlite::kOpenCreate,
                              nullptr),
              hui::disk::sqlite::kOk);
    const std::string sql =
        "CREATE TABLE tbl_contentinfo (titleId TEXT, titleName TEXT, icon0Info TEXT);"
        "INSERT INTO tbl_contentinfo VALUES ('PPSA77777', 'Lantern Bay', '" +
        icon_dir +
        "/icon0.png?ts=1791699632');"
        "INSERT INTO tbl_contentinfo VALUES ('CUSA77777', 'Old Harbour', "
        "'http://example.invalid/icon0.png');"
        "INSERT INTO tbl_contentinfo VALUES ('PPSA66666', 'No Icon', NULL);";
    ASSERT_EQ(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr), hui::disk::sqlite::kOk);
    sqlite3_close(db);

    const AppDbTitle lantern = title_from_app_db(path, "PPSA77777");
    EXPECT_EQ(lantern.name, "Lantern Bay");
    EXPECT_EQ(lantern.icon_path, icon_dir + "/icon0.png");
    const AppDbTitle harbour = title_from_app_db(path, "CUSA77777");
    EXPECT_EQ(harbour.name, "Old Harbour");
    EXPECT_EQ(harbour.icon_path, ""); // not a file path
    EXPECT_EQ(title_from_app_db(path, "PPSA66666").icon_path, "");
    EXPECT_EQ(title_name_from_app_db(path, "PPSA00000"), "");

    // A layout without the icon column still gives the name.
    const std::string bare = root + "/bare.db";
    ASSERT_EQ(sqlite3_open_v2(bare.c_str(), &db,
                              hui::disk::sqlite::kOpenReadWrite | hui::disk::sqlite::kOpenCreate,
                              nullptr),
              hui::disk::sqlite::kOk);
    ASSERT_EQ(sqlite3_exec(db,
                           "CREATE TABLE tbl_contentinfo (titleId TEXT, titleName TEXT);"
                           "INSERT INTO tbl_contentinfo VALUES ('PPSA77777', 'Lantern Bay');",
                           nullptr, nullptr, nullptr),
              hui::disk::sqlite::kOk);
    sqlite3_close(db);
    EXPECT_EQ(title_from_app_db(bare, "PPSA77777").name, "Lantern Bay");
    EXPECT_EQ(title_from_app_db(bare, "PPSA77777").icon_path, "");

    // No appmeta folder can be opened: the name and the icon come from app.db.
    const auto fs = make_posix_filesystem(false);
    TitleLoader loader(*fs, {root + "/locked-appmeta"}, 4, path);
    loader.request("PPSA77777");
    loader.request("CUSA77777");
    loader.wait_idle();
    const std::vector<TitleInfo> done = loader.poll();
    ASSERT_EQ(done.size(), 2u);
    EXPECT_EQ(done[0].name, "Lantern Bay");
    EXPECT_EQ(done[0].icon.width, 4);
    EXPECT_EQ(done[1].name, "Old Harbour");
    EXPECT_TRUE(done[1].icon.rgba.empty());
    EXPECT_EQ(std::system(("rm -rf '" + root + "'").c_str()), 0);
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

// A FileSystem of folders that each have a device number.
class DeviceFileSystem final : public FileSystem
{
  public:
    explicit DeviceFileSystem(std::vector<std::pair<std::string, std::uint64_t>> folders)
        : folders_(std::move(folders))
    {
    }
    bool list(const std::string &path, std::vector<Entry> *out, int *error) const override
    {
        out->clear();
        Entry e;
        *error = ENOENT;
        return stat(path, &e);
    }
    bool stat(const std::string &path, Entry *out) const override
    {
        for (const auto &[name, device] : folders_)
        {
            if (name == path)
            {
                out->name = name;
                out->type = EntryType::directory;
                out->device = device;
                return true;
            }
        }
        return false;
    }
    bool space(const std::string &, SpaceInfo *) const override
    {
        return false;
    }
    bool read_file(const std::string &, std::string *, std::size_t) const override
    {
        return false;
    }

  private:
    std::vector<std::pair<std::string, std::uint64_t>> folders_;
};

TEST(DiskVolumes, EmptyMountFoldersAreNotDrives)
{
    // As on a console with one M.2 drive and one USB drive: /mnt/ext0 and
    // /mnt/usb1 are listable folders of /mnt's own file system.
    const DeviceFileSystem fs({{"/user", 0},
                               {"/mnt", 7},
                               {"/mnt/ext0", 7},
                               {"/mnt/ext1", 0},
                               {"/mnt/usb0", 255},
                               {"/mnt/usb1", 7}});
    const std::vector<Volume> volumes =
        detect_volumes(fs, {{"internal", "Internal", "/user", VolumeKind::internal},
                            {"ext0", "Extended", "/mnt/ext0", VolumeKind::extended},
                            {"ext1", "Extended 2", "/mnt/ext1", VolumeKind::extended},
                            {"usb0", "USB 1", "/mnt/usb0", VolumeKind::usb},
                            {"usb1", "USB 2", "/mnt/usb1", VolumeKind::usb}});
    ASSERT_EQ(volumes.size(), 3u);
    EXPECT_EQ(volumes[0].spec.key, "internal");
    EXPECT_EQ(volumes[1].spec.key, "ext1");
    EXPECT_EQ(volumes[2].spec.key, "usb0");
}

// Folders with files in them, kept in memory.
class MemoryFileSystem final : public FileSystem
{
  public:
    void folder(const std::string &parent, const std::string &name)
    {
        Entry e;
        e.name = name;
        e.type = EntryType::directory;
        dirs_[parent].push_back(e);
    }
    void file(const std::string &path, const std::string &text)
    {
        files_[path] = text;
    }
    bool list(const std::string &path, std::vector<Entry> *out, int *error) const override
    {
        const auto it = dirs_.find(path);
        if (it == dirs_.end())
        {
            *error = ENOENT;
            return false;
        }
        *out = it->second;
        return true;
    }
    bool stat(const std::string &, Entry *) const override
    {
        return false;
    }
    bool space(const std::string &, SpaceInfo *) const override
    {
        return false;
    }
    bool read_file(const std::string &path, std::string *out, std::size_t) const override
    {
        const auto it = files_.find(path);
        if (it == files_.end())
            return false;
        *out = it->second;
        return true;
    }

  private:
    std::map<std::string, std::vector<Entry>> dirs_;
    std::map<std::string, std::string> files_;
};

TEST(PlaydenTitles, OwnerFilesMapInstallsToTitles)
{
    MemoryFileSystem fs;
    fs.folder("/hb", "FAKE78024");
    fs.file("/hb/FAKE78024/.playden-title-owner",
            "schema=1\nkind=title\ninstall_id=steam-2868840\ntitle_id=FAKE78024\n");
    fs.folder("/hb", "FAKE78001");
    fs.file("/hb/FAKE78001/.playden-title-owner",
            "install_id=gog-1207658695\r\ntitle_id=FAKE78001\r\n");
    // A copied owner file names another title: not believed.
    fs.folder("/hb", "FAKE78002");
    fs.file("/hb/FAKE78002/.playden-title-owner", "install_id=steam-1\ntitle_id=FAKE78024\n");
    fs.folder("/hb", "PPSA93475"); // a title without the file
    fs.folder("/hb", "notes");     // not a title folder
    fs.file("/hb/notes/.playden-title-owner", "install_id=steam-2\ntitle_id=notes\n");

    const auto titles = read_playden_titles(fs, "/hb");
    ASSERT_EQ(titles.size(), 2u);
    EXPECT_EQ(titles.at("steam-2868840"), "FAKE78024");
    EXPECT_EQ(titles.at("gog-1207658695"), "FAKE78001");
    EXPECT_TRUE(read_playden_titles(fs, "/missing").empty());
}

TEST(PlaydenTitles, InstallIdOnlyInsidePlaydenGameFolders)
{
    EXPECT_EQ(playden_install_id("/mnt/usb0/playden/games", "2610"), "steam-2610");
    EXPECT_EQ(playden_install_id("/data/playden-games", "gog-1207658695"), "gog-1207658695");
    EXPECT_EQ(playden_install_id("/user/data/playden-games", "220"), "steam-220");
    EXPECT_EQ(playden_install_id("/data/playden/games", "steam-440"), "steam-440");
    EXPECT_EQ(playden_install_id("/mnt/ext1/playden/games", "local-8a8066ef"), "local-8a8066ef");
    EXPECT_EQ(playden_install_id("/mnt/usb0/playden/games", ".playden-saves"), "");
    EXPECT_EQ(playden_install_id("/mnt/usb0/playden/import", "2610"), "");
    EXPECT_EQ(playden_install_id("/mnt/usb0/games", "2610"), "");
    EXPECT_EQ(playden_install_id("/mnt/usb0/playden/games/2610", "data"), "");
}

} // namespace
