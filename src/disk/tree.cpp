// Prospero Disk - The scanned folder tree: a compact arena of nodes and a string pool.
// Copyright (C) 2026 Leonardo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later

#include "disk/tree.hpp"

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
