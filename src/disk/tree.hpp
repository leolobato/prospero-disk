// Prospero Disk - The scanned folder tree: a compact arena of nodes and a string pool.
// Copyright (C) 2026 Leonardo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hui::disk
{

inline constexpr std::uint32_t kNoNode = 0xffffffffu;

enum class Kind : std::uint8_t
{
    directory = 0,
    file = 1,
    // The files of one folder beyond the largest few, rolled into one node so
    // that a volume with millions of files stays small (PRD 7.3).
    other_files = 2,
};

enum NodeFlag : std::uint8_t
{
    kUnreadable = 1, // the folder could not be listed; its size is unknown
    kListed = 2,     // the folder has been listed (its files are counted)
    kRemoved = 4,    // deleted: detached from its parent, dropped by compacted()
};

// One folder, one kept file, or one roll-up of a folder's other files. The
// layout is written to the cache file as is, so it has no padding holes.
struct Node
{
    std::uint64_t disk = 0;     // bytes on disk, st_blocks x 512, whole subtree
    std::uint64_t apparent = 0; // st_size, whole subtree
    std::uint64_t files = 0;    // files in the subtree
    std::int64_t mtime = 0;     // seconds since the epoch, this node only
    std::uint32_t name = 0;     // offset into the string pool
    std::uint32_t name_size = 0;
    std::uint32_t parent = kNoNode;
    std::uint32_t first_child = kNoNode;
    std::uint32_t next_sibling = kNoNode;
    std::uint32_t folders = 0; // folders in the subtree, not counting this one
    Kind kind = Kind::directory;
    std::uint8_t flags = 0;
    std::uint16_t reserved = 0;
    std::uint32_t reserved2 = 0;
};
static_assert(sizeof(Node) == 64, "the cache format depends on the node size");

class Tree
{
  public:
    // The root's name is the scanned path itself ("/user").
    explicit Tree(std::string_view root_path = "/");

    std::uint32_t root() const
    {
        return 0;
    }
    std::size_t size() const
    {
        return nodes_.size();
    }
    const Node &node(std::uint32_t index) const
    {
        return nodes_[index];
    }
    Node &node(std::uint32_t index)
    {
        return nodes_[index];
    }
    std::string_view name(std::uint32_t index) const;

    std::uint32_t add_child(std::uint32_t parent, std::string_view name, Kind kind);
    // Adds to a node and every ancestor: sizes and counts roll up as folders
    // finish, which is what lets the picture grow while the scan runs.
    void add_totals(std::uint32_t index, std::uint64_t disk, std::uint64_t apparent,
                    std::uint64_t files, std::uint32_t folders);

    // The full path of a node: the root path joined with the names below it.
    std::string path(std::uint32_t index) const;
    std::vector<std::uint32_t> children(std::uint32_t index) const;
    int depth(std::uint32_t index) const;
    // The node at a full path ("/user/app/PPSA00001"); kNoNode when the tree
    // has no such node. The root is the tree's own path.
    std::uint32_t find(std::string_view path) const;

    // Detaches a node (never the root) from its parent and subtracts its
    // totals from every ancestor: what a deletion freed leaves the picture at
    // once. The node stays in the arena, flagged kRemoved, until compacted().
    void remove(std::uint32_t index);
    // Puts subtree (a scan of the same folder, its root named by that path) in
    // place of index: what is left of a folder that was only partly deleted.
    // Returns the new node of that folder.
    std::uint32_t replace(std::uint32_t index, const Tree &subtree);
    // A copy without the removed nodes, for the cache file.
    Tree compacted() const;

    // The child of ancestor on the way to index (index itself when it is a
    // child); kNoNode when ancestor is not above index.
    std::uint32_t child_towards(std::uint32_t ancestor, std::uint32_t index) const;

    // Raw access for the cache file.
    const std::vector<Node> &nodes() const
    {
        return nodes_;
    }
    const std::string &pool() const
    {
        return pool_;
    }
    // Takes a node array and pool read from a cache file; false (and the tree
    // unchanged) when they do not describe a well-formed tree.
    bool adopt(std::vector<Node> nodes, std::string pool);

  private:
    std::vector<Node> nodes_;
    std::string pool_;
};

} // namespace hui::disk
