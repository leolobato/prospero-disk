// Prospero Disk - Tests for collect and delete: removal, tree edits, collection, deleter.
// Copyright (C) 2026 Leonardo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later

#include "disk/cache.hpp"
#include "disk/collection.hpp"
#include "disk/deleter.hpp"
#include "disk/filesystem.hpp"
#include "disk/title_remover.hpp"
#include "disk/tree.hpp"
#include "disk/volumes.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <map>
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

TEST(FixtureRemoval, AMoveTakesContentsAlong)
{
    const auto fs = make_fixture_filesystem();
    const std::string root = kFixtureRoot;
    int error = 0;
    ASSERT_TRUE(fs->move(root + "/appmeta/PPSA10021", root + "/download/moved", &error));
    std::string text;
    EXPECT_TRUE(fs->read_file(root + "/download/moved/param.json", &text, 4096));
    EXPECT_NE(text.find("Starlight Courier"), std::string::npos);
    EXPECT_FALSE(fs->read_file(root + "/appmeta/PPSA10021/param.json", &text, 4096));
    // And back, as a refused uninstall does.
    ASSERT_TRUE(fs->move(root + "/download/moved", root + "/appmeta/PPSA10021", &error));
    EXPECT_TRUE(fs->read_file(root + "/appmeta/PPSA10021/param.json", &text, 4096));
    EXPECT_FALSE(fs->move(root + "/nothing", root + "/download/x", &error));
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

TEST(TreeEdits, AdoptRefusesNamesAScanCannotHave)
{
    for (const std::string &bad : {std::string("a/b"), std::string(".."), std::string("."),
                                   std::string(""), std::string("a\0b", 3)})
    {
        Tree tree("/v");
        tree.add_child(0, bad, Kind::directory);
        Tree copy;
        EXPECT_FALSE(copy.adopt(tree.nodes(), tree.pool())) << bad;
    }
    Tree fine("/v");
    fine.add_child(0, "ok", Kind::directory);
    Tree copy;
    EXPECT_TRUE(copy.adopt(fine.nodes(), fine.pool()));
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

ProtectionRules console_rules()
{
    return console_protection(console_volume_specs(), {{"steam-2610", "FAKE78024"}});
}

TEST(Protection, RefusesWhatMustStay)
{
    const ProtectionRules rules = console_rules();
    const auto verdict = [&](const char *path) { return check_path(rules, path).verdict; };
    EXPECT_EQ(verdict("/user"), Verdict::volume_root);
    EXPECT_EQ(verdict("/mnt/usb0/"), Verdict::volume_root);
    EXPECT_EQ(verdict("/data"), Verdict::volume_root);
    EXPECT_EQ(verdict("/system"), Verdict::protected_path);
    EXPECT_EQ(verdict("/system_data/priv/mms/app.db"), Verdict::protected_path);
    EXPECT_EQ(verdict("/system_ex/app"), Verdict::protected_path);
    EXPECT_EQ(verdict("/preinst/common/font"), Verdict::protected_path);
    EXPECT_EQ(verdict("/user/appmeta"), Verdict::protected_path);
    EXPECT_EQ(verdict("/user/appmeta/PPSA01467/icon0.png"), Verdict::protected_path);
    // Our own install and data, through either spelling of /data.
    EXPECT_EQ(verdict("/data/homebrew/PPSA93475"), Verdict::protected_path);
    EXPECT_EQ(verdict("/user/data/homebrew/PPSA93475/eboot.bin"), Verdict::protected_path);
    EXPECT_EQ(verdict("/user/data/prospero-disk/cache"), Verdict::protected_path);
    EXPECT_EQ(verdict("/data/homebrew/FAKE77031"), Verdict::protected_path);
    EXPECT_EQ(verdict("/user/app/PPSA93475"), Verdict::protected_path);
    // A folder that holds a protected one is refused whole.
    EXPECT_EQ(verdict("/data/homebrew"), Verdict::contains_protected);
    EXPECT_EQ(verdict("/user/data"), Verdict::volume_root); // it is /data
    // "/user/appmetadata" is not inside "/user/appmeta".
    EXPECT_EQ(verdict("/user/appmetadata"), Verdict::folder);
    EXPECT_EQ(verdict("/user/home/1c9ee02b/captures"), Verdict::folder);
    EXPECT_EQ(verdict("/user/download"), Verdict::folder);
    EXPECT_EQ(verdict("/data/homebrew/PPSA99318"), Verdict::folder);         // other homebrew
    EXPECT_EQ(verdict("/data/homebrew/FAKE10101"), Verdict::protected_path); // ShadowMountPlus
    EXPECT_EQ(verdict("/data/../user/appmeta"), Verdict::protected_path);
    EXPECT_EQ(verdict("relative/path"), Verdict::protected_path);
    // The kernel reads "a//b" as "a/b", and stops at a NUL: such paths never
    // pass the rules.
    EXPECT_EQ(verdict("/user//appmeta"), Verdict::protected_path);
    EXPECT_EQ(check_path(rules, std::string_view("/user/appmeta\0x", 15)).verdict,
              Verdict::protected_path);
}

TEST(Protection, GamesAreUninstalledNeverDeletedByHand)
{
    const ProtectionRules rules = console_rules();
    Check c = check_path(rules, "/user/app/PPSA01467");
    EXPECT_EQ(c.verdict, Verdict::title);
    EXPECT_EQ(c.title_id, "PPSA01467");
    EXPECT_TRUE(c.then_delete.empty());
    c = check_path(rules, "/mnt/ext1/user/app/PPSA15508");
    EXPECT_EQ(c.verdict, Verdict::title);
    EXPECT_EQ(c.title_id, "PPSA15508");
    // PS4 games too.
    c = check_path(rules, "/mnt/ext1/user/app/CUSA01047");
    EXPECT_EQ(c.verdict, Verdict::title);
    EXPECT_EQ(c.title_id, "CUSA01047");
    EXPECT_EQ(check_path(rules, "/user/app/PPSA01467/sce_sys").verdict, Verdict::title_part);
    EXPECT_EQ(check_path(rules, "/user/app/FAKE10101").verdict, Verdict::title_part);
    // It holds Prospero Disk's own registration as well as games.
    EXPECT_EQ(check_path(rules, "/user/app").verdict, Verdict::contains_protected);
    EXPECT_EQ(check_path(rules, "/mnt/ext0/user/app").verdict, Verdict::contains_titles);
    // It holds appmeta as well as games: the first rule that refuses names it.
    EXPECT_EQ(check_path(rules, "/mnt/ext1/user").verdict, Verdict::contains_protected);
    EXPECT_EQ(check_path(rules, "/user/patch/PPSA01467").verdict, Verdict::title_part);
    EXPECT_EQ(check_path(rules, "/user/patch").verdict, Verdict::contains_titles);
    EXPECT_EQ(check_path(rules, "/mnt/ext1/user/addcont/PPSA01467").verdict, Verdict::title_part);
}

TEST(Protection, PlaydenTitleOrGameData)
{
    const ProtectionRules rules = console_rules();
    // The title: uninstall it, then delete its folder.
    Check c = check_path(rules, "/data/homebrew/FAKE78024");
    EXPECT_EQ(c.verdict, Verdict::title);
    EXPECT_EQ(c.title_id, "FAKE78024");
    EXPECT_EQ(c.then_delete, std::vector<std::string>{"/data/homebrew/FAKE78024"});
    EXPECT_EQ(check_path(rules, "/user/data/homebrew/FAKE78024").title_id, "FAKE78024");
    EXPECT_EQ(check_path(rules, "/data/homebrew/FAKE78024/eboot.bin").verdict, Verdict::title_part);
    // The game data: uninstall the title, delete the title and the data.
    c = check_path(rules, "/data/playden/games/steam-2610");
    EXPECT_EQ(c.verdict, Verdict::title);
    EXPECT_EQ(c.title_id, "FAKE78024");
    EXPECT_EQ(c.then_delete, (std::vector<std::string>{"/data/homebrew/FAKE78024",
                                                       "/data/playden/games/steam-2610"}));
    EXPECT_EQ(check_path(rules, "/mnt/usb0/playden/games/2610").title_id, "FAKE78024");
    // Playden 0.6 keeps the game files in /data/playden-games, seen as /user/data.
    c = check_path(rules, "/user/data/playden-games/2610");
    EXPECT_EQ(c.verdict, Verdict::title);
    EXPECT_EQ(c.title_id, "FAKE78024");
    EXPECT_EQ(check_path(rules, "/data/playden-games").verdict, Verdict::contains_titles);
    // Its files: Playden is asked to forget the install. Its bottle keeps the
    // saves Playden brings back on a new install: not asked.
    EXPECT_EQ(c.playden_game, "2610");
    EXPECT_EQ(check_path(rules, "/data/playden/games/steam-2610").playden_game, "");
    // A game whose title is gone (removed first): plain files, and Playden is
    // still asked to forget it. Not for a part of it, or a name it can't take.
    c = check_path(rules, "/user/data/playden-games/464260");
    EXPECT_EQ(c.verdict, Verdict::folder);
    EXPECT_EQ(c.playden_game, "464260");
    EXPECT_EQ(check_path(rules, "/data/playden-games/gog-1425293328").playden_game,
              "gog-1425293328");
    EXPECT_EQ(check_path(rules, "/data/playden-games/464260/Data").playden_game, "");
    EXPECT_EQ(check_path(rules, "/data/playden-games/epic-6a398fcd").playden_game, "");
    EXPECT_EQ(check_path(rules, "/data/playden/games/steam-1").playden_game, "");
    // The title in /user/app is the title in /data/homebrew.
    c = check_path(rules, "/user/app/FAKE78024");
    EXPECT_EQ(c.verdict, Verdict::title);
    EXPECT_EQ(c.then_delete, std::vector<std::string>{"/data/homebrew/FAKE78024"});
    // Inside a game with a title: refused, collect the game.
    EXPECT_EQ(check_path(rules, "/data/playden/games/steam-2610/drive_c").verdict,
              Verdict::title_part);
    // A game folder Playden has no title for is plain files.
    EXPECT_EQ(check_path(rules, "/data/playden/games/steam-1").verdict, Verdict::folder);
    EXPECT_EQ(check_path(rules, "/data/playden/games").verdict, Verdict::contains_titles);
    EXPECT_EQ(check_path(rules, "/data/playden").verdict, Verdict::contains_titles);
}

// ShadowMountPlus registers a title again while the folder it installs it
// from is there (console run 3): the title and that folder go together.
TEST(Protection, ShadowMountTitleAndItsSource)
{
    const std::string control = "/mnt/ext1/homebrew/CONTROL Resonant 1.300.300 PPSA34547 PS5";
    const ProtectionRules rules =
        console_protection(console_volume_specs(), {{"steam-2610", "FAKE78024"}},
                           {{"PPSA34547", control},
                            {"PPSA93475", "/user/data/homebrew/PPSA93475"},
                            {"FAKE10101", "/data/homebrew/FAKE10101"}});
    Check c = check_path(rules, "/user/app/PPSA34547");
    EXPECT_EQ(c.verdict, Verdict::title);
    EXPECT_EQ(c.then_delete, std::vector<std::string>{control});
    // The folder itself is the same job.
    c = check_path(rules, control);
    EXPECT_EQ(c.verdict, Verdict::title);
    EXPECT_EQ(c.title_id, "PPSA34547");
    EXPECT_EQ(c.then_delete, std::vector<std::string>{control});
    EXPECT_EQ(check_path(rules, control + "/sce_sys").verdict, Verdict::title_part);
    EXPECT_EQ(check_path(rules, "/mnt/ext1/homebrew").verdict, Verdict::contains_titles);
    // Our own install stays, by its title too; ShadowMount itself is not a game.
    EXPECT_EQ(check_path(rules, "/user/app/PPSA93475").verdict, Verdict::protected_path);
    EXPECT_EQ(check_path(rules, "/data/homebrew/FAKE10101").verdict, Verdict::protected_path);
    EXPECT_EQ(check_path(rules, "/user/app/FAKE10101").verdict, Verdict::title_part);
}

// A record that doesn't add up is refused from both sides, never guessed
// (Codex review of c2e46e1).
TEST(Protection, UnclearShadowMountRecordsAreRefused)
{
    const ProtectionRules rules = console_protection(
        console_volume_specs(), {},
        {{"PPSA00001", "/mnt/ext1/games/Elsewhere"},  // not where ShadowMount looks
         {"PPSA00002", "/data/homebrew/FAKE10101"},   // ShadowMount's own folder
         {"PPSA00003", "/data/homebrew/PPSA00009"},   // another title's folder
         {"PPSA00004", ""},                           // a link that didn't add up
         {"PPSA00005", "/user/data/homebrew/Shared"}, // two titles, one folder,
         {"PPSA00006", "/data/homebrew/Shared"},      // through both spellings
         {"PPSA00007", "/mnt/ext1/homebrew/Fine Game"}});
    for (const char *title :
         {"PPSA00001", "PPSA00002", "PPSA00003", "PPSA00004", "PPSA00005", "PPSA00006"})
        EXPECT_EQ(check_path(rules, std::string("/user/app/") + title).verdict, Verdict::title_part)
            << title;
    EXPECT_EQ(check_path(rules, "/data/homebrew/Shared").verdict, Verdict::title_part);
    EXPECT_EQ(check_path(rules, "/user/data/homebrew/Shared/eboot.bin").verdict,
              Verdict::title_part);
    EXPECT_EQ(check_path(rules, "/user/app/PPSA00007").verdict, Verdict::title);
}

TEST(Protection, APlaydenGameOnTwoDrivesIsLeftToPlayden)
{
    const ProtectionRules rules = console_protection(
        console_volume_specs(), {{"steam-2610", "FAKE78024"}}, {}, {"steam-2610"});
    EXPECT_EQ(check_path(rules, "/data/playden-games/2610").verdict, Verdict::title_part);
    EXPECT_EQ(check_path(rules, "/mnt/usb0/playden-games/2610").verdict, Verdict::title_part);
    // Its title alone is still the title.
    EXPECT_EQ(check_path(rules, "/data/homebrew/FAKE78024").verdict, Verdict::title);
}

TEST(Protection, SpellingsOfOneFolder)
{
    const ProtectionRules rules = console_rules();
    EXPECT_EQ(canonical_path(rules, "/user/data/x/"), "/data/x");
    EXPECT_EQ(canonical_path(rules, "/user/database"), "/user/database");
    EXPECT_EQ(path_spellings(rules, "/data/x"),
              (std::vector<std::string>{"/data/x", "/user/data/x"}));
    EXPECT_EQ(path_spellings(rules, "/user/x"), std::vector<std::string>{"/user/x"});
    EXPECT_TRUE(path_within("/a/b", "/a"));
    EXPECT_TRUE(path_within("/a", "/a"));
    EXPECT_FALSE(path_within("/ab", "/a"));
    EXPECT_TRUE(path_within("/a", "/"));
}

CollectedItem item(const char *key, const char *drive, std::uint64_t size)
{
    CollectedItem i;
    i.key = key;
    i.path = key;
    i.drive = drive;
    i.drive_name = drive;
    i.size = size;
    return i;
}

TEST(CollectionModel, NestingAndTotalsByDrive)
{
    Collection c;
    EXPECT_TRUE(c.add(item("/user/home/1/captures/a.mp4", "internal", 5)));
    EXPECT_TRUE(c.add(item("/user/download", "internal", 30)));
    EXPECT_TRUE(c.add(item("/mnt/usb0/backup.tar", "usb0", 50)));
    // A folder takes in the items inside it.
    EXPECT_TRUE(c.add(item("/user/home/1/captures", "internal", 40)));
    EXPECT_EQ(c.size(), 3u);
    EXPECT_EQ(c.find("/user/home/1/captures/a.mp4"), nullptr);
    // Inside a collected folder nothing more can be added.
    EXPECT_FALSE(c.add(item("/user/home/1/captures/b.mp4", "internal", 1)));
    EXPECT_FALSE(c.add(item("/user/download", "internal", 30)));
    EXPECT_NE(c.covering("/user/home/1/captures/b.mp4"), nullptr);
    EXPECT_EQ(c.covering("/user/home/1/capturesX"), nullptr);
    EXPECT_EQ(c.total(), 120u);

    const std::vector<DriveTotal> drives = c.drives();
    ASSERT_EQ(drives.size(), 2u);
    EXPECT_EQ(drives[0].drive, "internal");
    EXPECT_EQ(drives[0].size, 70u);
    EXPECT_EQ(drives[0].count, 2u);
    EXPECT_EQ(drives[1].size, 50u);

    const auto sorted = c.sorted();
    ASSERT_EQ(sorted.size(), 3u);
    EXPECT_EQ(sorted[0]->key, "/user/home/1/captures");
    EXPECT_EQ(sorted[1]->key, "/user/download");
    EXPECT_EQ(sorted[2]->key, "/mnt/usb0/backup.tar");

    EXPECT_TRUE(c.remove("/user/download"));
    EXPECT_FALSE(c.remove("/user/download"));
    c.retain(
        [](CollectedItem &i)
        {
            i.size = 1;
            return i.drive == "usb0";
        });
    ASSERT_EQ(c.size(), 1u);
    EXPECT_EQ(c.total(), 1u);
}

TEST(CollectionModel, OneFolderSeenFromTwoVolumesCountsOnce)
{
    const ProtectionRules rules = console_rules();
    Collection c;
    CollectedItem from_data = item("", "internal", 10);
    from_data.key = canonical_path(rules, "/data/backups");
    from_data.path = "/data/backups";
    CollectedItem from_user = item("", "internal", 10);
    from_user.key = canonical_path(rules, "/user/data/backups");
    from_user.path = "/user/data/backups";
    EXPECT_TRUE(c.add(from_data));
    EXPECT_FALSE(c.add(from_user));
    EXPECT_EQ(c.total(), 10u);
    EXPECT_EQ(c.drives().size(), 1u);
}

// ---- the deleter ----

DeleteJob folder_job(const std::string &path, const std::string &root, std::uint64_t size = 0)
{
    DeleteJob job;
    job.item.key = path;
    job.item.path = path;
    job.item.size = size;
    job.volume_root = root;
    return job;
}

std::vector<ItemResult> run(const FileSystem &fs, DeleteOptions options)
{
    Deleter deleter;
    EXPECT_TRUE(deleter.start(fs, std::move(options)));
    deleter.join();
    EXPECT_TRUE(deleter.progress().finished);
    return deleter.take();
}

using PosixDeleter = TempDir;

TEST_F(PosixDeleter, RemovesBottomUpAndNeverFollowsALink)
{
    folder("v");
    folder("v/item");
    folder("v/item/deep");
    folder("v/item/deep/empty");
    write("v/item/a.bin", 8192);
    write("v/item/deep/b.bin", 8192);
    folder("outside");
    write("outside/keep.bin");
    ASSERT_EQ(symlink(path("outside").c_str(), path("v/item/deep/to_outside").c_str()), 0);
    ASSERT_EQ(symlink(path("outside/keep.bin").c_str(), path("v/item/to_keep").c_str()), 0);

    const auto fs = make_posix_filesystem(true, true);
    DeleteOptions options;
    options.rules.volume_roots = {path("v")};
    options.jobs = {folder_job(path("v/item"), path("v"))};
    const auto results = run(*fs, std::move(options));
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].outcome, Outcome::removed) << results[0].error_path;
    EXPECT_GE(results[0].bytes, 16384u);
    EXPECT_FALSE(exists("v/item"));
    EXPECT_TRUE(exists("outside/keep.bin"));
}

TEST_F(PosixDeleter, KeepsGoingPastWhatItCannotRemove)
{
    if (geteuid() == 0)
        GTEST_SKIP() << "root ignores folder permissions here";
    folder("v");
    folder("v/item");
    folder("v/item/locked");
    write("v/item/locked/stays.bin");
    write("v/item/goes.bin");
    folder("v/item/also");
    write("v/item/also/goes.bin");
    ASSERT_EQ(chmod(path("v/item/locked").c_str(), 0555), 0);

    const auto fs = make_posix_filesystem(true, true);
    DeleteOptions options;
    options.rules.volume_roots = {path("v")};
    options.jobs = {folder_job(path("v/item"), path("v"))};
    const auto results = run(*fs, std::move(options));
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].outcome, Outcome::partly);
    EXPECT_EQ(results[0].error, EACCES);
    EXPECT_EQ(results[0].error_path, path("v/item/locked/stays.bin"));
    EXPECT_FALSE(exists("v/item/goes.bin"));
    EXPECT_FALSE(exists("v/item/also"));
    EXPECT_TRUE(exists("v/item/locked/stays.bin"));
    // What is left, scanned, for the picture.
    ASSERT_NE(results[0].left, nullptr);
    EXPECT_EQ(results[0].left->node(0).files, 1u);
}

TEST_F(PosixDeleter, AFileThatBecameAFolderIsNotEmptied)
{
    folder("v");
    folder("v/thing");
    write("v/thing/keep.bin");
    const auto fs = make_posix_filesystem(true, true);
    DeleteOptions options;
    options.rules.volume_roots = {path("v")};
    DeleteJob job = folder_job(path("v/thing"), path("v"));
    job.item.kind = ItemKind::file; // collected when it was a file
    options.jobs = {job};
    const auto results = run(*fs, std::move(options));
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].outcome, Outcome::failed);
    EXPECT_EQ(results[0].error, ESTALE);
    EXPECT_TRUE(exists("v/thing/keep.bin"));
}

TEST_F(PosixDeleter, AnEmptyFileRemovedCountsAsPartly)
{
    if (geteuid() == 0)
        GTEST_SKIP() << "root ignores folder permissions here";
    folder("v");
    folder("v/item");
    folder("v/item/locked");
    write("v/item/locked/stays.bin");
    write("v/item/empty.txt", 0);
    ASSERT_EQ(chmod(path("v/item/locked").c_str(), 0555), 0);
    const auto fs = make_posix_filesystem(true, true);
    DeleteOptions options;
    options.rules.volume_roots = {path("v")};
    options.jobs = {folder_job(path("v/item"), path("v"))};
    const auto results = run(*fs, std::move(options));
    ASSERT_EQ(results.size(), 1u);
    EXPECT_FALSE(exists("v/item/empty.txt"));
    // Something was removed, so the picture is rescanned: not "failed".
    EXPECT_EQ(results[0].outcome, Outcome::partly);
    EXPECT_NE(results[0].left, nullptr);
}

TEST_F(PosixDeleter, RefusesAPathThroughALink)
{
    folder("v");
    folder("elsewhere");
    folder("elsewhere/item");
    write("elsewhere/item/keep.bin");
    ASSERT_EQ(symlink(path("elsewhere").c_str(), path("v/link").c_str()), 0);

    const auto fs = make_posix_filesystem(true, true);
    DeleteOptions options;
    options.rules.volume_roots = {path("v")};
    // Collected when v/link was a real folder; now it points elsewhere.
    options.jobs = {folder_job(path("v/link/item"), path("v"))};
    const auto results = run(*fs, std::move(options));
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].outcome, Outcome::failed);
    EXPECT_EQ(results[0].error, ENOTDIR);
    EXPECT_TRUE(exists("elsewhere/item/keep.bin"));
}

TEST_F(PosixDeleter, DryRunRemovesNothing)
{
    folder("v");
    folder("v/item");
    write("v/item/a.bin", 8192);
    const auto fs = make_posix_filesystem(true, true);
    DeleteOptions options;
    options.rules.volume_roots = {path("v")};
    options.dry_run = true;
    options.jobs = {folder_job(path("v/item"), path("v"))};
    const auto results = run(*fs, std::move(options));
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].outcome, Outcome::removed);
    EXPECT_GE(results[0].bytes, 8192u);
    EXPECT_TRUE(exists("v/item/a.bin"));
}

ProtectionRules fixture_rules()
{
    ProtectionRules rules;
    const std::string root = kFixtureRoot;
    rules.volume_roots = {kFixtureRoot, kFixtureUsbRoot};
    rules.protected_roots = {{root + "/appmeta", "must stay"}};
    rules.title_parents = {root + "/app"};
    return rules;
}

TEST(FixtureDeleter, FreesSpaceSkipsMountsAndRechecksTheRules)
{
    const auto fs = make_fixture_filesystem();
    const std::string root = kFixtureRoot;
    SpaceInfo before;
    ASSERT_TRUE(fs->space(root, &before));
    DeleteOptions options;
    options.rules = fixture_rules();
    options.jobs = {folder_job(root + "/home", root), folder_job(root + "/mnt_other", root),
                    folder_job(root + "/appmeta", root), folder_job(root + "/app", root)};
    const auto results = run(*fs, std::move(options));
    ASSERT_EQ(results.size(), 4u);
    EXPECT_EQ(results[0].outcome, Outcome::removed);
    Entry gone;
    EXPECT_FALSE(fs->stat(root + "/home", &gone));
    SpaceInfo after;
    ASSERT_TRUE(fs->space(root, &after));
    EXPECT_EQ(before.used - after.used, results[0].bytes);
    // Another device: a mount point is never emptied.
    EXPECT_EQ(results[1].outcome, Outcome::failed);
    EXPECT_EQ(results[1].error, EXDEV);
    // The rules are checked again, whatever the collection said.
    EXPECT_EQ(results[2].outcome, Outcome::failed);
    EXPECT_EQ(results[2].reason, "must stay");
    EXPECT_EQ(results[3].outcome, Outcome::failed);
    EXPECT_TRUE(fs->stat(root + "/appmeta", &gone));
    EXPECT_TRUE(fs->stat(root + "/app", &gone));
}

// Changes what the console would: one folder's mount name can't be read, and
// one folder is listed with an identity it no longer has.
class TrickyFileSystem final : public FileSystem
{
  public:
    explicit TrickyFileSystem(const FileSystem &inner) : inner_(inner)
    {
    }
    bool list(const std::string &p, std::vector<Entry> *out, int *error) const override
    {
        if (!inner_.list(p, out, error))
            return false;
        for (Entry &e : *out)
            if (p + "/" + e.name == swapped)
                e.inode += 100000; // listed as one folder, found as another
        return true;
    }
    bool stat(const std::string &p, Entry *out) const override
    {
        return inner_.stat(p, out);
    }
    bool space(const std::string &p, SpaceInfo *out) const override
    {
        return inner_.space(p, out);
    }
    bool read_file(const std::string &p, std::string *out, std::size_t max) const override
    {
        return inner_.read_file(p, out, max);
    }
    bool mount_point(const std::string &p, std::string *out) const override
    {
        if (p == unnamed)
            return false;
        return inner_.mount_point(p, out);
    }
    bool remove_file(const std::string &p, int *error) const override
    {
        return inner_.remove_file(p, error);
    }
    bool remove_folder(const std::string &p, int *error) const override
    {
        return inner_.remove_folder(p, error);
    }
    std::string swapped;
    std::string unnamed;

  private:
    const FileSystem &inner_;
};

TEST(FixtureDeleter, AFolderChangedAfterListingIsNotEntered)
{
    const auto inner = make_fixture_filesystem();
    TrickyFileSystem fs(*inner);
    const std::string root = kFixtureRoot;
    fs.swapped = root + "/home/10000000/captures";
    DeleteOptions options;
    options.rules = fixture_rules();
    options.jobs = {folder_job(root + "/home", root)};
    const auto results = run(fs, std::move(options));
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].outcome, Outcome::partly);
    EXPECT_EQ(results[0].error, ESTALE);
    Entry e;
    EXPECT_TRUE(fs.stat(root + "/home/10000000/captures/clip_000.mp4", &e));
    EXPECT_FALSE(fs.stat(root + "/home/10000000/trophy", &e));
}

TEST(FixtureDeleter, AFolderWhoseMountCantBeReadIsLeftAlone)
{
    const auto inner = make_fixture_filesystem();
    TrickyFileSystem fs(*inner);
    const std::string root = kFixtureRoot;
    fs.unnamed = root + "/home/10000000/trophy";
    DeleteOptions options;
    options.rules = fixture_rules();
    options.jobs = {folder_job(root + "/home", root)};
    const auto results = run(fs, std::move(options));
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].outcome, Outcome::partly);
    EXPECT_EQ(results[0].error, EXDEV);
    Entry e;
    EXPECT_TRUE(fs.stat(root + "/home/10000000/trophy/t0000.dat", &e));
    EXPECT_FALSE(fs.stat(root + "/home/10000000/captures", &e));
}

// The console's case: two file systems that both report st_dev 0. Only the
// mount names tell that one is mounted inside the other.
class SameDeviceFileSystem final : public FileSystem
{
  public:
    explicit SameDeviceFileSystem(const FileSystem &inner) : inner_(inner)
    {
    }
    bool list(const std::string &p, std::vector<Entry> *out, int *error) const override
    {
        if (!inner_.list(p, out, error))
            return false;
        for (Entry &e : *out)
            e.device = 0;
        return true;
    }
    bool stat(const std::string &p, Entry *out) const override
    {
        if (!inner_.stat(p, out))
            return false;
        out->device = 0;
        return true;
    }
    bool space(const std::string &p, SpaceInfo *out) const override
    {
        return inner_.space(p, out);
    }
    bool read_file(const std::string &p, std::string *out, std::size_t max) const override
    {
        return inner_.read_file(p, out, max);
    }
    bool mount_point(const std::string &p, std::string *out) const override
    {
        return inner_.mount_point(p, out);
    }
    bool remove_file(const std::string &p, int *error) const override
    {
        return inner_.remove_file(p, error);
    }
    bool remove_folder(const std::string &p, int *error) const override
    {
        return inner_.remove_folder(p, error);
    }

  private:
    const FileSystem &inner_;
};

TEST(FixtureDeleter, MountNamesTellMountsApartWhenDevicesDoNot)
{
    const auto inner = make_fixture_filesystem();
    SameDeviceFileSystem fs(*inner);
    const std::string root = kFixtureRoot;
    DeleteOptions options;
    options.rules = fixture_rules();
    options.jobs = {folder_job(root + "/mnt_other", root)};
    const auto results = run(fs, std::move(options));
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].outcome, Outcome::failed);
    EXPECT_EQ(results[0].error, EXDEV);
    Entry still;
    EXPECT_TRUE(fs.stat(root + "/mnt_other/huge/elsewhere.bin", &still));
}

TEST(FixtureDeleter, SelectionBelowAMountOnTheSameDeviceIsRefused)
{
    const auto inner = make_fixture_filesystem();
    SameDeviceFileSystem fs(*inner);
    const std::string root = kFixtureRoot;
    DeleteOptions options;
    options.rules = fixture_rules();
    options.jobs = {folder_job(root + "/mnt_other/huge", root)};
    const auto results = run(fs, std::move(options));
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].outcome, Outcome::failed);
    EXPECT_EQ(results[0].error, EXDEV);
    Entry still;
    EXPECT_TRUE(fs.stat(root + "/mnt_other/huge/elsewhere.bin", &still));
}

// Refuses removal with EACCES until the euid is "raised", like a root-owned
// folder on the console.
class OwnedFileSystem final : public FileSystem
{
  public:
    explicit OwnedFileSystem(const FileSystem &inner) : inner_(inner)
    {
    }
    bool list(const std::string &p, std::vector<Entry> *out, int *error) const override
    {
        return inner_.list(p, out, error);
    }
    bool stat(const std::string &p, Entry *out) const override
    {
        return inner_.stat(p, out);
    }
    bool space(const std::string &p, SpaceInfo *out) const override
    {
        return inner_.space(p, out);
    }
    bool read_file(const std::string &p, std::string *out, std::size_t max) const override
    {
        return inner_.read_file(p, out, max);
    }
    bool remove_file(const std::string &p, int *error) const override
    {
        if (!raised)
        {
            *error = EACCES;
            return false;
        }
        return inner_.remove_file(p, error);
    }
    bool remove_folder(const std::string &p, int *error) const override
    {
        if (!raised)
        {
            *error = EACCES;
            return false;
        }
        return inner_.remove_folder(p, error);
    }
    mutable bool raised = false;
    mutable int raises = 0;

  private:
    const FileSystem &inner_;
};

TEST(FixtureDeleter, RetriesWithTheEuidRaised)
{
    const auto inner = make_fixture_filesystem();
    OwnedFileSystem fs(*inner);
    const std::string root = kFixtureRoot;
    DeleteOptions options;
    options.rules = fixture_rules();
    options.jobs = {folder_job(root + "/download", root)};
    options.privileged = [&fs](const std::function<bool()> &call)
    {
        ++fs.raises;
        fs.raised = true;
        const bool ok = call();
        fs.raised = false;
        return ok;
    };
    const auto results = run(fs, std::move(options));
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].outcome, Outcome::removed);
    EXPECT_GT(fs.raises, 0);
    EXPECT_FALSE(fs.raised);
    Entry gone;
    EXPECT_FALSE(fs.stat(root + "/download", &gone));

    // Without a way to raise it, nothing goes.
    const auto again = make_fixture_filesystem();
    OwnedFileSystem locked(*again);
    DeleteOptions plain;
    plain.rules = fixture_rules();
    plain.jobs = {folder_job(root + "/download", root)};
    const auto refused = run(locked, std::move(plain));
    EXPECT_EQ(refused[0].outcome, Outcome::failed);
    EXPECT_EQ(refused[0].error, EACCES);
}

class FakeRemover final : public TitleRemover
{
  public:
    UninstallResult uninstall(const std::string &title_id) override
    {
        asked.push_back(title_id);
        if (on_uninstall)
            on_uninstall();
        UninstallResult r;
        r.ok = answer;
        r.reason = answer ? "" : "The console refused (0x80990001).";
        return r;
    }
    bool answer = true;
    std::vector<std::string> asked;
    std::function<void()> on_uninstall;
};

DeleteJob title_job(const ProtectionRules &rules, const std::string &path, const std::string &root)
{
    const Check check = check_path(rules, path);
    DeleteJob job = folder_job(path, root, 1000);
    job.item.kind = ItemKind::title;
    job.item.title_id = check.title_id;
    job.item.then_delete = check.then_delete;
    return job;
}

TEST(FixtureDeleter, TitlesGoThroughTheSystemThenTheirFolders)
{
    const auto fs = make_fixture_filesystem();
    const std::string root = kFixtureRoot;
    ProtectionRules rules = fixture_rules();
    // A Playden-like layout on the fixture: game data under home stands in.
    rules.homebrew = root + "/home";
    rules.playden_game_folders = {root + "/home/10000000"};
    rules.playden_titles = {{"steam-1", "FAKE78001"}};
    FakeRemover remover;

    DeleteOptions options;
    options.rules = rules;
    options.remover = &remover;
    options.jobs = {title_job(rules, root + "/app/PPSA10021", root)};
    auto results = run(*fs, options);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].outcome, Outcome::removed);
    EXPECT_EQ(remover.asked, std::vector<std::string>{"PPSA10021"});
    // The game's folder is the system's to remove: never walked by hand.
    Entry still;
    EXPECT_TRUE(fs->stat(root + "/app/PPSA10021/app.pkg", &still));

    // A refusal leaves everything in place.
    remover.answer = false;
    remover.asked.clear();
    options.jobs = {title_job(rules, root + "/app/PPSA10388", root)};
    results = run(*fs, options);
    EXPECT_EQ(results[0].outcome, Outcome::failed);
    EXPECT_EQ(results[0].reason, "The console refused (0x80990001).");
}

TEST(FixtureDeleter, StopsAfterTheItemAtWork)
{
    const auto fs = make_fixture_filesystem();
    const std::string root = kFixtureRoot;
    const ProtectionRules rules = fixture_rules();
    FakeRemover remover;
    Deleter deleter;
    remover.on_uninstall = [&deleter] { deleter.stop(); };
    DeleteOptions options;
    options.rules = rules;
    options.remover = &remover;
    options.jobs = {title_job(rules, root + "/app/PPSA10021", root),
                    folder_job(root + "/download", root)};
    ASSERT_TRUE(deleter.start(*fs, std::move(options)));
    deleter.join();
    const auto results = deleter.take();
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].outcome, Outcome::removed);
    EXPECT_EQ(results[1].outcome, Outcome::skipped);
    Entry still;
    EXPECT_TRUE(fs->stat(root + "/download", &still));
}

// Playden's order on the fixture: a stand-in homebrew folder, a trash folder.
TEST(FixtureDeleter, PlaydenTitleFolderIsMovedAsideFirst)
{
    const auto fs = make_fixture_filesystem();
    const std::string root = kFixtureRoot;
    ProtectionRules rules = fixture_rules();
    rules.homebrew = root + "/home/10000000/savedata";
    FakeRemover remover;
    Entry seen;
    // While the system uninstalls, the folder is no longer where ShadowMount looks.
    remover.on_uninstall = [&]
    { EXPECT_FALSE(fs->stat(root + "/home/10000000/savedata/FAKE78001", &seen)); };
    int error = 0;
    ASSERT_TRUE(fs->move(root + "/home/10000000/savedata/PPSA10021",
                         root + "/home/10000000/savedata/FAKE78001", &error));
    DeleteOptions options;
    options.rules = rules;
    options.remover = &remover;
    options.trash_dir = root + "/download";
    options.jobs = {title_job(rules, root + "/home/10000000/savedata/FAKE78001", root)};
    ASSERT_EQ(options.jobs[0].item.title_id, "FAKE78001");
    auto results = run(*fs, options);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].outcome, Outcome::removed) << results[0].error_path;
    EXPECT_EQ(remover.asked, std::vector<std::string>{"FAKE78001"});
    EXPECT_FALSE(fs->stat(root + "/home/10000000/savedata/FAKE78001", &seen));
    std::vector<Entry> trash;
    ASSERT_TRUE(fs->list(root + "/download", &trash, &error));
    for (const Entry &e : trash)
        EXPECT_NE(e.name.rfind("FAKE78001", 0), 0u) << "left in the trash";

    // Refused: the folder goes back where it was.
    ASSERT_TRUE(fs->move(root + "/home/10000000/savedata/PPSA10388",
                         root + "/home/10000000/savedata/FAKE78002", &error));
    remover.answer = false;
    remover.on_uninstall = nullptr;
    options.jobs = {title_job(rules, root + "/home/10000000/savedata/FAKE78002", root)};
    results = run(*fs, options);
    EXPECT_EQ(results[0].outcome, Outcome::failed);
    EXPECT_TRUE(fs->stat(root + "/home/10000000/savedata/FAKE78002/sdimg_save", &seen));
}

TEST(FixtureDeleter, AShadowMountSourceIsMovedAsideOnItsOwnDrive)
{
    const auto fs = make_fixture_filesystem();
    const std::string root = kFixtureRoot;
    const std::string usb = kFixtureUsbRoot;
    int error = 0;
    ASSERT_TRUE(fs->make_folder(usb + "/homebrew", &error));
    ASSERT_TRUE(fs->make_folder(usb + "/homebrew/GAME", &error));
    ProtectionRules rules = fixture_rules();
    rules.shadowmount_sources = {{"PPSA10021", usb + "/homebrew/GAME"}};
    FakeRemover remover;
    Entry seen;
    remover.on_uninstall = [&]
    {
        // Out of ShadowMount's way, in a trash made on the source's drive.
        EXPECT_FALSE(fs->stat(usb + "/homebrew/GAME", &seen));
        EXPECT_TRUE(fs->stat(usb + "/prospero-disk/trash", &seen));
    };
    DeleteOptions options;
    options.rules = rules;
    options.remover = &remover;
    options.trash_dir = root + "/download";
    options.jobs = {title_job(rules, root + "/app/PPSA10021", root)};
    ASSERT_EQ(options.jobs[0].item.then_delete, std::vector<std::string>{usb + "/homebrew/GAME"});
    const auto results = run(*fs, options);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].outcome, Outcome::removed) << results[0].error_path;
    EXPECT_EQ(remover.asked, std::vector<std::string>{"PPSA10021"});
    std::vector<Entry> trash;
    ASSERT_TRUE(fs->list(usb + "/prospero-disk/trash", &trash, &error));
    EXPECT_TRUE(trash.empty());
}

TEST(FixtureDeleter, ASourceThatChangedSinceStartupIsLeftAlone)
{
    const auto fs = make_fixture_filesystem();
    const std::string root = kFixtureRoot;
    const std::string usb = kFixtureUsbRoot;
    int error = 0;
    ASSERT_TRUE(fs->make_folder(usb + "/homebrew", &error));
    ASSERT_TRUE(fs->make_folder(usb + "/homebrew/GAME", &error));
    ProtectionRules rules = fixture_rules();
    rules.shadowmount_sources = {{"PPSA10021", usb + "/homebrew/GAME"}};
    FakeRemover remover;
    DeleteOptions options;
    options.rules = rules;
    options.remover = &remover;
    options.trash_dir = root + "/download";
    // The title now names another folder (GAME was reused).
    options.read_shadowmount_sources = [&]
    { return std::map<std::string, std::string>{{"PPSA10021", usb + "/homebrew/OTHER"}}; };
    options.jobs = {title_job(rules, root + "/app/PPSA10021", root)};
    auto results = run(*fs, options);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].outcome, Outcome::failed);
    EXPECT_TRUE(remover.asked.empty());
    Entry seen;
    EXPECT_TRUE(fs->stat(usb + "/homebrew/GAME", &seen));
    // Unreadable now: the same.
    options.read_shadowmount_sources = [&]
    { return std::map<std::string, std::string>{{"PPSA10021", ""}}; };
    results = run(*fs, options);
    EXPECT_EQ(results[0].outcome, Outcome::failed);
    EXPECT_TRUE(remover.asked.empty());
}

TEST_F(PosixDeleter, ALinkedTrashFolderIsNotUsed)
{
    for (const char *f : {"a", "a/app", "a/app/PPSA00001", "a/homebrew", "a/homebrew/GAME", "b",
                          "b/trash", "elsewhere"})
        folder(f);
    write("a/homebrew/GAME/eboot.bin");
    // <drive>/prospero-disk is a link to another folder.
    ASSERT_EQ(symlink(path("elsewhere").c_str(), path("a/prospero-disk").c_str()), 0);
    ProtectionRules rules;
    rules.volume_roots = {path("a"), path("b")};
    rules.title_parents = {path("a/app")};
    rules.shadowmount_sources = {{"PPSA00001", path("a/homebrew/GAME")}};
    const auto fs = make_posix_filesystem(true, true);
    FakeRemover remover;
    DeleteOptions options;
    options.rules = rules;
    options.remover = &remover;
    options.trash_dir = path("b/trash");
    options.jobs = {title_job(rules, path("a/app/PPSA00001"), path("a"))};
    const auto results = run(*fs, options);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].outcome, Outcome::failed);
    EXPECT_TRUE(remover.asked.empty());
    EXPECT_TRUE(exists("a/homebrew/GAME/eboot.bin"));
    EXPECT_FALSE(exists("elsewhere/trash"));
}

TEST(FixtureDeleter, PlaydenIsToldWhenAGamesFilesAreDeleted)
{
    const auto fs = make_fixture_filesystem();
    const std::string root = kFixtureRoot;
    int error = 0;
    for (const char *folder : {"/hb", "/hb/FAKE78024", "/playden-games", "/playden-games/2610"})
        ASSERT_TRUE(fs->make_folder(root + folder, &error)) << folder;
    ProtectionRules rules = fixture_rules();
    rules.homebrew = root + "/hb";
    rules.playden_game_folders = {root + "/playden-games"};
    rules.playden_titles = {{"steam-2610", "FAKE78024"}};
    FakeRemover remover;
    std::vector<std::string> told;
    bool answer = true;
    DeleteOptions options;
    options.rules = rules;
    options.remover = &remover;
    options.forget_playden_game = [&](const std::string &game)
    {
        told.push_back(game);
        return answer;
    };
    options.jobs = {title_job(rules, root + "/playden-games/2610", root)};
    auto results = run(*fs, options);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].outcome, Outcome::removed) << results[0].error_path;
    EXPECT_EQ(told, std::vector<std::string>{"2610"});
    EXPECT_TRUE(results[0].reason.empty());

    // A game folder with no title left: deleted as files, Playden still told.
    ASSERT_TRUE(fs->make_folder(root + "/playden-games/777", &error));
    told.clear();
    options.jobs = {folder_job(root + "/playden-games/777", root)};
    results = run(*fs, options);
    EXPECT_EQ(results[0].outcome, Outcome::removed) << results[0].error_path;
    EXPECT_EQ(told, std::vector<std::string>{"777"});

    // Not written: the result says to finish in Playden.
    for (const char *folder : {"/hb/FAKE78024", "/playden-games/2610"})
        ASSERT_TRUE(fs->make_folder(root + folder, &error)) << folder;
    answer = false;
    options.jobs = {title_job(rules, root + "/playden-games/2610", root)};
    results = run(*fs, options);
    EXPECT_EQ(results[0].outcome, Outcome::removed);
    EXPECT_NE(results[0].reason.find("Playden"), std::string::npos);
}

using ConsoleFiles = TempDir;

TEST_F(ConsoleFiles, ShadowMountSourcesComeFromMountLinks)
{
    folder("app");
    const auto link = [&](const char *title, const std::string &text)
    {
        folder(std::string("app/") + title);
        std::FILE *f = std::fopen(path(std::string("app/") + title + "/mount.lnk").c_str(), "wb");
        ASSERT_NE(f, nullptr);
        std::fwrite(text.data(), 1, text.size(), f);
        std::fclose(f);
    };
    // Source folders, each with the param.json of the title it holds.
    const auto source = [&](const std::string &name, const char *title)
    {
        folder("homebrew/" + name);
        folder("homebrew/" + name + "/sce_sys");
        std::FILE *f = std::fopen(path("homebrew/" + name + "/sce_sys/param.json").c_str(), "wb");
        ASSERT_NE(f, nullptr);
        std::fprintf(f, "{\"titleId\": \"%s\"}", title);
        std::fclose(f);
    };
    folder("homebrew");
    source("CONTROL Resonant ", "PPSA34547"); // a trailing space is part of the name
    source("FAKE78079", "FAKE78079");
    source("Someone Else", "PPSA00009");
    link("PPSA34547", path("homebrew/CONTROL Resonant ") + "\n");
    link("FAKE78079", path("homebrew/FAKE78079"));
    link("PPSA00002", path("homebrew/Someone Else")); // another title's folder
    link("PPSA00003", path("homebrew/../app"));       // not a plain path
    link("PPSA00004", path("homebrew/A") + "\nB");    // two lines
    link("PPSA00005", path("homebrew/Missing"));      // no such folder
    link("NOTANID", path("homebrew/NOTANID"));
    folder("app/CUSA01867"); // a package install: no mount.lnk
    const auto fs = make_posix_filesystem();
    const auto sources = read_shadowmount_sources(*fs, Privileged{}, {path("app"), path("none")});
    EXPECT_EQ(sources, (std::map<std::string, std::string>{
                           {"FAKE78079", path("homebrew/FAKE78079")},
                           {"PPSA00002", ""},
                           {"PPSA00003", ""},
                           {"PPSA00004", ""},
                           {"PPSA00005", ""},
                           {"PPSA34547", path("homebrew/CONTROL Resonant ")}}));

    // One title linked to two folders from two app folders: neither.
    folder("app2");
    folder("app2/FAKE78079");
    std::FILE *f = std::fopen(path("app2/FAKE78079/mount.lnk").c_str(), "wb");
    ASSERT_NE(f, nullptr);
    std::fputs(path("homebrew/CONTROL Resonant ").c_str(), f);
    std::fclose(f);
    EXPECT_EQ(
        read_shadowmount_sources(*fs, Privileged{}, {path("app"), path("app2")}).at("FAKE78079"),
        "");
}

TEST_F(ConsoleFiles, PlaydenCopiesOnTwoDrives)
{
    for (const char *f : {"a", "a/playden-games", "a/playden-games/2610", "a/playden-games/70", "b",
                          "b/playden-games", "b/playden-games/2610"})
        folder(f);
    const auto fs = make_posix_filesystem();
    EXPECT_EQ(
        read_playden_copies(*fs, Privileged{}, {path("a/playden-games"), path("b/playden-games")}),
        std::vector<std::string>{"steam-2610"});
}

TEST_F(ConsoleFiles, PlaydenRequestsGoOnlyToTheRealFile)
{
    folder("FAKE77031");
    folder("elsewhere");
    // A linked dev folder would send the line elsewhere.
    ASSERT_EQ(symlink(path("elsewhere").c_str(), path("FAKE77031/dev").c_str()), 0);
    EXPECT_FALSE(append_playden_request(path("FAKE77031"), "2610", Privileged{}));
    EXPECT_FALSE(exists("elsewhere/install-request.txt"));
    // A request file with a second name would change that file too.
    ASSERT_EQ(unlink(path("FAKE77031/dev").c_str()), 0);
    folder("FAKE77031/dev");
    write("elsewhere/other.txt", 0);
    ASSERT_EQ(link(path("elsewhere/other.txt").c_str(),
                   path("FAKE77031/dev/install-request.txt").c_str()),
              0);
    EXPECT_FALSE(append_playden_request(path("FAKE77031"), "2610", Privileged{}));
    struct stat st
    {
    };
    ASSERT_EQ(stat(path("elsewhere/other.txt").c_str(), &st), 0);
    EXPECT_EQ(st.st_size, 0);
}

TEST_F(ConsoleFiles, PlaydenRequestsAreAppendedLineByLine)
{
    folder("FAKE77031");
    EXPECT_TRUE(append_playden_request(path("FAKE77031"), "2610", Privileged{}));
    EXPECT_TRUE(append_playden_request(path("FAKE77031"), "gog-1207659723", Privileged{}));
    EXPECT_FALSE(append_playden_request(path("FAKE77031"), "2610\ncancel 1", Privileged{}));
    EXPECT_FALSE(append_playden_request(path("FAKE77031"), "steam-2610", Privileged{}));
    std::string text;
    if (std::FILE *f = std::fopen(path("FAKE77031/dev/install-request.txt").c_str(), "rb"))
    {
        char buffer[256];
        text.assign(buffer, std::fread(buffer, 1, sizeof buffer, f));
        std::fclose(f);
    }
    EXPECT_EQ(text, "uninstall 2610\nuninstall gog-1207659723\n");
}

// ---- uninstalling through the system ----

using PayloadClient = TempDir;

// Reads a whole file of the temporary folder ("" when missing).
std::string slurp(const std::string &path)
{
    std::string out;
    if (std::FILE *f = std::fopen(path.c_str(), "rb"))
    {
        char buffer[512];
        std::size_t got = 0;
        while ((got = std::fread(buffer, 1, sizeof buffer, f)) > 0)
            out.append(buffer, got);
        std::fclose(f);
    }
    return out;
}

TitleRemoverConfig client_config(const std::string &root)
{
    TitleRemoverConfig config;
    config.request_dir = root + "/title-remover";
    config.payload = root + "/title-remover.elf";
    config.sleep = [](int) {};
    config.answer_polls = 3;
    config.confirm_polls = 3;
    return config;
}

TEST_F(PayloadClient, SendsTheRequestAndReadsTheAnswer)
{
    write("title-remover.elf");
    {
        std::FILE *f = std::fopen(path("title-remover.elf").c_str(), "wb");
        std::fputs("\x7f"
                   "ELF payload",
                   f);
        std::fclose(f);
    }
    TitleRemoverConfig config = client_config(root_);
    std::string request;
    unsigned answer = 0;
    // Stands in for the payload: answers the request with its nonce.
    config.send = [&](const std::string &elf, std::string *)
    {
        EXPECT_EQ(elf.compare(0, 4,
                              "\x7f"
                              "ELF"),
                  0);
        request = slurp(path("title-remover/request"));
        const std::size_t at = request.find("nonce=");
        const std::string nonce = request.substr(at + 6, request.find('\n', at) - at - 6);
        std::FILE *f = std::fopen(path("title-remover/result").c_str(), "wb");
        std::fprintf(f, "title=PPSA01467\nnonce=%s\ncode=%08x\n", nonce.c_str(), answer);
        std::fclose(f);
        return true;
    };
    int looks = 0;
    config.presence = [&](const std::string &id)
    {
        EXPECT_EQ(id, "PPSA01467");
        // Listed before; then unknown once (never taken for gone); then gone.
        ++looks;
        return looks <= 1   ? TitlePresence::listed
               : looks == 2 ? TitlePresence::unknown
                            : TitlePresence::gone;
    };
    PayloadTitleRemover remover(config);
    UninstallResult r = remover.uninstall("PPSA01467");
    EXPECT_TRUE(r.ok) << r.reason;
    EXPECT_EQ(looks, 3);
    EXPECT_EQ(request.rfind("title=PPSA01467\nnonce=", 0), 0u);
    EXPECT_FALSE(exists("title-remover/request")); // cleaned up
    struct stat info
    {
    };
    ASSERT_EQ(lstat(path("title-remover").c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 07777, 0777u);

    // A refusal carries the console's code.
    answer = 0x80990001;
    looks = 0;
    r = remover.uninstall("PPSA01467");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.reason.find("0x80990001"), std::string::npos);

    // Accepted but never confirmed.
    answer = 0;
    config.presence = [](const std::string &) { return TitlePresence::listed; };
    PayloadTitleRemover stubborn(config);
    r = stubborn.uninstall("PPSA01467");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.reason.find("not confirmed"), std::string::npos);
}

TEST_F(PayloadClient, NeverSendsForATitleItCannotSee)
{
    {
        std::FILE *f = std::fopen(path("title-remover.elf").c_str(), "wb");
        std::fputs("\x7f"
                   "ELF",
                   f);
        std::fclose(f);
    }
    TitleRemoverConfig config = client_config(root_);
    bool sent = false;
    config.send = [&](const std::string &, std::string *)
    {
        sent = true;
        return true;
    };
    // A failed check is not "gone", and not "listed" either.
    config.presence = [](const std::string &) { return TitlePresence::unknown; };
    EXPECT_FALSE(PayloadTitleRemover(config).uninstall("PPSA01467").ok);
    config.presence = [](const std::string &) { return TitlePresence::gone; };
    EXPECT_FALSE(PayloadTitleRemover(config).uninstall("PPSA01467").ok);
    EXPECT_FALSE(sent);
}

TEST_F(PayloadClient, FailsCleanlyWithoutPayloadOrAnswer)
{
    TitleRemoverConfig config = client_config(root_);
    config.presence = [](const std::string &) { return TitlePresence::listed; };
    bool sent = false;
    config.send = [&](const std::string &, std::string *)
    {
        sent = true;
        return true;
    };
    PayloadTitleRemover missing(config);
    UninstallResult r = missing.uninstall("PPSA01467");
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(sent);
    EXPECT_NE(r.reason.find("missing"), std::string::npos);

    {
        std::FILE *f = std::fopen(path("title-remover.elf").c_str(), "wb");
        std::fputs("\x7f"
                   "ELF",
                   f);
        std::fclose(f);
    }
    PayloadTitleRemover silent(config); // sent, but nobody writes a result
    r = silent.uninstall("PPSA01467");
    EXPECT_TRUE(sent);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.reason.find("did not answer"), std::string::npos);

    // A stale answer with another nonce is not taken.
    {
        std::FILE *f = std::fopen(path("title-remover/result").c_str(), "wb");
        std::fputs("nonce=0000000000000000-0000\ncode=00000000\n", f);
        std::fclose(f);
    }
    config.send = [&](const std::string &, std::string *) { return true; };
    PayloadTitleRemover stale(config);
    r = stale.uninstall("PPSA01467");
    EXPECT_FALSE(r.ok);
}

TEST(FixtureTitles, UninstallRemovesTheGameItsPatchAndItsMeta)
{
    const auto fs = make_fixture_filesystem();
    const std::string root = kFixtureRoot;
    const auto remover = make_fixture_title_remover(*fs, root);
    EXPECT_TRUE(remover->uninstall("PPSA10021").ok);
    Entry gone;
    EXPECT_FALSE(fs->stat(root + "/app/PPSA10021", &gone));
    EXPECT_FALSE(fs->stat(root + "/patch/PPSA10021", &gone));
    EXPECT_FALSE(fs->stat(root + "/appmeta/PPSA10021", &gone));
    EXPECT_TRUE(fs->stat(root + "/app/PPSA10388", &gone));
    EXPECT_FALSE(remover->uninstall("PPSA99999").ok);
}

} // namespace
