// Prospero Disk - A made-up volume for the PC preview, the tour and the tests.
// Copyright (C) 2026 Leonardo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later

#include "disk/filesystem.hpp"

#include "disk/png.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <map>
#include <set>
#include <condition_variable>
#include <mutex>

namespace hui::disk
{

namespace
{

constexpr std::uint64_t kGB = 1000ull * 1000 * 1000;
constexpr std::uint64_t kMB = 1000ull * 1000;
constexpr std::uint64_t kBlock = 4096;

std::uint64_t rounded(std::uint64_t bytes)
{
    return (bytes + kBlock - 1) / kBlock * kBlock;
}

// A 64 x 64 icon: a diagonal gradient of the title's colour with a lighter disc.
std::string fake_icon(std::uint32_t colour)
{
    constexpr int kSize = 64;
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(kSize * kSize * 4));
    const float r = static_cast<float>((colour >> 16) & 0xff);
    const float g = static_cast<float>((colour >> 8) & 0xff);
    const float b = static_cast<float>(colour & 0xff);
    for (int y = 0; y < kSize; ++y)
        for (int x = 0; x < kSize; ++x)
        {
            const float t = static_cast<float>(x + y) / (2.0f * kSize);
            const float dx = static_cast<float>(x) - 40.0f;
            const float dy = static_cast<float>(y) - 24.0f;
            const float lift = dx * dx + dy * dy < 220.0f ? 0.35f : 0.0f;
            const float k = 0.55f + 0.45f * (1.0f - t);
            std::uint8_t *p = &rgba[static_cast<std::size_t>((y * kSize + x) * 4)];
            p[0] = static_cast<std::uint8_t>(r * k + (255.0f - r * k) * lift);
            p[1] = static_cast<std::uint8_t>(g * k + (255.0f - g * k) * lift);
            p[2] = static_cast<std::uint8_t>(b * k + (255.0f - b * k) * lift);
            p[3] = 255;
        }
    return encode_png(kSize, kSize, rgba.data());
}

// Invented titles: no real product names (AGENTS.md). The last four are in
// other scripts, for the text engine (docs/PRD.md 7.5).
struct FakeTitle
{
    const char *id;
    const char *name;
    std::uint64_t size;
    std::uint32_t colour;
};

constexpr FakeTitle kTitles[] = {
    {"PPSA10021", "Starlight Courier", 112 * kGB, 0x3b82f6},
    {"PPSA10388", "Ember Tide", 86 * kGB, 0xef4444},
    {"PPSA11420", "Hollow Lanterns", 64 * kGB, 0xf59e0b},
    {"PPSA12077", "Gravity Orchard", 48 * kGB, 0x22c55e},
    {"PPSA12902", "Paper Kingdoms", 31 * kGB, 0xa855f7},
    {"PPSA13311", "Neon Harbour", 22 * kGB, 0x06b6d4},
    {"PPSA14556", "Quiet Summit", 12 * kGB, 0x84cc16},
    {"PPSA15003", "Tiny Engines", 6 * kGB, 0xf97316},
    {"PPSA15790", "Caf\u00e9 Lumi\u00e8re", 3 * kGB, 0x10b981},
    {"PPSA16234", "\u661f\u306e\u706f\u53f0", 1200 * kMB, 0xec4899},
    {"PPSA17702", "\u0421\u0435\u0432\u0435\u0440\u043d\u044b\u0439 \u0432\u0435\u0442\u0435\u0440",
     700 * kMB, 0x0ea5e9},
    {"PPSA18815", "\u0631\u062d\u0644\u0629 \u0627\u0644\u0635\u062d\u0631\u0627\u0621", 260 * kMB,
     0xeab308},
};

class FixtureFileSystem final : public FileSystem
{
  public:
    explicit FixtureFileSystem(double folders_per_second) : rate_(folders_per_second)
    {
        build();
    }

    void advance(double seconds) const override
    {
        if (rate_ <= 0.0)
            return;
        {
            std::lock_guard<std::mutex> lock(pace_mutex_);
            budget_ += seconds * rate_;
        }
        pace_cv_.notify_all();
    }

    void begin() const override
    {
        std::lock_guard<std::mutex> lock(pace_mutex_);
        budget_ = 0.0;
        released_ = false;
        scanning_ = true;
    }

    void release() const override
    {
        {
            std::lock_guard<std::mutex> lock(pace_mutex_);
            released_ = true;
            scanning_ = false;
        }
        pace_cv_.notify_all();
    }

    // Only a scan is paced: listing the volumes at launch is not.
    bool pacing() const
    {
        std::lock_guard<std::mutex> lock(pace_mutex_);
        return scanning_;
    }

    bool list(const std::string &path, std::vector<Entry> *out, int *error) const override
    {
        if (rate_ > 0.0 && pacing())
        {
            // Wait for the screen's clock. The long real-time limit only
            // guards a caller that neither advances nor releases.
            std::unique_lock<std::mutex> lock(pace_mutex_);
            pace_cv_.wait_for(lock, std::chrono::seconds(10),
                              [this] { return budget_ >= 1.0 || released_; });
            budget_ = budget_ >= 1.0 ? budget_ - 1.0 : 0.0;
        }
        out->clear();
        std::lock_guard<std::mutex> data(data_mutex_);
        if (unreadable_.count(path) != 0)
        {
            *error = EACCES;
            return false;
        }
        const auto it = dirs_.find(path);
        if (it == dirs_.end())
        {
            *error = ENOENT;
            return false;
        }
        *out = it->second;
        return true;
    }

    bool stat(const std::string &path, Entry *out) const override
    {
        std::lock_guard<std::mutex> data(data_mutex_);
        if (path == kFixtureRoot || path == kFixtureUsbRoot)
        {
            Entry e;
            e.name = path == kFixtureRoot ? "user" : "usb0";
            e.type = EntryType::directory;
            e.device = path == kFixtureRoot ? 1 : 3;
            e.inode = 2;
            e.disk = kBlock;
            e.apparent = kBlock;
            *out = e;
            return true;
        }
        const std::size_t slash = path.find_last_of('/');
        const auto it =
            slash == std::string::npos ? dirs_.end() : dirs_.find(path.substr(0, slash));
        if (it != dirs_.end())
            for (const Entry &e : it->second)
                if (e.name == path.substr(slash + 1))
                {
                    *out = e;
                    return true;
                }
        errno = ENOENT;
        return false;
    }

    // Every device is its own mount, named by the folder it starts at.
    bool mount_point(const std::string &path, std::string *out) const override
    {
        Entry e;
        if (!stat(path, &e) || e.type != EntryType::directory)
            return false;
        *out = e.device == 3   ? kFixtureUsbRoot
               : e.device == 2 ? "/fixture/user/mnt_other"
                               : kFixtureRoot;
        return true;
    }

    bool remove_file(const std::string &path, int *error) const override
    {
        std::lock_guard<std::mutex> data(data_mutex_);
        std::vector<Entry> *siblings = nullptr;
        const std::size_t at = find(path, &siblings, error);
        if (at == kMissing)
            return false;
        const Entry gone = (*siblings)[at];
        if (gone.type == EntryType::directory)
        {
            *error = EPERM; // unlink on a folder
            return false;
        }
        siblings->erase(siblings->begin() + static_cast<std::ptrdiff_t>(at));
        contents_.erase(path);
        // A hard link frees nothing while another name is left.
        bool linked = false;
        for (auto &dir : dirs_)
            for (Entry &e : dir.second)
                if (e.device == gone.device && e.inode == gone.inode)
                {
                    e.links = e.links > 1 ? e.links - 1 : 1;
                    linked = true;
                }
        if (!linked)
            release(gone);
        return true;
    }

    bool move(const std::string &from, const std::string &to, int *error) const override
    {
        std::lock_guard<std::mutex> data(data_mutex_);
        std::vector<Entry> *siblings = nullptr;
        const std::size_t at = find(from, &siblings, error);
        if (at == kMissing)
            return false;
        std::vector<Entry> *target_siblings = nullptr;
        int ignored = 0;
        if (find(to, &target_siblings, &ignored) != kMissing)
        {
            *error = EEXIST;
            return false;
        }
        const std::size_t slash = to.find_last_of('/');
        const auto parent = dirs_.find(to.substr(0, slash));
        if (slash == std::string::npos || parent == dirs_.end())
        {
            *error = ENOENT;
            return false;
        }
        Entry moved = (*siblings)[at];
        siblings->erase(siblings->begin() + static_cast<std::ptrdiff_t>(at));
        moved.name = to.substr(slash + 1);
        parent->second.push_back(moved);
        // A folder takes everything below it along.
        std::map<std::string, std::vector<Entry>> renamed;
        for (auto it = dirs_.begin(); it != dirs_.end();)
        {
            if (it->first == from || it->first.rfind(from + "/", 0) == 0)
            {
                renamed[to + it->first.substr(from.size())] = std::move(it->second);
                it = dirs_.erase(it);
            }
            else
                ++it;
        }
        for (auto &moved_dir : renamed)
            dirs_[moved_dir.first] = std::move(moved_dir.second);
        // Every map keyed by path moves too, or stat and read_file disagree.
        const auto rekey = [&](const std::string &key) -> std::string
        {
            if (key == from || key.rfind(from + "/", 0) == 0)
                return to + key.substr(from.size());
            return key;
        };
        std::map<std::string, std::string> contents;
        for (auto &c : contents_)
            contents[rekey(c.first)] = std::move(c.second);
        contents_ = std::move(contents);
        std::set<std::string> unreadable;
        for (const std::string &u : unreadable_)
            unreadable.insert(rekey(u));
        unreadable_ = std::move(unreadable);
        return true;
    }

    bool remove_folder(const std::string &path, int *error) const override
    {
        std::lock_guard<std::mutex> data(data_mutex_);
        std::vector<Entry> *siblings = nullptr;
        const std::size_t at = find(path, &siblings, error);
        if (at == kMissing)
            return false;
        if ((*siblings)[at].type != EntryType::directory)
        {
            *error = ENOTDIR;
            return false;
        }
        const auto dir = dirs_.find(path);
        if (dir != dirs_.end() && !dir->second.empty())
        {
            *error = ENOTEMPTY;
            return false;
        }
        release((*siblings)[at]);
        siblings->erase(siblings->begin() + static_cast<std::ptrdiff_t>(at));
        if (dir != dirs_.end())
            dirs_.erase(dir);
        return true;
    }

    bool space(const std::string &path, SpaceInfo *out) const override
    {
        std::lock_guard<std::mutex> data(data_mutex_);
        if (path.rfind(kFixtureUsbRoot, 0) == 0)
        {
            out->total = 256 * kGB;
            out->used = usb_used_;
            out->free = out->total - out->used;
            return true;
        }
        if (path.rfind(kFixtureRoot, 0) != 0)
            return false;
        out->total = kFixtureTotal;
        // More used than the scan can see: the hidden system share (PRD 5.3).
        out->used = internal_used_;
        out->free = out->total - out->used;
        return true;
    }

    bool read_file(const std::string &path, std::string *out, std::size_t max_bytes) const override
    {
        std::lock_guard<std::mutex> data(data_mutex_);
        const auto it = contents_.find(path);
        if (it == contents_.end() || it->second.size() > max_bytes)
            return false;
        *out = it->second;
        return true;
    }

  private:
    static constexpr std::size_t kMissing = static_cast<std::size_t>(-1);

    // The entry for path in its parent's list; kMissing with an errno value.
    // A folder that cannot be read refuses changes inside it (EACCES), and so
    // does a volume root (EBUSY). Called with data_mutex_ held.
    std::size_t find(const std::string &path, std::vector<Entry> **siblings, int *error) const
    {
        const std::size_t slash = path.find_last_of('/');
        if (path == kFixtureRoot || path == kFixtureUsbRoot || slash == std::string::npos)
        {
            *error = EBUSY;
            return kMissing;
        }
        const std::string parent = path.substr(0, slash);
        if (unreadable_.count(parent) != 0 || unreadable_.count(path) != 0)
        {
            *error = EACCES;
            return kMissing;
        }
        const auto it = dirs_.find(parent);
        if (it != dirs_.end())
            for (std::size_t i = 0; i < it->second.size(); ++i)
                if (it->second[i].name == path.substr(slash + 1))
                {
                    *siblings = &it->second;
                    return i;
                }
        *error = ENOENT;
        return kMissing;
    }

    // The space an entry used is free again on its drive.
    void release(const Entry &e) const
    {
        std::uint64_t &used = e.device == 3 ? usb_used_ : internal_used_;
        used = used > e.disk ? used - e.disk : 0;
    }

    Entry dir_entry(const char *name, std::uint64_t device = 1)
    {
        Entry e;
        e.name = name;
        e.type = EntryType::directory;
        e.device = device;
        e.inode = ++inode_;
        e.disk = kBlock;
        e.apparent = kBlock;
        e.mtime = kNow - static_cast<std::int64_t>(inode_ * 3607 % 2000000);
        return e;
    }

    Entry file_entry(const std::string &name, std::uint64_t bytes)
    {
        Entry e;
        e.name = name;
        e.type = EntryType::file;
        e.device = 1;
        e.inode = ++inode_;
        e.disk = rounded(bytes);
        e.apparent = bytes;
        e.mtime = kNow - static_cast<std::int64_t>(inode_ * 7919 % 9000000);
        return e;
    }

    // Adds a folder under parent (a full path) and returns its full path.
    std::string folder(const std::string &parent, const char *name, std::uint64_t device = 1)
    {
        dirs_[parent].push_back(dir_entry(name, device));
        std::string path = parent + "/" + name;
        dirs_[path]; // exists, possibly empty
        return path;
    }

    void file(const std::string &parent, const std::string &name, std::uint64_t bytes)
    {
        dirs_[parent].push_back(file_entry(name, bytes));
    }

    void content(const std::string &parent, const std::string &name, std::string bytes)
    {
        Entry e = file_entry(name, bytes.size());
        dirs_[parent].push_back(e);
        contents_[parent + "/" + name] = std::move(bytes);
    }

    std::uint64_t next_random()
    {
        seed_ = seed_ * 6364136223846793005ull + 1442695040888963407ull;
        return seed_ >> 33;
    }

    void build()
    {
        const std::string root = kFixtureRoot;
        dirs_[root];

        const std::string app = folder(root, "app");
        const std::string patch = folder(root, "patch");
        const std::string appmeta = folder(root, "appmeta");
        for (const FakeTitle &t : kTitles)
        {
            const std::string dir = folder(app, t.id);
            file(dir, "app.pkg", t.size * 97 / 100);
            file(dir, "app.json", 2 * 1024);
            const std::string sce = folder(dir, "sce_sys");
            file(sce, "param.json", 6 * 1024);
            file(sce, "icon0.png", 300 * 1024);
            const std::string data = folder(dir, "data");
            for (int i = 0; i < 6; ++i)
            {
                char name[32];
                std::snprintf(name, sizeof name, "chunk%02d.bin", i);
                file(data, name, t.size / 100 / 6);
            }
            const std::string meta = folder(appmeta, t.id);
            char json[256];
            std::snprintf(
                json, sizeof json,
                "{\"titleId\":\"%s\",\"localizedParameters\":{\"defaultLanguage\":\"en-US\","
                "\"en-US\":{\"titleName\":\"%s\"}}}",
                t.id, t.name);
            content(meta, "param.json", json);
            content(meta, "icon0.png", fake_icon(t.colour));
        }
        // Patches for the bigger titles.
        for (int i = 0; i < 4; ++i)
        {
            const std::string dir = folder(patch, kTitles[i].id);
            file(dir, "patch.pkg", kTitles[i].size / (6 + i));
        }

        const std::string home = folder(root, "home");
        const std::string player = folder(home, "10000000");
        const std::string captures = folder(player, "captures");
        for (int i = 0; i < 40; ++i)
        {
            char name[32];
            std::snprintf(name, sizeof name, "clip_%03d.mp4", i);
            file(captures, name, (40 + next_random() % 900) * kMB);
        }
        const std::string saves = folder(player, "savedata");
        for (const FakeTitle &t : kTitles)
            file(folder(saves, t.id), "sdimg_save", (2 + next_random() % 60) * kMB);

        // Many small files: most roll up into one "(other files)" node.
        const std::string trophy = folder(player, "trophy");
        for (int i = 0; i < 400; ++i)
        {
            char name[32];
            std::snprintf(name, sizeof name, "t%04d.dat", i);
            file(trophy, name, (4 + next_random() % 60) * 1024);
        }

        const std::string download = folder(root, "download");
        file(download, "update_cache.bin", 9 * kGB);
        file(download, "partial_3.tmp", 2400 * kMB);

        // A hard link: two names, one file, counted once (PRD F4).
        Entry first = file_entry("shared.pak", 1500 * kMB);
        first.links = 2;
        Entry second = first;
        second.name = "shared_link.pak";
        const std::string shared = folder(root, "shared");
        dirs_[shared].push_back(first);
        dirs_[shared].push_back(second);

        // A symbolic link counts its own few bytes and is not followed.
        Entry link = file_entry("latest", 0);
        link.type = EntryType::symlink;
        link.apparent = 24;
        link.disk = 0;
        dirs_[download].push_back(link);

        // A folder that cannot be read (PRD F6) and another device (F3).
        unreadable_.insert(folder(root, "system_tmp"));
        file(folder(folder(root, "mnt_other", 2), "huge"), "elsewhere.bin", 400 * kGB);

        // The USB drive: a library of game dumps and some emulator files.
        const std::string usb = kFixtureUsbRoot;
        dirs_[usb];
        const std::string dumps = folder(usb, "dumps", 3);
        for (int i = 6; i < 10; ++i)
        {
            const std::string dir = folder(dumps, kTitles[i].id, 3);
            usb_file(dir, "eboot.bin", 40 * kMB);
            usb_file(dir, "data.pak", kTitles[i].size);
        }
        const std::string roms = folder(usb, "roms", 3);
        for (const char *system : {"arcade", "handheld", "16bit"})
        {
            const std::string dir = folder(roms, system, 3);
            for (int i = 0; i < 120; ++i)
            {
                char name[32];
                std::snprintf(name, sizeof name, "game_%03d.zip", i);
                usb_file(dir, name, (1 + next_random() % 40) * kMB);
            }
        }
        usb_file(usb, "backup.tar", 14 * kGB);
    }

    void usb_file(const std::string &parent, const std::string &name, std::uint64_t bytes)
    {
        Entry e = file_entry(name, bytes);
        e.device = 3;
        dirs_[parent].push_back(e);
        usb_used_ += e.disk;
    }

    static constexpr std::int64_t kNow = 1794931200; // 2026-11-17 16:00 UTC
    double rate_;
    mutable std::mutex pace_mutex_;
    mutable std::condition_variable pace_cv_;
    mutable double budget_ = 0.0;
    mutable bool released_ = false;
    mutable bool scanning_ = false;
    std::uint64_t inode_ = 100;
    std::uint64_t seed_ = 0x5eed;
    // The tree is read by scanner and title threads and changed by removals.
    mutable std::mutex data_mutex_;
    mutable std::map<std::string, std::vector<Entry>> dirs_;
    mutable std::map<std::string, std::string> contents_;
    mutable std::set<std::string> unreadable_;
    mutable std::uint64_t usb_used_ = 1ull << 30; // the file system's own share
    mutable std::uint64_t internal_used_ = 602 * kGB;
};

} // namespace

std::unique_ptr<FileSystem> make_fixture_filesystem(double folders_per_second)
{
    return std::make_unique<FixtureFileSystem>(folders_per_second);
}

} // namespace hui::disk
