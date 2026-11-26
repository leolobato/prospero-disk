// Prospero Disk - The collection: what the user marked for deletion, and what may never be marked.
// Copyright (C) 2026 Leonardo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace hui::disk
{

// A folder that must stay, with the reason the toast gives. A path ending in
// '*' matches any name that starts with it ("/system*": /system, /system_ex).
struct ProtectedRoot
{
    std::string path;
    std::string reason;
};

// Two spellings of one folder: /user/data is what /data shows (a nullfs view).
struct PathAlias
{
    std::string from; // "/user/data"
    std::string to;   // "/data": the canonical spelling
};

// What may be collected, as data: console paths on a PS5, the fixture's on a
// PC (docs/plans/collect-delete.md, sections 1 and 3).
struct ProtectionRules
{
    std::vector<std::string> volume_roots;
    std::vector<ProtectedRoot> protected_roots;
    // Folders whose children are installed titles (/user/app): a child named
    // PPSA/CUSA + five digits is uninstalled through the system, any other
    // child is refused, and the folder itself (and what contains it) too.
    std::vector<std::string> title_parents;
    // Folders whose children belong to a title (/user/patch, /user/addcont):
    // refused, the game's uninstall removes them.
    std::vector<std::string> title_companions;
    std::vector<PathAlias> aliases;
    // Playden: its titles' folders (/data/homebrew/FAKE78xxx), its game
    // folders (<volume>/playden/games) and their install ids mapped to titles.
    std::string homebrew; // "/data/homebrew"
    std::vector<std::string> playden_game_folders;
    std::map<std::string, std::string> playden_titles; // steam-2610 -> FAKE78024
};

enum class Verdict : std::uint8_t
{
    folder,             // collectable: deleted file by file
    title,              // collectable: uninstalled through the system
    volume_root,        // refused
    protected_path,     // refused: it is, or is inside, a protected root
    contains_protected, // refused: a protected root is inside it
    contains_titles,    // refused: installed titles are inside it
    title_part,         // refused: a patch or add-on, or an unknown title
};

struct Check
{
    Verdict verdict = Verdict::folder;
    std::string reason;   // for a refusal: the toast's text
    std::string title_id; // Verdict::title: what to uninstall
    // Verdict::title: folders deleted file by file once the system confirms
    // the uninstall (a Playden title's folder, its game data). Empty for a
    // retail game: the system removes its folders itself.
    std::vector<std::string> then_delete;
};

// The spelling every comparison uses: aliases resolved, no trailing '/'.
std::string canonical_path(const ProtectionRules &rules, std::string_view path);
// Every spelling of a canonical path, the canonical one first: how each
// volume's tree may show it.
std::vector<std::string> path_spellings(const ProtectionRules &rules, std::string_view canonical);
// True when path is inside folder or is folder ("/a/b" is inside "/a", "/ab" is not).
bool path_within(std::string_view path, std::string_view folder);

// What collecting a path (any spelling) would do.
Check check_path(const ProtectionRules &rules, std::string_view path);

enum class ItemKind : std::uint8_t
{
    folder,
    file,
    title,
};

struct CollectedItem
{
    std::string key;        // the canonical path
    std::string path;       // as the volume that collected it shows it
    std::string drive;      // groups and totals: Internal and Homebrew data are one drive
    std::string drive_name; // "Internal"
    std::uint64_t size = 0; // on disk, as the scan measured it
    ItemKind kind = ItemKind::folder;
    std::string title_id;                 // ItemKind::title
    std::vector<std::string> then_delete; // ItemKind::title: see Check
};

struct DriveTotal
{
    std::string drive;
    std::string name;
    std::uint64_t size = 0;
    std::size_t count = 0;
};

// The items the user collected this session (never saved: a list of paths to
// delete, waiting from last week, is a trap).
//
//   Collection c;
//   if (c.covering(key) == nullptr) c.add(item);   // drops the items inside it
//   for (const CollectedItem *i : c.sorted()) ...  // the drawer
class Collection
{
  public:
    // Adds an item and drops the collected items inside it. False (and no
    // change) when the item or a folder above it is already collected.
    bool add(CollectedItem item);
    bool remove(std::string_view key);
    void clear()
    {
        items_.clear();
    }
    bool empty() const
    {
        return items_.empty();
    }
    std::size_t size() const
    {
        return items_.size();
    }
    // The collected item that is key or holds it; null when none does.
    const CollectedItem *covering(std::string_view key) const;
    const CollectedItem *find(std::string_view key) const;
    std::uint64_t total() const;
    // Largest drive first.
    std::vector<DriveTotal> drives() const;
    // By drive (largest first), then largest item first: the drawer's order.
    std::vector<const CollectedItem *> sorted() const;
    // After a rescan or a deletion: keep(item) may refresh the item and says
    // whether it stays.
    void retain(const std::function<bool(CollectedItem &)> &keep);

  private:
    std::vector<CollectedItem> items_;
};

} // namespace hui::disk
