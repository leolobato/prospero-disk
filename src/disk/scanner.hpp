// Prospero Disk - A parallel, read-only walk of one volume into a Tree.
// Copyright (C) 2026 Leonardo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "disk/filesystem.hpp"
#include "disk/tree.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace hui::disk
{

struct ScanOptions
{
    std::string root;
    int threads = 8; // PRD 7.3: 8 on NVMe and M.2, 2 on USB (HDDs thrash)
    // Each folder keeps its largest files as nodes; the rest roll up into one.
    std::size_t keep_files = 32;
    // Never entered, whatever their device (PRD F3).
    std::vector<std::string> excluded = {"/dev", "/mnt/sandbox", "/mnt/sandbox/pfsmnt",
                                         "/mnt/shadowmnt"};
    bool same_device = true;
};

struct ScanProgress
{
    std::uint64_t files = 0;
    std::uint64_t folders = 0;
    std::uint64_t bytes = 0; // on disk
    std::uint64_t unreadable = 0;
    double seconds = 0.0;
    std::string current; // a folder being read now
    bool running = false;
    bool finished = false; // ended on its own, not cancelled
    bool cancelled = false;
};

// Usage:
//   Scanner scanner;
//   scanner.start(fs, options);
//   each frame: scanner.progress(); scanner.try_read([](const Tree &t) { ... });
//   when progress().finished: auto tree = scanner.take();
//
// The UI thread never waits for the walker: try_read gives up when a worker
// holds the tree, and the counters are atomics.
class Scanner
{
  public:
    Scanner() = default;
    ~Scanner();
    Scanner(const Scanner &) = delete;
    Scanner &operator=(const Scanner &) = delete;

    // fs must outlive the scan. Returns false when the root cannot be read.
    bool start(const FileSystem &fs, ScanOptions options);
    void cancel();
    // Waits for the workers to stop (after cancel, or for the scan to end).
    void join();

    ScanProgress progress() const;

    // Calls read(tree) when the tree is free right now; false when it is busy.
    template <typename F> bool try_read(F &&read) const
    {
        std::unique_lock<std::mutex> lock(tree_mutex_, std::try_to_lock);
        if (!lock.owns_lock() || !tree_)
            return false;
        read(static_cast<const Tree &>(*tree_));
        return true;
    }
    // Waits for the tree; for tests and the end of a scan.
    template <typename F> void read(F &&read) const
    {
        std::lock_guard<std::mutex> lock(tree_mutex_);
        if (tree_)
            read(static_cast<const Tree &>(*tree_));
    }

    // The finished tree; null while the scan runs or after a cancel.
    std::unique_ptr<Tree> take();
    std::vector<std::string> unreadable_paths() const;
    double duration() const;

  private:
    struct Job
    {
        std::uint32_t node;
        std::string path;
    };

    void worker();
    void visit(const Job &job, std::vector<Entry> &entries);
    bool first_link(std::uint64_t device, std::uint64_t inode);
    bool excluded(const std::string &path) const;

    const FileSystem *fs_ = nullptr;
    ScanOptions options_;
    std::uint64_t root_device_ = 0;

    mutable std::mutex tree_mutex_;
    std::unique_ptr<Tree> tree_;
    std::vector<std::string> unreadable_;

    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<Job> queue_;
    std::size_t busy_ = 0; // jobs taken but not finished

    static constexpr std::size_t kShards = 16;
    struct Shard
    {
        std::mutex mutex;
        std::set<std::pair<std::uint64_t, std::uint64_t>> seen; // (device, inode)
    };
    Shard shards_[kShards];

    std::vector<std::thread> threads_;
    std::atomic<bool> cancel_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> finished_{false};
    std::atomic<int> live_{0}; // workers still running
    std::atomic<std::uint64_t> files_{0};
    std::atomic<std::uint64_t> folders_{0};
    std::atomic<std::uint64_t> bytes_{0};
    std::atomic<std::uint64_t> unreadable_count_{0};
    std::atomic<std::int64_t> started_us_{0};
    std::atomic<std::int64_t> ended_us_{0};
    mutable std::mutex current_mutex_;
    std::string current_;
};

std::int64_t now_us();

} // namespace hui::disk
