// Prospero Disk - The deleter: removes the collected items on a worker thread, bottom-up.
// Copyright (C) 2026 Leonardo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later

#include "disk/deleter.hpp"

#include "disk/scanner.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace hui::disk
{

namespace
{

bool permission(int error)
{
    return error == EACCES || error == EPERM;
}

// The volume root that holds path, the longest that does.
std::string root_of(const ProtectionRules &rules, const std::string &path)
{
    std::string best;
    for (const std::string &root : rules.volume_roots)
        if (path_within(path, root) && root.size() > best.size())
            best = root;
    return best;
}

std::string parent_of(const std::string &path)
{
    const std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos)
        return {};
    return slash == 0 ? std::string("/") : path.substr(0, slash);
}

std::string child_path(const std::string &folder, const std::string &name)
{
    return folder.empty() || folder.back() == '/' ? folder + name : folder + "/" + name;
}

} // namespace

Deleter::~Deleter()
{
    abort_ = true;
    stop_ = true;
    join();
}

bool Deleter::start(const FileSystem &fs, DeleteOptions options)
{
    if (running_ || thread_.joinable())
        return false;
    fs_ = &fs;
    options_ = std::move(options);
    items_ = options_.jobs.size();
    std::uint64_t planned = 0;
    for (const DeleteJob &job : options_.jobs)
        planned += job.item.size;
    planned_ = planned;
    done_ = 0;
    item_ = 0;
    stop_ = false;
    abort_ = false;
    finished_ = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        results_.clear();
        current_.clear();
    }
    running_ = true;
    thread_ = std::thread([this] { run(); });
    return true;
}

void Deleter::stop()
{
    stop_ = true;
}

void Deleter::join()
{
    if (thread_.joinable())
        thread_.join();
}

DeleteProgress Deleter::progress() const
{
    DeleteProgress p;
    p.planned = planned_;
    p.done = done_;
    p.item = item_;
    p.items = items_;
    p.running = running_;
    p.finished = finished_;
    p.stopping = stop_;
    std::lock_guard<std::mutex> lock(mutex_);
    p.current = current_;
    return p;
}

std::vector<ItemResult> Deleter::take()
{
    if (!finished_)
        return {};
    join();
    std::lock_guard<std::mutex> lock(mutex_);
    return std::move(results_);
}

void Deleter::set_current(const std::string &text)
{
    std::lock_guard<std::mutex> lock(mutex_);
    current_ = text;
}

void Deleter::run()
{
    for (std::size_t i = 0; i < options_.jobs.size(); ++i)
    {
        const DeleteJob &job = options_.jobs[i];
        ItemResult result;
        result.item = job.item;
        result.volume_root = job.volume_root;
        if (stop_)
            result.outcome = Outcome::skipped;
        else
        {
            item_ = i;
            delete_item(job, result);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        results_.push_back(std::move(result));
    }
    set_current({});
    finished_ = true;
    running_ = false;
}

void Deleter::fail(ItemResult &result, const std::string &path, int error)
{
    ++result.failures;
    if (result.error == 0)
    {
        result.error = error;
        result.error_path = path;
    }
    if (options_.log)
        options_.log("delete: " + path + ": " + std::strerror(error));
}

// Each call is tried as the app's user, then once with the euid raised: the
// folder's owner decides, and on a PS5 that is root for users' folders and
// the app's user for Playden's.
bool Deleter::stat(const std::string &path, Entry *out, int *error) const
{
    if (fs_->stat(path, out))
        return true;
    *error = errno;
    if (!permission(*error) || !options_.privileged)
        return false;
    bool ok = false;
    options_.privileged(
        [&]
        {
            ok = fs_->stat(path, out);
            if (!ok)
                *error = errno;
            return ok;
        });
    return ok;
}

bool Deleter::list(const std::string &path, std::vector<Entry> *out, int *error) const
{
    if (fs_->list(path, out, error))
        return true;
    if (!permission(*error) || !options_.privileged)
        return false;
    bool ok = false;
    options_.privileged(
        [&]
        {
            ok = fs_->list(path, out, error);
            return ok;
        });
    return ok;
}

bool Deleter::remove(const std::string &path, bool folder, int *error) const
{
    const auto call = [&]
    { return folder ? fs_->remove_folder(path, error) : fs_->remove_file(path, error); };
    if (call())
        return true;
    if (!permission(*error) || !options_.privileged)
        return false;
    bool ok = false;
    options_.privileged(
        [&]
        {
            ok = call();
            return ok;
        });
    return ok;
}

bool Deleter::move(const std::string &from, const std::string &to, int *error) const
{
    if (fs_->move(from, to, error))
        return true;
    if (!permission(*error) || !options_.privileged)
        return false;
    bool ok = false;
    options_.privileged(
        [&]
        {
            ok = fs_->move(from, to, error);
            return ok;
        });
    return ok;
}

bool Deleter::checked_ancestors(const std::string &path, const std::string &volume_root,
                                ItemResult &result)
{
    if (volume_root.empty() || !path_within(path, volume_root) || path == volume_root)
    {
        fail(result, path, EPERM);
        return false;
    }
    // From the volume root down to the item's parent: real folders, no links.
    std::string at = volume_root;
    std::string rest = path.substr(volume_root.size());
    for (;;)
    {
        Entry e;
        int error = 0;
        if (!stat(at, &e, &error))
        {
            fail(result, at, error);
            return false;
        }
        if (e.type != EntryType::directory)
        {
            fail(result, at, ENOTDIR);
            return false;
        }
        while (!rest.empty() && rest.front() == '/')
            rest.erase(0, 1);
        const std::size_t slash = rest.find('/');
        if (slash == std::string::npos)
            return true; // what is left is the item's own name
        at = child_path(at, rest.substr(0, slash));
        rest.erase(0, slash);
    }
}

bool Deleter::remove_tree(const std::string &path, const std::string &volume_root,
                          ItemResult &result)
{
    (void)volume_root;
    Entry top;
    int error = 0;
    if (!stat(path, &top, &error))
    {
        if (error == ENOENT)
            return true; // already gone
        fail(result, path, error);
        return false;
    }
    Entry parent;
    if (!stat(parent_of(path), &parent, &error))
    {
        fail(result, parent_of(path), error);
        return false;
    }
    if (top.device != parent.device)
    {
        fail(result, path, EXDEV); // a mount point: another drive, never ours to empty
        return false;
    }
    if (top.type != EntryType::directory)
    {
        set_current(path);
        if (!options_.dry_run && !remove(path, false, &error))
        {
            fail(result, path, error);
            return false;
        }
        result.bytes += top.disk;
        done_ += top.disk;
        return true;
    }

    struct Frame
    {
        std::string path;
        Entry entry;
        std::size_t parent = 0; // its folder's place in the stack (the top has none)
        bool listed = false;
        bool blocked = false; // something inside stays, so the folder does too
    };
    // An explicit stack: folders nest deeply and worker stacks are small.
    std::vector<Frame> stack;
    stack.push_back({path, top, 0});
    std::vector<Entry> entries;
    while (!stack.empty() && !abort_)
    {
        if (!stack.back().listed)
        {
            stack.back().listed = true;
            const std::string folder = stack.back().path;
            const std::uint64_t device = stack.back().entry.device;
            const std::size_t self = stack.size() - 1;
            set_current(folder);
            if (!list(folder, &entries, &error))
            {
                fail(result, folder, error);
                stack.back().blocked = true;
                continue;
            }
            std::vector<Frame> inner;
            bool blocked = false;
            for (const Entry &e : entries)
            {
                const std::string child = child_path(folder, e.name);
                if (e.type == EntryType::directory)
                {
                    if (e.device != device)
                    {
                        fail(result, child, EXDEV); // a drive mounted inside: left alone
                        blocked = true;
                    }
                    else
                        inner.push_back({child, e, self});
                    continue;
                }
                // lstat again just before removing: the entry must still be
                // the one listed, and not a folder now.
                Entry now;
                if (!stat(child, &now, &error))
                {
                    if (error != ENOENT)
                    {
                        fail(result, child, error);
                        blocked = true;
                    }
                    continue;
                }
                if (now.device != e.device || now.inode != e.inode ||
                    now.type == EntryType::directory)
                {
                    fail(result, child, ESTALE);
                    blocked = true;
                    continue;
                }
                if (!options_.dry_run && !remove(child, false, &error))
                {
                    fail(result, child, error);
                    blocked = true;
                    continue;
                }
                result.bytes += now.disk;
                done_ += now.disk;
            }
            stack.back().blocked = stack.back().blocked || blocked;
            for (Frame &f : inner)
                stack.push_back(std::move(f));
            continue;
        }
        // Everything inside has been handled: the folder itself.
        Frame done = std::move(stack.back());
        stack.pop_back();
        bool kept = done.blocked;
        if (!kept)
        {
            Entry now;
            error = 0;
            if (!stat(done.path, &now, &error) || now.type != EntryType::directory ||
                now.device != done.entry.device || now.inode != done.entry.inode)
            {
                fail(result, done.path, error != 0 ? error : ESTALE);
                kept = true;
            }
            else if (!options_.dry_run && !remove(done.path, true, &error))
            {
                fail(result, done.path, error);
                kept = true;
            }
            else
            {
                result.bytes += now.disk;
                done_ += now.disk;
            }
        }
        // The folder that holds it, not the sibling that may be on top now.
        if (kept && !stack.empty())
            stack[done.parent].blocked = true;
    }
    if (abort_)
        return false;
    if (options_.dry_run)
        return result.failures == 0;
    Entry still;
    return !stat(path, &still, &error) && error == ENOENT;
}

void Deleter::delete_item(const DeleteJob &job, ItemResult &result)
{
    const CollectedItem &item = job.item;
    // The rules again: the collection is never trusted to have checked.
    const Check check = check_path(options_.rules, item.path);
    const bool title = item.kind == ItemKind::title;
    if (title ? check.verdict != Verdict::title || check.title_id != item.title_id ||
                    check.then_delete != item.then_delete
              : check.verdict != Verdict::folder)
    {
        result.outcome = Outcome::failed;
        result.reason = check.reason.empty() ? "It may not be deleted." : check.reason;
        fail(result, item.path, EPERM);
        return;
    }
    if (title)
    {
        delete_title(job, result);
        return;
    }
    if (!checked_ancestors(item.path, job.volume_root, result))
    {
        result.outcome = Outcome::failed;
        return;
    }
    const bool gone = remove_tree(item.path, job.volume_root, result);
    result.outcome = gone ? Outcome::removed : result.bytes > 0 ? Outcome::partly : Outcome::failed;
    if (result.outcome != Outcome::partly || options_.dry_run || abort_)
        return;
    // What is left, so the picture shows it: a scan of the folder alone.
    Scanner scanner;
    ScanOptions scan;
    scan.root = item.path;
    scan.threads = 1;
    if (scanner.start(*fs_, scan))
    {
        scanner.join();
        result.left = scanner.take();
    }
}

void Deleter::delete_title(const DeleteJob &job, ItemResult &result)
{
    const CollectedItem &item = job.item;
    set_current("Uninstalling " + item.title_id);
    if (options_.dry_run)
    {
        result.outcome = Outcome::removed;
        return;
    }
    if (options_.remover == nullptr)
    {
        result.outcome = Outcome::failed;
        result.reason = "Uninstalling is not available.";
        return;
    }
    // A Playden title's folder leaves /data/homebrew first, as Playden does:
    // what ShadowMount can no longer find, it cannot register again.
    std::vector<std::string> folders = item.then_delete;
    std::string retired_from;
    std::string retired_to;
    for (std::string &path : folders)
    {
        if (options_.trash_dir.empty() || options_.rules.homebrew.empty() ||
            parent_of(path) != options_.rules.homebrew)
            continue;
        Entry folder;
        int error = 0;
        if (!checked_ancestors(path, root_of(options_.rules, path), result) ||
            !stat(path, &folder, &error) || folder.type != EntryType::directory)
        {
            if (error != 0)
                fail(result, path, error);
            result.outcome = Outcome::failed;
            result.reason = "The title's folder could not be checked; nothing was changed.";
            return;
        }
        const std::string to =
            child_path(options_.trash_dir, item.title_id + "-" + std::to_string(folder.inode));
        if (!move(path, to, &error))
        {
            fail(result, path, error);
            result.outcome = Outcome::failed;
            result.reason = "The title's folder could not be moved aside; nothing was changed.";
            return;
        }
        retired_from = path;
        retired_to = to;
        path = to;
    }
    const UninstallResult u = options_.remover->uninstall(item.title_id);
    if (!u.ok)
    {
        int error = 0;
        if (!retired_to.empty() && !move(retired_to, retired_from, &error))
            fail(result, retired_to, error);
        result.outcome = Outcome::failed;
        result.reason = u.reason;
        if (options_.log)
            options_.log("uninstall " + item.title_id + ": " + u.reason);
        return;
    }
    // The system removed what it owns; then the folders that go with the
    // title (a Playden title's folder, its game data), file by file.
    bool all_gone = true;
    for (const std::string &path : folders)
    {
        const std::string root =
            path == retired_to ? options_.trash_dir : root_of(options_.rules, path);
        all_gone =
            checked_ancestors(path, root, result) && remove_tree(path, root, result) && all_gone;
    }
    done_ += item.size > result.bytes ? item.size - result.bytes : 0;
    result.bytes = std::max(result.bytes, item.size);
    result.outcome = all_gone ? Outcome::removed : Outcome::partly;
    if (!all_gone)
        result.reason = "The title was uninstalled, but some of its files are left.";
}

} // namespace hui::disk
