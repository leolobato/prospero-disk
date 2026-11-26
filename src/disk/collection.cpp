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

Check refuse(Verdict verdict, std::string reason)
{
    Check c;
    c.verdict = verdict;
    c.reason = std::move(reason);
    return c;
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
    if (path.empty() || path.front() != '/' || path.find("/../") != std::string::npos ||
        path.find("/./") != std::string::npos || path.ends_with("/..") || path.ends_with("/."))
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
        if (!retail_title(name))
            return refuse(Verdict::title_part, "Uninstall this from the home screen.");
        Check c;
        c.verdict = Verdict::title;
        c.title_id = std::string(name);
        return c;
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

    // Playden: a game's title, or its game data, uninstalls the title.
    if (!rules.homebrew.empty() && path_within(path, rules.homebrew) && path != rules.homebrew)
    {
        const std::string_view id = child_below(path, rules.homebrew);
        if (playden_title(id))
        {
            if (parent != rules.homebrew)
                return refuse(Verdict::title_part,
                              "Part of a Playden game's title: collect the title or the game.");
            Check c;
            c.verdict = Verdict::title;
            c.title_id = std::string(id);
            c.then_delete = {path};
            return c;
        }
    }
    for (const std::string &games : rules.playden_game_folders)
    {
        if (path_within(games, path))
            return refuse(Verdict::contains_titles,
                          "It holds Playden games: collect them one at a time.");
        if (!path_within(path, games) || parent != games)
            continue;
        const auto it = rules.playden_titles.find(playden_install_id(parent, name));
        if (it == rules.playden_titles.end())
            continue; // no title of its own: plain files
        Check c;
        c.verdict = Verdict::title;
        c.title_id = it->second;
        if (!rules.homebrew.empty())
            c.then_delete.push_back(rules.homebrew + "/" + it->second);
        c.then_delete.push_back(path);
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
        sum += i.size;
    return sum;
}

std::vector<DriveTotal> Collection::drives() const
{
    std::vector<DriveTotal> out;
    for (const CollectedItem &i : items_)
    {
        auto it = std::find_if(out.begin(), out.end(),
                               [&](const DriveTotal &d) { return d.drive == i.drive; });
        if (it == out.end())
        {
            out.push_back({i.drive, i.drive_name, 0, 0});
            it = out.end() - 1;
        }
        it->size += i.size;
        ++it->count;
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
