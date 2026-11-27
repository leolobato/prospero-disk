// Prospero Disk - The deleter: removes the collected items on a worker thread, bottom-up.
// Copyright (C) 2026 Leonardo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "disk/collection.hpp"
#include "disk/filesystem.hpp"
#include "disk/tree.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hui::disk
{

// Runs call once with the effective uid raised to root, then restores it;
// false when it cannot be raised. Removing needs the owner of the folder, and
// on a PS5 users' folders are root's while Playden's are the app's own
// (docs/platform/CONSOLE_NOTES.md section 4), so each call is tried as the
// app's user first and once more raised after EACCES or EPERM. Empty: never
// raise (a PC, the tests).
using Privileged = std::function<bool(const std::function<bool()> &call)>;

struct UninstallResult
{
    bool ok = false;       // the system confirmed the title is gone
    bool accepted = false; // the system took the request (it may still be working)
    std::string reason;    // why not, for the result screen
};

// Uninstalls a title through the system (docs/plans/collect-delete.md,
// section 4). Called on the deleter's thread; it may take seconds.
class TitleRemover
{
  public:
    virtual ~TitleRemover() = default;
    virtual UninstallResult uninstall(const std::string &title_id) = 0;
};

struct DeleteJob
{
    CollectedItem item;
    std::string volume_root; // the volume the item was collected from
};

struct DeleteOptions
{
    std::vector<DeleteJob> jobs;
    ProtectionRules rules;           // checked again for every item
    TitleRemover *remover = nullptr; // null: title items fail
    Privileged privileged;
    // Walks and reports, removes nothing.
    bool dry_run = false;
    // Where a title's folder (a Playden title's, or any folder ShadowMount
    // installs a title from) is moved before the title is uninstalled, so
    // ShadowMount cannot register it again (Playden's order). On another
    // drive the trash is <drive>/prospero-disk/trash. Empty: the folder is
    // deleted where it is, after the uninstall.
    std::string trash_dir;
    // Asks Playden to forget a game whose files were deleted ("2610"); false
    // when the request could not be written. Empty: not asked.
    std::function<bool(const std::string &)> forget_playden_game;
    std::function<void(const std::string &)> log; // one line per failure
};

enum class Outcome : std::uint8_t
{
    removed, // gone entirely
    partly,  // some of it is left
    failed,  // nothing was removed
    skipped, // not started: Stop was pressed before it
};

struct ItemResult
{
    CollectedItem item;
    std::string volume_root;
    Outcome outcome = Outcome::failed;
    std::uint64_t bytes = 0; // removed, by the deleter's own count
    int error = 0;           // the first errno (0 when none)
    std::string error_path;  // where it happened
    std::string reason;      // a title's uninstall failure, or a refused path
    std::size_t failures = 0;
    std::size_t removed = 0; // entries removed, whatever their size (an empty file is 0 bytes)
    // Outcome::partly: a scan of what is left of the item's folder.
    std::unique_ptr<Tree> left;
};

struct DeleteProgress
{
    std::uint64_t planned = 0; // the items' sizes as scanned
    std::uint64_t done = 0;
    std::size_t item = 0; // the one being worked on, 0-based
    std::size_t items = 0;
    std::string current; // a path, or "Uninstalling PPSA01467"
    bool running = false;
    bool finished = false;
    bool stopping = false;
};

// True while any Deleter's worker runs: the main loop then writes no file
// (the effective uid may be raised for a moment) and does not quit midway.
bool deletion_running();

// Usage:
//   Deleter deleter;
//   deleter.start(fs, options);
//   each frame: deleter.progress();   circle: deleter.stop();
//   when progress().finished: deleter.join(); for (auto &r : deleter.take()) ...
//
// Per item: the path is checked against the rules again; every folder from
// the volume root down to it must be a real folder, not a link; the walk
// never follows a link and never enters another device (a mount point), and
// lstats each entry again just before removing it.
class Deleter
{
  public:
    Deleter() = default;
    ~Deleter();
    Deleter(const Deleter &) = delete;
    Deleter &operator=(const Deleter &) = delete;

    // fs must outlive the run. False when one is already running.
    bool start(const FileSystem &fs, DeleteOptions options);
    // Stops after the item being worked on.
    void stop();
    void join();
    DeleteProgress progress() const;
    // The results, once finished; empty before.
    std::vector<ItemResult> take();

  private:
    void run();
    void delete_item(const DeleteJob &job, ItemResult &result);
    void delete_title(const DeleteJob &job, const Check &check, ItemResult &result);
    // Where a title's folder waits while its title is uninstalled: the trash
    // on the folder's own drive (a rename cannot cross drives), made if needed.
    std::string trash_for(const std::string &path, int *error) const;
    // Removes path and everything under it; true when it is gone.
    // What the collection said the item was: a folder is never emptied when
    // a file was collected, and the reverse.
    enum class Expect : std::uint8_t
    {
        any,
        folder,
        file,
    };
    bool remove_tree(const std::string &path, const std::string &volume_root, Expect expect,
                     ItemResult &result);
    bool checked_ancestors(const std::string &path, const std::string &volume_root,
                           ItemResult &result);
    bool stat(const std::string &path, Entry *out, int *error) const;
    bool list(const std::string &path, std::vector<Entry> *out, int *error) const;
    bool remove(const std::string &path, bool folder, int *error) const;
    bool move(const std::string &from, const std::string &to, int *error) const;
    bool make_folder(const std::string &path, int *error) const;
    // The mount a folder is on ("" when the file system cannot say).
    std::string mount_of(const std::string &path) const;
    void fail(ItemResult &result, const std::string &path, int error);
    void set_current(const std::string &text);

    const FileSystem *fs_ = nullptr;
    DeleteOptions options_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> abort_{false}; // the screen is going away: stop inside an item too
    std::atomic<bool> running_{false};
    std::atomic<bool> finished_{false};
    std::atomic<std::uint64_t> planned_{0};
    std::atomic<std::uint64_t> done_{0};
    std::atomic<std::size_t> item_{0};
    std::size_t items_ = 0;
    mutable std::mutex mutex_; // current_ and results_
    std::string current_;
    std::vector<ItemResult> results_;
};

} // namespace hui::disk
