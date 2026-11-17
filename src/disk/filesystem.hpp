// Prospero Disk - What the scanner needs from a file system, with a POSIX and a synthetic one.
// Copyright (C) 2026 Leonardo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hui::disk
{

enum class EntryType : std::uint8_t
{
    file,
    directory,
    symlink, // counted as a file of its own size; never followed (PRD F2)
    other,   // sockets, pipes, devices
};

// One directory entry as lstat sees it.
struct Entry
{
    std::string name;
    EntryType type = EntryType::file;
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    std::uint32_t links = 1;
    std::uint64_t disk = 0;     // st_blocks x 512
    std::uint64_t apparent = 0; // st_size
    std::int64_t mtime = 0;
};

struct SpaceInfo
{
    std::uint64_t total = 0;
    std::uint64_t used = 0;
    std::uint64_t free = 0;
};

// Every call may come from several scanner threads at once.
class FileSystem
{
  public:
    virtual ~FileSystem() = default;
    // Fills out with the entries of a directory, without "." and "..".
    // Returns false with an errno value when it cannot be read.
    virtual bool list(const std::string &path, std::vector<Entry> *out, int *error) const = 0;
    // lstat of one path.
    virtual bool stat(const std::string &path, Entry *out) const = 0;
    virtual bool space(const std::string &path, SpaceInfo *out) const = 0;
    virtual bool read_file(const std::string &path, std::string *out,
                           std::size_t max_bytes) const = 0;
};

// The real file system through opendir, fstatat and statvfs.
std::unique_ptr<FileSystem> make_posix_filesystem();

// A made-up volume for the PC preview, the tour and the tests: the same tree
// every time, with invented titles under user/app and the appmeta files that
// name them. It answers slowly enough (delay_us per folder) for a scan to be
// seen growing.
std::unique_ptr<FileSystem> make_fixture_filesystem(std::uint32_t delay_us = 0);

// The fixture's two volumes: an internal drive and a USB drive.
inline constexpr const char *kFixtureRoot = "/fixture/user";
inline constexpr const char *kFixtureUsbRoot = "/fixture/usb0";
inline constexpr std::uint64_t kFixtureTotal = 825ull * 1000 * 1000 * 1000;

} // namespace hui::disk
