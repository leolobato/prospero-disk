// Prospero Disk - The collection: what the user marked for deletion, and what may never be marked.
// Copyright (C) 2026 Leonardo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later

#include "disk/collection.hpp"

#include "disk/titles.hpp"

#include <algorithm>

namespace hui::disk
{

namespace
{

std::string_view trimmed(std::string_view path)
{
    while (path.size() > 1 && path.back() == '/')
        path.remove_suffix(1);
    return path;
}

std::string_view parent_of(std::string_view path)
{
    const std::size_t slash = path.find_last_of('/');
    if (slash == std::string_view::npos)
        return {};
    return slash == 0 ? path.substr(0, 1) : path.substr(0, slash);
}

std::string_view name_of(std::string_view path)
{
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

// The first name below folder on the way to path ("" when path is folder).
std::string_view child_below(std::string_view path, std::string_view folder)
{
    std::string_view rest = path.substr(folder.size());
    while (!rest.empty() && rest.front() == '/')
        rest.remove_prefix(1);
    return rest.substr(0, rest.find('/'));
}

// A protected root matches path: the same folder or one inside it. With a
// trailing '*', any name in the root's folder that starts with its prefix.
bool matches(std::string_view path, std::string_view root)
{
    if (!root.empty() && root.back() == '*')
    {
        const std::string_view prefix = root.substr(0, root.size() - 1);
        const std::string_view folder = parent_of(prefix);
        if (!path_within(path, folder) || path == folder)
            return false;
        return child_below(path, folder).starts_with(name_of(prefix));
    }
    return path_within(path, root);
}

// The root is inside path, so collecting path would take it too.
bool contains(std::string_view path, std::string_view root)
{
    if (!root.empty() && root.back() == '*')
        return path_within(parent_of(root.substr(0, root.size() - 1)), path);
    return path_within(root, path) && root != path;
}

// A PS5 or a PS4 game.
bool retail_title(std::string_view name)
{
    return is_title_id(name) && (name.starts_with("PPSA") || name.starts_with("CUSA"));
}

// FAKE78 and three digits: one of Playden's game titles.
bool playden_title(std::string_view name)
{
    if (name.size() != 9 || !name.starts_with("FAKE78"))
        return false;
    return std::all_of(name.begin() + 6, name.end(), [](char c) { return c >= '0' && c <= '9'; });
}

constexpr const char *kUnclearSource =
    "ShadowMountPlus's record of this game doesn't add up: uninstall it there.";

Check refuse(Verdict verdict, std::string reason)
{
    Check c;
    c.verdict = verdict;
    c.reason = std::move(reason);
    return c;
}

Check uninstall(std::string_view id, std::vector<std::string> then_delete)
{
    Check c;
    c.verdict = Verdict::title;
    c.title_id = std::string(id);
    c.then_delete = std::move(then_delete);
    return c;
}

// The protected root a ShadowMount source sits in: Prospero Disk's own, or
// Playden's. Null when none does.
const ProtectedRoot *protecting(const ProtectionRules &rules, std::string_view path)
{
    for (const ProtectedRoot &root : rules.protected_roots)
        if (matches(path, root.path) || contains(path, root.path))
            return &root;
    return nullptr;
}

// Playden's uninstall request names a Steam game by its app ID, others by
// their install id.
std::string playden_request_game(std::string_view install_id)
{
    if (install_id.starts_with("steam-"))
        return std::string(install_id.substr(6));
    return std::string(install_id);
}

} // namespace

bool path_within(std::string_view path, std::string_view folder)
{
    if (folder.empty() || path.size() < folder.size() || path.substr(0, folder.size()) != folder)
        return false;
    return path.size() == folder.size() || folder.back() == '/' || path[folder.size()] == '/';
}

std::string canonical_path(const ProtectionRules &rules, std::string_view path)
{
    path = trimmed(path);
    for (const PathAlias &alias : rules.aliases)
        if (path_within(path, alias.from))
            return alias.to + std::string(path.substr(alias.from.size()));
    return std::string(path);
}

std::vector<std::string> path_spellings(const ProtectionRules &rules, std::string_view canonical)
{
    std::vector<std::string> out = {std::string(canonical)};
    for (const PathAlias &alias : rules.aliases)
        if (path_within(canonical, alias.to))
            out.push_back(alias.from + std::string(canonical.substr(alias.to.size())));
    return out;
}

Check check_path(const ProtectionRules &rules, std::string_view raw)
{
    const std::string path = canonical_path(rules, raw);
    for (const std::string &root : rules.volume_roots)
        if (path == canonical_path(rules, root))
            return refuse(Verdict::volume_root, "A whole drive can't be collected.");
    // A path the system would read differently from these rules: "..", ".",
    // empty components ("a//b" is "a/b" to the kernel) or a NUL inside.
    if (path.empty() || path.front() != '/' || path.find("/../") != std::string::npos ||
        path.find("/./") != std::string::npos || path.ends_with("/..") || path.ends_with("/.") ||
        path.find("//") != std::string::npos || path.find('\0') != std::string::npos)
        return refuse(Verdict::protected_path, "That path can't be collected.");

    for (const ProtectedRoot &root : rules.protected_roots)
        if (matches(path, root.path))
            return refuse(Verdict::protected_path, root.reason);
    for (const ProtectedRoot &root : rules.protected_roots)
        if (contains(path, root.path))
            return refuse(Verdict::contains_protected,
                          "It holds files that must stay, so it can't be collected whole.");

    const std::string_view name = name_of(path);
    const std::string_view parent = parent_of(path);
    for (const std::string &folder : rules.title_parents)
    {
        if (path_within(folder, path))
            return refuse(Verdict::contains_titles,
                          "It holds installed games: collect them one at a time.");
        if (!path_within(path, folder))
            continue;
        if (parent != folder)
            return refuse(Verdict::title_part,
                          "Part of an installed game: collect the game to uninstall it.");
        const bool playden = playden_title(name);
        if (!retail_title(name) && !playden)
            return refuse(Verdict::title_part, "Uninstall this from the home screen.");
        if (std::find(rules.shadowmount_refused.begin(), rules.shadowmount_refused.end(), name) !=
            rules.shadowmount_refused.end())
            return refuse(Verdict::title_part, kUnclearSource);
        const auto source = rules.shadowmount_sources.find(std::string(name));
        if (source != rules.shadowmount_sources.end())
        {
            if (const ProtectedRoot *root = protecting(rules, source->second))
                return refuse(Verdict::protected_path, root->reason);
            return uninstall(name, {source->second});
        }
        // A Playden title is installed from its folder in /data/homebrew.
        if (playden && !rules.homebrew.empty())
            return uninstall(name, {rules.homebrew + "/" + std::string(name)});
        if (playden)
            return refuse(Verdict::title_part, "Uninstall this from the home screen.");
        return uninstall(name, {});
    }
    for (const std::string &folder : rules.title_companions)
    {
        if (path_within(folder, path))
            return refuse(Verdict::contains_titles,
                          "It holds parts of installed games: collect the games instead.");
        if (path_within(path, folder))
            return refuse(Verdict::title_part,
                          "It goes with its game: collect the game to uninstall both.");
    }

    // A folder ShadowMount installs a title from: the title goes with it.
    for (const std::string &refused : rules.shadowmount_refused)
        if (refused.front() == '/' && (path_within(path, refused) || path_within(refused, path)))
            return refuse(Verdict::title_part, kUnclearSource);
    for (const auto &[id, source] : rules.shadowmount_sources)
    {
        if (path == source)
        {
            if (!retail_title(id) && !playden_title(id))
                return refuse(Verdict::title_part, "Uninstall this from the home screen.");
            return uninstall(id, {source});
        }
        if (path_within(path, source))
            return refuse(Verdict::title_part,
                          "Part of an installed game: collect the game to uninstall it.");
        if (path_within(source, path))
            return refuse(Verdict::contains_titles,
                          "It holds installed games: collect them one at a time.");
    }

    // Playden: a game's title, or its game data, uninstalls the title.
    if (!rules.homebrew.empty() && path_within(path, rules.homebrew) && path != rules.homebrew)
    {
        const std::string_view id = child_below(path, rules.homebrew);
        if (playden_title(id))
        {
            if (parent != rules.homebrew)
                return refuse(Verdict::title_part,
                              "Part of a Playden game's title: collect the title or the game.");
            return uninstall(id, {path});
        }
    }
    for (const std::string &games : rules.playden_game_folders)
    {
        if (path_within(games, path))
            return refuse(Verdict::contains_titles,
                          "It holds Playden games: collect them one at a time.");
        if (!path_within(path, games) || path == games)
            continue;
        // The game folder is the first name below the games folder.
        const std::string_view game = child_below(path, games);
        const std::string install_id = playden_install_id(games, game);
        if (games.ends_with("/playden-games") &&
            std::find(rules.playden_copies.begin(), rules.playden_copies.end(), install_id) !=
                rules.playden_copies.end())
            return refuse(Verdict::title_part,
                          "This game's files are on more than one drive: uninstall it in Playden.");
        const auto it = rules.playden_titles.find(install_id);
        if (it == rules.playden_titles.end())
        {
            // No title (removed first, or never set up): plain files, but
            // Playden still lists the game, so the whole game folder asks it
            // to forget the install too.
            Check c;
            if (parent == games && !install_id.empty() && games.ends_with("/playden-games"))
                c.playden_game = playden_request_game(install_id);
            return c;
        }
        if (!playden_title(it->second))
            return refuse(Verdict::title_part, "Uninstall this game from Playden.");
        if (parent != games)
            return refuse(Verdict::title_part,
                          "Part of a Playden game: collect the game to uninstall it.");
        Check c;
        c.verdict = Verdict::title;
        c.title_id = it->second;
        if (!rules.homebrew.empty())
            c.then_delete.push_back(rules.homebrew + "/" + it->second);
        c.then_delete.push_back(path);
        // The game's files (not its bottle, which keeps its saves): Playden
        // is asked to forget the install, or it waits for a drive.
        if (games.ends_with("/playden-games"))
            c.playden_game = playden_request_game(it->first);
        return c;
    }
    return Check{};
}

bool Collection::add(CollectedItem item)
{
    if (covering(item.key) != nullptr)
        return false;
    std::erase_if(items_, [&](const CollectedItem &i) { return path_within(i.key, item.key); });
    items_.push_back(std::move(item));
    return true;
}

bool Collection::remove(std::string_view key)
{
    return std::erase_if(items_, [&](const CollectedItem &i) { return i.key == key; }) != 0;
}

const CollectedItem *Collection::covering(std::string_view key) const
{
    for (const CollectedItem &i : items_)
        if (path_within(key, i.key))
            return &i;
    return nullptr;
}

const CollectedItem *Collection::find(std::string_view key) const
{
    for (const CollectedItem &i : items_)
        if (i.key == key)
            return &i;
    return nullptr;
}

std::uint64_t Collection::total() const
{
    std::uint64_t sum = 0;
    for (const CollectedItem &i : items_)
    {
        sum += i.size;
        for (const DriveTotal &other : i.also)
            sum += other.size;
    }
    return sum;
}

bool Collection::size_unknown() const
{
    return std::any_of(items_.begin(), items_.end(),
                       [](const CollectedItem &i) { return i.size_unknown; });
}

std::vector<DriveTotal> Collection::drives() const
{
    std::vector<DriveTotal> out;
    const auto add = [&](const std::string &drive, const std::string &name, std::uint64_t size,
                         std::size_t count)
    {
        auto it = std::find_if(out.begin(), out.end(),
                               [&](const DriveTotal &d) { return d.drive == drive; });
        if (it == out.end())
        {
            out.push_back({drive, name, 0, 0});
            it = out.end() - 1;
        }
        it->size += size;
        it->count += count;
    };
    for (const CollectedItem &i : items_)
    {
        add(i.drive, i.drive_name, i.size, 1);
        for (const DriveTotal &other : i.also)
            add(other.drive, other.name, other.size, 0);
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const DriveTotal &a, const DriveTotal &b) { return a.size > b.size; });
    return out;
}

std::vector<const CollectedItem *> Collection::sorted() const
{
    const std::vector<DriveTotal> order = drives();
    std::vector<const CollectedItem *> out;
    for (const CollectedItem &i : items_)
        out.push_back(&i);
    const auto rank = [&](const CollectedItem *i)
    {
        for (std::size_t d = 0; d < order.size(); ++d)
            if (order[d].drive == i->drive)
                return d;
        return order.size();
    };
    std::stable_sort(out.begin(), out.end(),
                     [&](const CollectedItem *a, const CollectedItem *b)
                     {
                         const std::size_t ra = rank(a);
                         const std::size_t rb = rank(b);
                         return ra != rb ? ra < rb : a->size > b->size;
                     });
    return out;
}

void Collection::retain(const std::function<bool(CollectedItem &)> &keep)
{
    std::erase_if(items_, [&](CollectedItem &i) { return !keep(i); });
}

} // namespace hui::disk
