// Prospero Disk - Tests for collect and delete: removal, the tree edits, the collection, the
// deleter. Copyright (C) 2026 Leonardo Lobato SPDX-License-Identifier: GPL-3.0-or-later

#include "disk/cache.hpp"
#include "disk/filesystem.hpp"
#include "disk/tree.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace
{

using namespace hui::disk;

// A temporary folder on the PC, removed after each test.
class TempDir : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        char tmpl[] = "/tmp/pd-delete-XXXXXX";
        ASSERT_NE(mkdtemp(tmpl), nullptr);
        root_ = tmpl;
    }
    void TearDown() override
    {
        const std::string command = "chmod -R u+rwx '" + root_ + "'; rm -rf '" + root_ + "'";
        EXPECT_EQ(std::system(command.c_str()), 0);
    }
    void write(const std::string &relative, std::size_t bytes = 4096)
    {
        std::FILE *f = std::fopen(path(relative).c_str(), "wb");
        ASSERT_NE(f, nullptr);
        const std::string data(bytes, 'x');
        std::fwrite(data.data(), 1, data.size(), f);
        std::fclose(f);
    }
    void folder(const std::string &relative)
    {
        ASSERT_EQ(mkdir(path(relative).c_str(), 0755), 0);
    }
    std::string path(const std::string &relative) const
    {
        return root_ + "/" + relative;
    }
    bool exists(const std::string &relative) const
    {
        struct stat st
        {
        };
        return lstat(path(relative).c_str(), &st) == 0;
    }
    std::string root_;
};

using PosixRemoval = TempDir;

TEST_F(PosixRemoval, RefusesUnlessAllowed)
{
    write("a.bin");
    folder("empty");
    int error = 0;
    const auto locked = make_posix_filesystem(true, false);
    EXPECT_FALSE(locked->remove_file(path("a.bin"), &error));
    EXPECT_EQ(error, EROFS);
    EXPECT_FALSE(locked->remove_folder(path("empty"), &error));
    EXPECT_TRUE(exists("a.bin"));
    EXPECT_TRUE(exists("empty"));

    const auto open = make_posix_filesystem(true, true);
    EXPECT_TRUE(open->remove_file(path("a.bin"), &error));
    EXPECT_TRUE(open->remove_folder(path("empty"), &error));
    EXPECT_FALSE(exists("a.bin"));
    EXPECT_FALSE(exists("empty"));
}

TEST_F(PosixRemoval, RemovesALinkNotItsTarget)
{
    folder("target");
    write("target/keep.bin");
    ASSERT_EQ(symlink(path("target").c_str(), path("link").c_str()), 0);
    const auto fs = make_posix_filesystem(true, true);
    int error = 0;
    // rmdir does not follow a link; unlink removes the link itself.
    EXPECT_FALSE(fs->remove_folder(path("link"), &error));
    EXPECT_EQ(error, ENOTDIR);
    EXPECT_TRUE(fs->remove_file(path("link"), &error));
    EXPECT_FALSE(exists("link"));
    EXPECT_TRUE(exists("target/keep.bin"));
}

TEST(FixtureRemoval, FreesSpaceAndKeepsTheRules)
{
    const auto fs = make_fixture_filesystem();
    const std::string root = kFixtureRoot;
    SpaceInfo before;
    ASSERT_TRUE(fs->space(root, &before));
    int error = 0;

    // A file: gone from its folder, its space free.
    Entry update;
    ASSERT_TRUE(fs->stat(root + "/download/update_cache.bin", &update));
    EXPECT_TRUE(fs->remove_file(root + "/download/update_cache.bin", &error));
    EXPECT_FALSE(fs->stat(root + "/download/update_cache.bin", &update));
    EXPECT_EQ(errno, ENOENT);
    SpaceInfo after;
    ASSERT_TRUE(fs->space(root, &after));
    EXPECT_EQ(before.used - after.used, update.disk);
    EXPECT_GT(after.free, before.free);

    // A folder with something in it is refused; unlink on a folder too.
    EXPECT_FALSE(fs->remove_folder(root + "/download", &error));
    EXPECT_EQ(error, ENOTEMPTY);
    EXPECT_FALSE(fs->remove_file(root + "/download", &error));
    EXPECT_EQ(error, EPERM);

    // One of two hard links frees nothing; the second frees the file.
    const std::uint64_t used = after.used;
    EXPECT_TRUE(fs->remove_file(root + "/shared/shared.pak", &error));
    ASSERT_TRUE(fs->space(root, &after));
    EXPECT_EQ(after.used, used);
    EXPECT_TRUE(fs->remove_file(root + "/shared/shared_link.pak", &error));
    ASSERT_TRUE(fs->space(root, &after));
    EXPECT_LT(after.used, used);
    EXPECT_TRUE(fs->remove_folder(root + "/shared", &error));
    std::vector<Entry> entries;
    EXPECT_FALSE(fs->list(root + "/shared", &entries, &error));

    // A folder that cannot be read refuses, and so does a volume root.
    EXPECT_FALSE(fs->remove_folder(root + "/system_tmp", &error));
    EXPECT_EQ(error, EACCES);
    EXPECT_FALSE(fs->remove_folder(root, &error));
    EXPECT_EQ(error, EBUSY);
    EXPECT_FALSE(fs->remove_file(root + "/nothing", &error));
    EXPECT_EQ(error, ENOENT);
}

TEST(FixtureRemoval, UsbSpaceIsItsOwn)
{
    const auto fs = make_fixture_filesystem();
    const std::string usb = kFixtureUsbRoot;
    SpaceInfo internal_before;
    SpaceInfo usb_before;
    ASSERT_TRUE(fs->space(kFixtureRoot, &internal_before));
    ASSERT_TRUE(fs->space(usb, &usb_before));
    Entry backup;
    ASSERT_TRUE(fs->stat(usb + "/backup.tar", &backup));
    int error = 0;
    EXPECT_TRUE(fs->remove_file(usb + "/backup.tar", &error));
    SpaceInfo internal_after;
    SpaceInfo usb_after;
    ASSERT_TRUE(fs->space(kFixtureRoot, &internal_after));
    ASSERT_TRUE(fs->space(usb, &usb_after));
    EXPECT_EQ(internal_after.used, internal_before.used);
    EXPECT_EQ(usb_before.used - usb_after.used, backup.disk);
}

// /v: a/ (b.bin 100, c/ (d.bin 50)), e.bin 10
Tree sample()
{
    Tree tree("/v");
    const std::uint32_t a = tree.add_child(0, "a", Kind::directory);
    tree.add_totals(0, 0, 0, 0, 1); // the root counts a
    tree.add_totals(tree.add_child(a, "b.bin", Kind::file), 100, 90, 1, 0);
    const std::uint32_t c = tree.add_child(a, "c", Kind::directory);
    tree.add_totals(a, 0, 0, 0, 1); // a and the root count c
    tree.add_totals(tree.add_child(c, "d.bin", Kind::file), 50, 40, 1, 0);
    tree.add_totals(tree.add_child(0, "e.bin", Kind::file), 10, 9, 1, 0);
    return tree;
}

TEST(TreeEdits, FindsByPath)
{
    const Tree tree = sample();
    EXPECT_EQ(tree.find("/v"), 0u);
    EXPECT_EQ(tree.path(tree.find("/v/a/c/d.bin")), "/v/a/c/d.bin");
    EXPECT_EQ(tree.find("/v/a/x"), kNoNode);
    EXPECT_EQ(tree.find("/vv/a"), kNoNode);
    EXPECT_EQ(tree.find("/w"), kNoNode);
    Tree slash("/");
    const std::uint32_t data = slash.add_child(0, "data", Kind::directory);
    EXPECT_EQ(slash.find("/data"), data);
}

TEST(TreeEdits, RemoveSubtractsUpThePath)
{
    Tree tree = sample();
    const std::uint32_t c = tree.find("/v/a/c");
    tree.remove(c);
    EXPECT_EQ(tree.find("/v/a/c"), kNoNode);
    const Node &a = tree.node(tree.find("/v/a"));
    EXPECT_EQ(a.disk, 100u);
    EXPECT_EQ(a.files, 1u);
    EXPECT_EQ(a.folders, 0u);
    EXPECT_EQ(tree.node(0).disk, 110u);
    EXPECT_EQ(tree.node(0).folders, 1u);
    EXPECT_EQ(tree.node(0).files, 2u);
    tree.remove(c); // twice changes nothing
    EXPECT_EQ(tree.node(0).disk, 110u);
    tree.remove(0); // never the root
    EXPECT_EQ(tree.node(0).disk, 110u);
}

TEST(TreeEdits, ReplaceGraftsWhatIsLeft)
{
    Tree tree = sample();
    Tree left("/v/a");
    const std::uint32_t c = left.add_child(0, "c", Kind::directory);
    left.add_totals(0, 0, 0, 0, 1);
    left.add_totals(left.add_child(c, "d.bin", Kind::file), 50, 40, 1, 0);
    const std::uint32_t a = tree.replace(tree.find("/v/a"), left);
    ASSERT_NE(a, kNoNode);
    EXPECT_EQ(tree.find("/v/a"), a);
    EXPECT_EQ(tree.find("/v/a/b.bin"), kNoNode);
    EXPECT_NE(tree.find("/v/a/c/d.bin"), kNoNode);
    EXPECT_EQ(tree.node(0).disk, 60u);
    EXPECT_EQ(tree.node(0).files, 2u);
    EXPECT_EQ(tree.node(0).folders, 2u);
}

TEST(TreeEdits, CompactedDropsRemovedNodesAndRoundTrips)
{
    Tree tree = sample();
    tree.remove(tree.find("/v/a"));
    const Tree small = tree.compacted();
    EXPECT_EQ(small.size(), 2u);
    EXPECT_EQ(small.node(0).disk, 10u);
    EXPECT_NE(small.find("/v/e.bin"), kNoNode);
    CacheInfo info;
    info.volume_key = "v";
    CachedScan back;
    ASSERT_TRUE(decode_cache(encode_cache(info, small), &back));
    EXPECT_EQ(back.tree->node(0).disk, 10u);
    EXPECT_EQ(back.tree->size(), 2u);
}

} // namespace
