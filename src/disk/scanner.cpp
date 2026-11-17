// Prospero Disk - A parallel, read-only walk of one volume into a Tree.
// Copyright (C) 2026 Leonardo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The diskus model, in C++: a pool of threads takes folders from one shared
// queue, lists each with lstat (never following links), counts hard-linked
// files once per (device, inode), and does not leave the root's device. A
// folder's result goes into the tree under one short lock, and its sizes roll
// up to every ancestor at once, so the picture is correct at every moment for
// what has been read so far.

#include "disk/scanner.hpp"

#include <algorithm>
#include <chrono>

namespace hui::disk
{

std::int64_t now_us()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

namespace
{

std::string join_path(const std::string &parent, const std::string &name)
{
    if (!parent.empty() && parent.back() == '/')
        return parent + name;
    return parent + "/" + name;
}

} // namespace

Scanner::~Scanner()
{
    cancel();
    join();
}

bool Scanner::start(const FileSystem &fs, ScanOptions options)
{
    cancel();
    join();

    Entry root;
    if (!fs.stat(options.root, &root) || root.type != EntryType::directory)
        return false;

    fs_ = &fs;
    fs.begin();
    options_ = std::move(options);
    options_.threads = std::clamp(options_.threads, 1, 32);
    root_device_ = root.device;
    for (Shard &shard : shards_)
        shard.seen.clear();
    {
        std::lock_guard<std::mutex> lock(tree_mutex_);
        tree_ = std::make_unique<Tree>(options_.root);
        tree_->node(0).mtime = root.mtime;
        tree_->add_totals(0, root.disk, root.apparent, 0, 0);
        unreadable_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        queue_.clear();
        queue_.push_back({0, options_.root});
        busy_ = 0;
    }
    cancel_ = false;
    finished_ = false;
    files_ = 0;
    folders_ = 0;
    bytes_ = root.disk;
    unreadable_count_ = 0;
    started_us_ = now_us();
    ended_us_ = 0;
    running_ = true;
    live_ = options_.threads;
    for (int i = 0; i < options_.threads; ++i)
        threads_.emplace_back([this] { worker(); });
    return true;
}

void Scanner::cancel()
{
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        cancel_ = true;
    }
    queue_cv_.notify_all();
    if (fs_ != nullptr)
        fs_->release();
}

void Scanner::join()
{
    for (std::thread &t : threads_)
        if (t.joinable())
            t.join();
    threads_.clear();
}

void Scanner::worker()
{
    std::vector<Entry> entries;
    for (;;)
    {
        Job job;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] { return cancel_ || !queue_.empty() || busy_ == 0; });
            if (cancel_ || queue_.empty())
                break; // cancelled, or nothing queued and nobody can add more
            job = std::move(queue_.front());
            queue_.pop_front();
            ++busy_;
        }
        visit(job, entries);
        bool drained = false;
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            --busy_;
            drained = busy_ == 0 && queue_.empty();
        }
        // Waking every worker per folder costs more than the folder itself
        // on a warm cache: only the end of the walk wakes them all.
        if (drained)
            queue_cv_.notify_all();
    }
    queue_cv_.notify_all();
    if (live_.fetch_sub(1) == 1)
    {
        ended_us_ = now_us();
        finished_ = !cancel_;
        running_ = false;
    }
}

bool Scanner::excluded(const std::string &path) const
{
    for (const std::string &ex : options_.excluded)
        if (path == ex || (path.size() > ex.size() && path.compare(0, ex.size(), ex) == 0 &&
                           path[ex.size()] == '/'))
            return true;
    return false;
}

bool Scanner::first_link(std::uint64_t device, std::uint64_t inode)
{
    Shard &shard = shards_[(inode * 0x9e3779b97f4a7c15ull >> 32) % kShards];
    std::lock_guard<std::mutex> lock(shard.mutex);
    return shard.seen.insert({device, inode}).second;
}

void Scanner::visit(const Job &job, std::vector<Entry> &entries)
{
    {
        std::lock_guard<std::mutex> lock(current_mutex_);
        current_ = job.path;
    }
    int error = 0;
    if (!fs_->list(job.path, &entries, &error))
    {
        std::lock_guard<std::mutex> lock(tree_mutex_);
        tree_->node(job.node).flags |= kUnreadable | kListed;
        unreadable_.push_back(job.path);
        ++unreadable_count_;
        return;
    }

    std::vector<const Entry *> folders;
    std::vector<const Entry *> files;
    std::uint64_t disk = 0;
    std::uint64_t apparent = 0;
    for (const Entry &e : entries)
    {
        if (e.type == EntryType::directory)
        {
            if (options_.same_device && e.device != root_device_)
                continue; // another file system mounted here
            if (excluded(join_path(job.path, e.name)))
                continue;
            folders.push_back(&e);
            disk += e.disk; // the folder's own blocks
            apparent += e.apparent;
            continue;
        }
        if (e.type == EntryType::file && e.links > 1 && !first_link(e.device, e.inode))
            continue; // another name of a file already counted
        files.push_back(&e);
    }

    // Largest first; only the first keep_files become nodes.
    const std::size_t keep = std::min(files.size(), options_.keep_files);
    std::partial_sort(files.begin(), files.begin() + static_cast<std::ptrdiff_t>(keep), files.end(),
                      [](const Entry *a, const Entry *b)
                      { return a->disk != b->disk ? a->disk > b->disk : a->name < b->name; });
    std::uint64_t rest_disk = 0;
    std::uint64_t rest_apparent = 0;
    std::int64_t rest_mtime = 0;
    for (std::size_t i = 0; i < files.size(); ++i)
    {
        disk += files[i]->disk;
        apparent += files[i]->apparent;
        if (i >= keep)
        {
            rest_disk += files[i]->disk;
            rest_apparent += files[i]->apparent;
            rest_mtime = std::max(rest_mtime, files[i]->mtime);
        }
    }

    std::vector<Job> jobs;
    jobs.reserve(folders.size());
    {
        std::lock_guard<std::mutex> lock(tree_mutex_);
        Tree &tree = *tree_;
        for (std::size_t i = 0; i < keep; ++i)
        {
            const std::uint32_t at = tree.add_child(job.node, files[i]->name, Kind::file);
            Node &n = tree.node(at);
            n.disk = files[i]->disk;
            n.apparent = files[i]->apparent;
            n.files = 1;
            n.mtime = files[i]->mtime;
        }
        if (files.size() > keep)
        {
            const std::uint32_t at = tree.add_child(job.node, "(other files)", Kind::other_files);
            Node &n = tree.node(at);
            n.disk = rest_disk;
            n.apparent = rest_apparent;
            n.files = files.size() - keep;
            n.mtime = rest_mtime;
        }
        for (const Entry *f : folders)
        {
            const std::uint32_t at = tree.add_child(job.node, f->name, Kind::directory);
            Node &n = tree.node(at);
            n.disk = f->disk;
            n.apparent = f->apparent;
            n.mtime = f->mtime;
            jobs.push_back({at, join_path(job.path, f->name)});
        }
        tree.add_totals(job.node, disk, apparent, files.size(),
                        static_cast<std::uint32_t>(folders.size()));
        tree.node(job.node).flags |= kListed;
    }
    files_ += files.size();
    folders_ += folders.size();
    bytes_ += disk;

    if (!jobs.empty())
    {
        const std::size_t added = jobs.size();
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            for (Job &j : jobs)
                queue_.push_back(std::move(j));
        }
        // This thread takes one of them itself.
        for (std::size_t i = 1; i < added && i < static_cast<std::size_t>(options_.threads); ++i)
            queue_cv_.notify_one();
    }
}

ScanProgress Scanner::progress() const
{
    ScanProgress p;
    p.files = files_;
    p.folders = folders_;
    p.bytes = bytes_;
    p.unreadable = unreadable_count_;
    p.running = running_;
    p.finished = finished_;
    p.cancelled = !running_ && !finished_ && started_us_ != 0;
    p.seconds = duration();
    {
        std::lock_guard<std::mutex> lock(current_mutex_);
        p.current = current_;
    }
    return p;
}

double Scanner::duration() const
{
    const std::int64_t start = started_us_;
    if (start == 0)
        return 0.0;
    const std::int64_t end = running_ ? now_us() : ended_us_.load();
    return static_cast<double>(end - start) / 1e6;
}

std::unique_ptr<Tree> Scanner::take()
{
    if (!finished_)
        return nullptr;
    join();
    std::lock_guard<std::mutex> lock(tree_mutex_);
    return std::move(tree_);
}

std::vector<std::string> Scanner::unreadable_paths() const
{
    std::lock_guard<std::mutex> lock(tree_mutex_);
    return unreadable_;
}

} // namespace hui::disk
