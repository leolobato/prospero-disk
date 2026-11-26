// Prospero Disk - Tests for collect and delete: removal, the tree edits, the collection, the
// deleter. Copyright (C) 2026 Leonardo Lobato SPDX-License-Identifier: GPL-3.0-or-later

#include "disk/filesystem.hpp"

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

} // namespace
