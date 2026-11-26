// Prospero Disk - The scanned folder tree: a compact arena of nodes and a string pool.
// Copyright (C) 2026 Leonardo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later

#include "disk/tree.hpp"

#include <utility>

namespace hui::disk
{

Tree::Tree(std::string_view root_path)
{
    Node root;
    root.name = 0;
    root.name_size = static_cast<std::uint32_t>(root_path.size());
    pool_.assign(root_path);
    nodes_.push_back(root);
}

std::string_view Tree::name(std::uint32_t index) const
{
    const Node &n = nodes_[index];
    return std::string_view(pool_).substr(n.name, n.name_size);
}

std::uint32_t Tree::add_child(std::uint32_t parent, std::string_view name, Kind kind)
{
    Node child;
    child.kind = kind;
    child.name = static_cast<std::uint32_t>(pool_.size());
    child.name_size = static_cast<std::uint32_t>(name.size());
    child.parent = parent;
    child.next_sibling = nodes_[parent].first_child;
    pool_.append(name);
    const auto index = static_cast<std::uint32_t>(nodes_.size());
    nodes_.push_back(child);
    nodes_[parent].first_child = index;
    return index;
}

void Tree::add_totals(std::uint32_t index, std::uint64_t disk, std::uint64_t apparent,
                      std::uint64_t files, std::uint32_t folders)
{
    for (std::uint32_t at = index; at != kNoNode; at = nodes_[at].parent)
    {
        Node &n = nodes_[at];
        n.disk += disk;
        n.apparent += apparent;
        n.files += files;
        n.folders += folders;
    }
}

std::string Tree::path(std::uint32_t index) const
{
    std::vector<std::uint32_t> chain;
    for (std::uint32_t at = index; at != 0 && at != kNoNode; at = nodes_[at].parent)
        chain.push_back(at);
    std::string out(name(0));
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
    {
        if (out.empty() || out.back() != '/')
            out.push_back('/');
        out.append(name(*it));
    }
    return out;
}

std::vector<std::uint32_t> Tree::children(std::uint32_t index) const
{
    std::vector<std::uint32_t> out;
    for (std::uint32_t at = nodes_[index].first_child; at != kNoNode; at = nodes_[at].next_sibling)
        out.push_back(at);
    return out;
}

int Tree::depth(std::uint32_t index) const
{
    int d = 0;
    for (std::uint32_t at = nodes_[index].parent; at != kNoNode; at = nodes_[at].parent)
        ++d;
    return d;
}

std::uint32_t Tree::find(std::string_view path) const
{
    const std::string_view root = name(0);
    if (path.substr(0, root.size()) != root)
        return kNoNode;
    std::string_view rest = path.substr(root.size());
    if (!rest.empty() && rest.front() != '/' && !root.empty() && root.back() != '/')
        return kNoNode; // "/user2" is not under "/user"
    std::uint32_t at = 0;
    while (!rest.empty())
    {
        if (rest.front() == '/')
        {
            rest.remove_prefix(1);
            continue;
        }
        const std::size_t slash = rest.find('/');
        const std::string_view part = rest.substr(0, slash);
        rest = slash == std::string_view::npos ? std::string_view() : rest.substr(slash);
        std::uint32_t next = kNoNode;
        for (std::uint32_t c = nodes_[at].first_child; c != kNoNode; c = nodes_[c].next_sibling)
            if (name(c) == part)
            {
                next = c;
                break;
            }
        if (next == kNoNode)
            return kNoNode;
        at = next;
    }
    return at;
}

void Tree::remove(std::uint32_t index)
{
    if (index == 0 || index >= nodes_.size() || (nodes_[index].flags & kRemoved) != 0)
        return;
    Node &gone = nodes_[index];
    const std::uint32_t parent = gone.parent;
    std::uint32_t *link = &nodes_[parent].first_child;
    while (*link != kNoNode && *link != index)
        link = &nodes_[*link].next_sibling;
    if (*link == index)
        *link = gone.next_sibling;
    gone.next_sibling = kNoNode;
    gone.flags |= kRemoved;
    const std::uint32_t folders = gone.folders + (gone.kind == Kind::directory ? 1u : 0u);
    for (std::uint32_t at = parent; at != kNoNode; at = nodes_[at].parent)
    {
        Node &n = nodes_[at];
        n.disk = n.disk > gone.disk ? n.disk - gone.disk : 0;
        n.apparent = n.apparent > gone.apparent ? n.apparent - gone.apparent : 0;
        n.files = n.files > gone.files ? n.files - gone.files : 0;
        n.folders = n.folders > folders ? n.folders - folders : 0;
    }
}

namespace
{

// Copies the node src of from, and everything under it, as a child of parent
// in to; returns the copy. An explicit stack: folders can nest deeply and
// worker stacks are small.
std::uint32_t copy_subtree(const Tree &from, std::uint32_t src, Tree &to, std::uint32_t parent,
                           std::string_view name)
{
    const auto copy_fields = [](const Node &a, Node &b)
    {
        b.disk = a.disk;
        b.apparent = a.apparent;
        b.files = a.files;
        b.folders = a.folders;
        b.mtime = a.mtime;
        b.flags = a.flags;
    };
    const std::uint32_t top = to.add_child(parent, name, from.node(src).kind);
    copy_fields(from.node(src), to.node(top));
    std::vector<std::pair<std::uint32_t, std::uint32_t>> stack = {{src, top}};
    while (!stack.empty())
    {
        const auto [s, d] = stack.back();
        stack.pop_back();
        for (std::uint32_t c = from.node(s).first_child; c != kNoNode;
             c = from.node(c).next_sibling)
        {
            const std::uint32_t made = to.add_child(d, from.name(c), from.node(c).kind);
            copy_fields(from.node(c), to.node(made));
            stack.emplace_back(c, made);
        }
    }
    return top;
}

} // namespace

std::uint32_t Tree::replace(std::uint32_t index, const Tree &subtree)
{
    if (index == 0 || index >= nodes_.size())
        return kNoNode;
    const std::uint32_t parent = nodes_[index].parent;
    const std::string folder(name(index));
    remove(index);
    const std::uint32_t made = copy_subtree(subtree, 0, *this, parent, folder);
    const Node &n = nodes_[made];
    const std::uint32_t folders = n.folders + (n.kind == Kind::directory ? 1u : 0u);
    add_totals(parent, n.disk, n.apparent, n.files, folders);
    return made;
}

Tree Tree::compacted() const
{
    Tree out(name(0));
    const Node &root = nodes_[0];
    Node &copy = out.nodes_[0];
    copy.disk = root.disk;
    copy.apparent = root.apparent;
    copy.files = root.files;
    copy.folders = root.folders;
    copy.mtime = root.mtime;
    copy.flags = root.flags;
    for (std::uint32_t c = root.first_child; c != kNoNode; c = nodes_[c].next_sibling)
        copy_subtree(*this, c, out, 0, name(c));
    return out;
}

std::uint32_t Tree::child_towards(std::uint32_t ancestor, std::uint32_t index) const
{
    for (std::uint32_t at = index; at != kNoNode; at = nodes_[at].parent)
        if (nodes_[at].parent == ancestor)
            return at;
    return kNoNode;
}

bool Tree::adopt(std::vector<Node> nodes, std::string pool)
{
    if (nodes.empty() || nodes[0].parent != kNoNode)
        return false;
    const std::size_t count = nodes.size();
    for (std::size_t i = 0; i < count; ++i)
    {
        const Node &n = nodes[i];
        if (static_cast<std::uint64_t>(n.name) + n.name_size > pool.size())
            return false;
        if (n.kind > Kind::other_files)
            return false;
        // Links point only to existing nodes, and a parent always comes
        // before its children: that rules out cycles.
        if (i > 0 && (n.parent == kNoNode || n.parent >= i))
            return false;
        // A child is always added after its parent and before its next
        // sibling (add_child prepends), so these orders also rule out loops.
        if (n.first_child != kNoNode && (n.first_child >= count || n.first_child <= i))
            return false;
        if (n.next_sibling != kNoNode && n.next_sibling >= i)
            return false;
    }
    for (std::size_t i = 0; i < count; ++i)
    {
        const Node &n = nodes[i];
        if (n.first_child != kNoNode && nodes[n.first_child].parent != i)
            return false;
        if (n.next_sibling != kNoNode && nodes[n.next_sibling].parent != n.parent)
            return false;
    }
    nodes_ = std::move(nodes);
    pool_ = std::move(pool);
    return true;
}

} // namespace hui::disk
